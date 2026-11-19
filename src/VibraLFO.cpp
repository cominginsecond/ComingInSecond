#include "house_style.hpp"


static const int NUM_WAVES = 5;
enum Wave { TRIANGLE, SINE, SQUARE, RAMP_UP, RAMP_DOWN };
static const char* WAVE_NAMES[NUM_WAVES] = {"Triangle", "Sine", "Square", "Ramp up", "Ramp down"};

// Clock-sync ratios: LFO cycles per clock pulse, chosen with the SPEED knob when CLOCK is patched.
static const int NUM_RATIOS = 7;
static const float RATIOS[NUM_RATIOS] = {1.f / 8, 1.f / 4, 1.f / 2, 1.f, 2.f, 4.f, 8.f};
static const char* RATIO_LABELS[NUM_RATIOS] = {"1/8x", "1/4x", "1/2x", "1x", "2x", "4x", "8x"};

static const float MAX_DELAY = 2.5f;       // delay and rise knobs: 0-2.5 s
static const float MAX_RELEASE = 5.f;      // release knob: 0-5 s
static const float MAX_AMPLITUDE = 2.5f;   // peak volts
static const float NOON_AMPLITUDE = 0.22f; // AMP knob at noon
// SPEED sweeps 0.05-20 Hz, with 5 Hz at noon. Each half of the knob is exponential: 0.05-5 Hz on
// the left, 5-20 Hz on the right.
static const float MIN_HZ = 0.05f;
static const float NOON_HZ = 5.f;
static const float MAX_HZ = 20.f;

/** SPEED knob position (0..1) to log2(Hz). */
static float speedKnobToLog2Hz(float x) {
	if (x < 0.5f)
		return crossfade(std::log2(MIN_HZ), std::log2(NOON_HZ), x / 0.5f);
	return crossfade(std::log2(NOON_HZ), std::log2(MAX_HZ), (x - 0.5f) / 0.5f);
}

/** log2(Hz) to SPEED knob position (0..1), the inverse of speedKnobToLog2Hz(). */
static float log2HzToSpeedKnob(float l) {
	if (l < std::log2(NOON_HZ))
		return 0.5f * (l - std::log2(MIN_HZ)) / (std::log2(NOON_HZ) - std::log2(MIN_HZ));
	return 0.5f + 0.5f * (l - std::log2(NOON_HZ)) / (std::log2(MAX_HZ) - std::log2(NOON_HZ));
}

/** AMP taper: knob position (0..1) to peak volts. A power curve puts NOON_AMPLITUDE at noon, so
small vibrato depths are easy to set. */
static const float AMP_TAPER = std::log2(MAX_AMPLITUDE / NOON_AMPLITUDE);
static float ampKnobToVolts(float x) {
	return MAX_AMPLITUDE * std::pow(x, AMP_TAPER);
}

/** How long the clock ratio stays on the panel after the SPEED knob moves to a new one. */
static const float RATIO_POPUP_SECONDS = 1.f;


/** One cycle of each waveform, bipolar (-1..1), for phase in [0, 1). Triangle and sine start at 0
and rise, so a vibrato fades in smoothly. */
static float waveValue(int wave, float phase) {
	switch (wave) {
		case TRIANGLE:
			if (phase < 0.25f)
				return 4.f * phase;
			if (phase < 0.75f)
				return 2.f - 4.f * phase;
			return 4.f * phase - 4.f;
		case SINE:
			return std::sin(2.f * float(M_PI) * phase);
		case SQUARE:
			return phase < 0.5f ? 1.f : -1.f;
		case RAMP_UP:
			return 2.f * phase - 1.f;
		case RAMP_DOWN:
		default:
			return 1.f - 2.f * phase;
	}
}


struct VibraLFO : Module {
	enum ParamId {
		DELAY_PARAM,
		RISE_PARAM,
		RELEASE_PARAM,
		SPEED_PARAM,
		AMPLITUDE_PARAM,
		OFFSET_PARAM,
		ENUMS(MIX_PARAM, NUM_WAVES),
		PARAMS_LEN
	};
	enum InputId {
		GATE_INPUT,
		CLOCK_INPUT,
		// One CV input per knob and fader, in the same order as the params.
		DELAY_CV_INPUT,
		RISE_CV_INPUT,
		RELEASE_CV_INPUT,
		SPEED_CV_INPUT,
		AMPLITUDE_CV_INPUT,
		OFFSET_CV_INPUT,
		ENUMS(MIX_CV_INPUT, NUM_WAVES),
		INPUTS_LEN
	};
	enum OutputId {
		LFO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(MIX_LIGHT, NUM_WAVES),
		OUT_LIGHT,
		LIGHTS_LEN
	};

	enum Stage {
		IDLE,     // no gate: output sits at OFFSET
		DELAY,    // gate is high, waiting DELAY seconds before the LFO starts
		RUN,      // LFO running, fading in over RISE seconds, then at full amplitude
		RELEASE,  // gate ended: LFO keeps running while fading out over RELEASE seconds
	};

	dsp::SchmittTrigger gateTrigger;
	dsp::SchmittTrigger clockTrigger;
	Stage stage = IDLE;
	float delayElapsed = 0.f;
	float envelope = 0.f;  // 0..1, scales the LFO
	float phase = 0.f;     // 0..1

	// Clock sync.
	float clockPeriod = 0.5f;      // seconds between the last two clock pulses
	float timeSinceClock = 0.f;
	bool clockSeen = false;
	int clockCount = 0;            // pulses since the last cycle restart, for divided ratios

	/** SPEED shows Hz when free-running, and the clock ratio when CLOCK is patched. When clocked,
	setValue() lands on the nearest ratio, and SpeedKnob steps from one ratio to the next. */
	struct SpeedQuantity : ParamQuantity {
		bool clocked() {
			return module && module->inputs[CLOCK_INPUT].isConnected();
		}
		/** The knob value for ratio step `index`: the ratios are spread evenly over the travel. */
		float ratioValue(int index) {
			return getMinValue() + (getMaxValue() - getMinValue()) * index / (NUM_RATIOS - 1);
		}
		void setValue(float value) override {
			if (clocked()) {
				float t = (clamp(value, getMinValue(), getMaxValue()) - getMinValue()) / (getMaxValue() - getMinValue());
				value = ratioValue((int) std::round(t * (NUM_RATIOS - 1)));
			}
			ParamQuantity::setValue(value);
		}
		std::string getDisplayValueString() override {
			if (clocked())
				return RATIO_LABELS[static_cast<VibraLFO*>(module)->ratioIndex(0.f)];
			return ParamQuantity::getDisplayValueString();
		}
		float getDisplayValue() override {
			return std::pow(2.f, speedKnobToLog2Hz(getValue()));
		}
		void setDisplayValue(float hz) override {
			setValue(log2HzToSpeedKnob(std::log2(clamp(hz, MIN_HZ, MAX_HZ))));
		}
		std::string getUnit() override {
			return clocked() ? " of clock" : ParamQuantity::getUnit();
		}
	};

	/** AMPLITUDE shows peak volts through the AMP taper: 0-2.5 V, 0.22 V at noon. */
	struct AmplitudeQuantity : ParamQuantity {
		float getDisplayValue() override {
			return ampKnobToVolts(getValue());
		}
		void setDisplayValue(float v) override {
			setValue(std::pow(clamp(v, 0.f, MAX_AMPLITUDE) / MAX_AMPLITUDE, 1.f / AMP_TAPER));
		}
	};

	VibraLFO() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DELAY_PARAM, 0.f, MAX_DELAY, 0.3f, "Delay (gate start to LFO start)", " s");
		configParam(RISE_PARAM, 0.f, MAX_DELAY, 0.5f, "Rise (fade-in to full amplitude)", " s");
		configParam(RELEASE_PARAM, 0.f, MAX_RELEASE, 0.2f, "Release (fade-out after the gate ends)", " s");
		// Knob position 0..1; SpeedQuantity and speedKnobToLog2Hz() turn it into Hz.
		configParam<SpeedQuantity>(SPEED_PARAM, 0.f, 1.f, 0.5f, "Speed", " Hz");
		configParam<AmplitudeQuantity>(AMPLITUDE_PARAM, 0.f, 1.f, 0.5f, "Amplitude", " V");
		configParam(OFFSET_PARAM, -5.f, 5.f, 0.f, "Offset", " V");
		for (int i = 0; i < NUM_WAVES; i++)
			configParam(MIX_PARAM + i, 0.f, 1.f, i == SINE ? 1.f : 0.f, WAVE_NAMES[i], "%", 0.f, 100.f);

		configInput(GATE_INPUT, "Gate (LFO runs while high; runs all the time when unpatched)");
		configInput(CLOCK_INPUT, "Clock (sync; SPEED then picks the ratio)");
		// Each CV adds to its control. 10 V sweeps the whole range (SPEED: 1 V/oct, OFFSET: 1 V/V).
		configInput(DELAY_CV_INPUT, "Delay CV (+10 V = +2.5 s)");
		configInput(RISE_CV_INPUT, "Rise CV (+10 V = +2.5 s)");
		configInput(RELEASE_CV_INPUT, "Release CV (+10 V = +5 s)");
		configInput(SPEED_CV_INPUT, "Speed CV (1 V/oct; clocked, 1 V = one ratio step)");
		configInput(AMPLITUDE_CV_INPUT, "Amplitude CV (+10 V = full knob turn)");
		configInput(OFFSET_CV_INPUT, "Offset CV (added to the knob)");
		for (int i = 0; i < NUM_WAVES; i++)
			configInput(MIX_CV_INPUT + i, std::string(WAVE_NAMES[i]) + " CV (+10 V = full fader)");
		configOutput(LFO_OUTPUT, "LFO");
		for (int i = 0; i < NUM_WAVES; i++)
			configLight(MIX_LIGHT + i, std::string(WAVE_NAMES[i]) + " level");
		configLight(OUT_LIGHT, "LFO");
	}

	void onReset() override {
		stage = IDLE;
		envelope = 0.f;
		phase = 0.f;
		clockSeen = false;
		clockCount = 0;
		clockPeriod = 0.5f;
	}

	/** The clock ratio picked by the SPEED knob, moved `speedCv` steps (1 V per step: each step doubles). */
	int ratioIndex(float speedCv) {
		ParamQuantity* pq = getParamQuantity(SPEED_PARAM);
		float t = (params[SPEED_PARAM].getValue() - pq->getMinValue()) / (pq->getMaxValue() - pq->getMinValue());
		return clamp((int) std::round(t * (NUM_RATIOS - 1) + speedCv), 0, NUM_RATIOS - 1);
	}

	/** A control's knob or fader value plus its CV input times `voltsToValue`, clamped to [lo, hi]. */
	float withCv(int param, int input, float voltsToValue, float lo, float hi) {
		return clamp(params[param].getValue() + inputs[input].getVoltage() * voltsToValue, lo, hi);
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;

		// LFO rate: free-running from SPEED, or a ratio of the measured clock tempo.
		bool clocked = inputs[CLOCK_INPUT].isConnected();
		float speedCv = inputs[SPEED_CV_INPUT].getVoltage();
		float freq;
		if (clocked) {
			float ratio = RATIOS[ratioIndex(speedCv)];
			int division = ratio < 1.f ? (int) std::round(1.f / ratio) : 1;
			timeSinceClock += dt;
			if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f)) {
				// Ignore very slow gaps (a stopped clock) when measuring the tempo.
				if (clockSeen && timeSinceClock < 10.f)
					clockPeriod = timeSinceClock;
				clockSeen = true;
				timeSinceClock = 0.f;
				// Restart the cycle on the beat: every pulse for 1x and faster (a whole number of
				// cycles per pulse), or every Nth pulse for 1/N.
				clockCount = (clockCount + 1) % division;
				if (clockCount == 0)
					phase = 0.f;
			}
			freq = ratio / clockPeriod;
		}
		else {
			clockSeen = false;
			clockCount = 0;
			// Adding the CV in log2(Hz) gives 1 V/oct.
			float log2Hz = speedKnobToLog2Hz(params[SPEED_PARAM].getValue()) + speedCv;
			freq = std::pow(2.f, clamp(log2Hz, std::log2(MIN_HZ), std::log2(MAX_HZ)));
		}

		float delay = withCv(DELAY_PARAM, DELAY_CV_INPUT, MAX_DELAY / 10.f, 0.f, MAX_DELAY);
		float rise = withCv(RISE_PARAM, RISE_CV_INPUT, MAX_DELAY / 10.f, 0.f, MAX_DELAY);
		float release = withCv(RELEASE_PARAM, RELEASE_CV_INPUT, MAX_RELEASE / 10.f, 0.f, MAX_RELEASE);

		// Gate stages. With nothing patched to GATE, the gate counts as held high, so the LFO runs
		// all the time (after one delay and rise when the module starts or the cable is pulled).
		float gate = inputs[GATE_INPUT].isConnected() ? inputs[GATE_INPUT].getVoltage() : 10.f;
		bool wasHigh = gateTrigger.isHigh();
		if (gateTrigger.process(gate, 0.1f, 1.f)) {
			stage = DELAY;
			delayElapsed = 0.f;
			envelope = 0.f;
		}
		else if (wasHigh && !gateTrigger.isHigh()) {
			if (stage == DELAY)
				stage = IDLE;
			else if (stage == RUN)
				stage = RELEASE;
		}

		if (stage == DELAY) {
			delayElapsed += dt;
			if (delayElapsed >= delay) {
				stage = RUN;
				// Free-running, each gate starts a fresh cycle. Clocked, the phase stays on the beat.
				if (!clocked)
					phase = 0.f;
			}
		}
		if (stage == RUN) {
			envelope = (rise > 0.f) ? std::min(envelope + dt / rise, 1.f) : 1.f;
		}
		else if (stage == RELEASE) {
			envelope = (release > 0.f) ? envelope - dt / release : 0.f;
			if (envelope <= 0.f) {
				envelope = 0.f;
				stage = IDLE;
			}
		}

		// Advance the LFO while it is audible, and always when clocked so it stays on the beat.
		if (clocked || stage == RUN || stage == RELEASE) {
			phase += freq * dt;
			phase -= std::floor(phase);
		}

		// Blend the waveforms in proportion to their faders. Dividing by the total keeps the
		// peak at AMPLITUDE however many faders are up.
		float mix = 0.f;
		float total = 0.f;
		for (int i = 0; i < NUM_WAVES; i++) {
			float level = withCv(MIX_PARAM + i, MIX_CV_INPUT + i, 0.1f, 0.f, 1.f);
			total += level;
			mix += level * waveValue(i, phase);
			lights[MIX_LIGHT + i].setBrightness(level);
		}
		if (total > 0.f)
			mix /= total;

		// The CV moves the knob position, then the taper applies, matching AmplitudeQuantity.
		float amp = withCv(AMPLITUDE_PARAM, AMPLITUDE_CV_INPUT, 0.1f, 0.f, 1.f);
		float amplitude = ampKnobToVolts(amp);
		float lfo = amplitude * envelope * mix;
		float offset = withCv(OFFSET_PARAM, OFFSET_CV_INPUT, 1.f, -10.f, 10.f);
		outputs[LFO_OUTPUT].setVoltage(offset + lfo);
		lights[OUT_LIGHT].setBrightness(envelope * (mix + 1.f) / 2.f);

	}

};


/** A small white drawing of one waveform, shown under its fader. */
struct WaveIcon : TransparentWidget {
	int wave;

	WaveIcon(Vec centerMm, int wave) {
		this->wave = wave;
		box.size = mm2px(Vec(8.f, 4.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	void draw(const DrawArgs& args) override {
		const int steps = 48;
		float pad = 1.f;
		float w = box.size.x - 2 * pad;
		float h = box.size.y - 2 * pad;
		nvgBeginPath(args.vg);
		for (int i = 0; i <= steps; i++) {
			float p = float(i) / steps;
			// Square and ramps jump at the cycle edge; draw those jumps as vertical lines.
			float v = waveValue(wave, std::min(p, 0.9999f));
			float x = pad + p * w;
			float y = pad + (1.f - v) / 2.f * h;
			if (i == 0)
				nvgMoveTo(args.vg, x, y);
			else
				nvgLineTo(args.vg, x, y);
		}
		nvgStrokeColor(args.vg, COLOR_TEXT);
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
	}
};


/** When clocked, shows the SPEED knob's clock ratio for a moment after it changes: large black
text in a white box, centered over the top of the panel. It is drawn with nanovg, so it works on
the MetaModule too (as a graphics display). */
struct RatioPopup : TransparentWidget {
	VibraLFO* module;
	int lastIndex = -1;
	double shownAt = -1e9;

	RatioPopup(Vec centerMm, VibraLFO* module) {
		this->module = module;
		box.size = mm2px(Vec(32.f, 11.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	bool clocked() {
		return module && module->inputs[VibraLFO::CLOCK_INPUT].isConnected();
	}

	void step() override {
		TransparentWidget::step();
		if (!clocked()) {
			lastIndex = -1;
			return;
		}
		int index = module->ratioIndex(0.f);
		// Show it only when the knob moves to a new ratio, not when the clock is first patched.
		if (lastIndex >= 0 && index != lastIndex)
			shownAt = system::getTime();
		lastIndex = index;
	}

	void draw(const DrawArgs& args) override {
		if (!clocked() || system::getTime() - shownAt > RATIO_POPUP_SECONDS)
			return;
		std::shared_ptr<window::Font> font = loadPanelFont();
		if (!font || font->handle < 0)
			return;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
		nvgFill(args.vg);
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 26.f);
		nvgFillColor(args.vg, nvgRGB(0x00, 0x00, 0x00));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, box.size.x / 2, box.size.y / 2, RATIO_LABELS[lastIndex], NULL);
	}
};


/** The SPEED knob. While CLOCK is patched, dragging or scrolling steps it from one clock ratio to
the next. Rack's own snapping can't do this: it rounds the knob to whole numbers, and the ratio
steps aren't whole numbers apart, so the knob got stuck. Free-running, it is a normal knob. */
#ifdef METAMODULE
// The MetaModule has its own knob handling; the knob value still picks the nearest ratio.
using SpeedKnob = MediumKnob;
#else
struct SpeedKnob : MediumKnob {
	/** Mouse travel per ratio step, in screen pixels. */
	static constexpr float PIXELS_PER_STEP = 45.f;
	float dragTravel = 0.f;

	VibraLFO::SpeedQuantity* speedQuantity() {
		return dynamic_cast<VibraLFO::SpeedQuantity*>(getParamQuantity());
	}

	void stepRatio(VibraLFO::SpeedQuantity* pq, int steps) {
		VibraLFO* m = static_cast<VibraLFO*>(pq->module);
		int index = clamp(m->ratioIndex(0.f) + steps, 0, NUM_RATIOS - 1);
		pq->setValue(pq->ratioValue(index));
	}

	void onDragStart(const DragStartEvent& e) override {
		dragTravel = 0.f;
		MediumKnob::onDragStart(e);
	}

	void onDragMove(const DragMoveEvent& e) override {
		VibraLFO::SpeedQuantity* pq = speedQuantity();
		if (!pq || !pq->clocked() || e.button != GLFW_MOUSE_BUTTON_LEFT) {
			MediumKnob::onDragMove(e);
			return;
		}
		// Up or right turns it up, like Rack's linear knob mode. Hold Ctrl for finer steps.
		float travel = e.mouseDelta.x - e.mouseDelta.y;
		if ((APP->window->getMods() & RACK_MOD_MASK) == RACK_MOD_CTRL)
			travel /= 4.f;
		dragTravel += travel;
		int steps = (int) std::trunc(dragTravel / PIXELS_PER_STEP);
		if (steps != 0) {
			dragTravel -= steps * PIXELS_PER_STEP;
			stepRatio(pq, steps);
		}
	}

	void onHoverScroll(const HoverScrollEvent& e) override {
		VibraLFO::SpeedQuantity* pq = speedQuantity();
		if (!pq || !pq->clocked()) {
			MediumKnob::onHoverScroll(e);
			return;
		}
		float scroll = e.scrollDelta.y;
		if (scroll != 0.f) {
			stepRatio(pq, scroll > 0.f ? 1 : -1);
			e.consume(this);
		}
	}
};
#endif


struct VibraLFOWidget : ModuleWidget {
	void label(float xMm, float yMm, std::string text, float size = LABEL_SIZE) {
		addChild(new PanelLabel(Vec(xMm, yMm), text, size));
	}

	void cvJack(Vec posMm, Module* module, int input) {
		addInput(createInputCentered<PJ301MPort>(mm2px(posMm), module, input));
	}

	VibraLFOWidget(VibraLFO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VibraLFO.svg")));
		addScrews(this);

		label(35.56f, 6.f, "VibraLFo", TITLE_SIZE);

		// Three columns of controls. Each knob and fader has its CV jack right under it.
		const float colX[3] = {14.f, 35.56f, 57.12f};
		const float cvBelowKnob = 9.5f;

		// Envelope: delay, rise, release.
		const float envY = 23.f;
		label(colX[0], envY - 7.5f, "DELAY");
		label(colX[1], envY - 7.5f, "RISE");
		label(colX[2], envY - 7.5f, "RELEASE");
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[0], envY)), module, VibraLFO::DELAY_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[1], envY)), module, VibraLFO::RISE_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[2], envY)), module, VibraLFO::RELEASE_PARAM));
		cvJack(Vec(colX[0], envY + cvBelowKnob), module, VibraLFO::DELAY_CV_INPUT);
		cvJack(Vec(colX[1], envY + cvBelowKnob), module, VibraLFO::RISE_CV_INPUT);
		cvJack(Vec(colX[2], envY + cvBelowKnob), module, VibraLFO::RELEASE_CV_INPUT);

		// LFO: speed, amplitude, offset.
		const float lfoY = 48.5f;
		label(colX[0], lfoY - 7.5f, "SPEED");
		label(colX[1], lfoY - 7.5f, "AMP");
		label(colX[2], lfoY - 7.5f, "OFFSET");
		addParam(createParamCentered<SpeedKnob>(mm2px(Vec(colX[0], lfoY)), module, VibraLFO::SPEED_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[1], lfoY)), module, VibraLFO::AMPLITUDE_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[2], lfoY)), module, VibraLFO::OFFSET_PARAM));
		cvJack(Vec(colX[0], lfoY + cvBelowKnob), module, VibraLFO::SPEED_CV_INPUT);
		cvJack(Vec(colX[1], lfoY + cvBelowKnob), module, VibraLFO::AMPLITUDE_CV_INPUT);
		cvJack(Vec(colX[2], lfoY + cvBelowKnob), module, VibraLFO::OFFSET_CV_INPUT);

		// Jacks: gate and clock in, LFO out.
		const float ioY = 75.f;
		label(colX[0], ioY - 8.f, "GATE");
		label(colX[1], ioY - 8.f, "CLOCK");
		label(colX[2], ioY - 8.f, "OUT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[0], ioY)), module, VibraLFO::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[1], ioY)), module, VibraLFO::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colX[2], ioY)), module, VibraLFO::LFO_OUTPUT));
		addChild(createLightCentered<SmallLight<BrightBlueLight>>(mm2px(Vec(colX[2] + 7.f, ioY - 7.5f)), module, VibraLFO::OUT_LIGHT));

		// Mix faders along the bottom, one per waveform: a drawing of the wave above each fader,
		// and its CV jack below.
		for (int i = 0; i < NUM_WAVES; i++) {
			float x = 9.56f + i * 13.f;
			addChild(new WaveIcon(Vec(x, 84.f), i));
			addParam(createLightParamCentered<BlueFader>(mm2px(Vec(x, 100.f)), module, VibraLFO::MIX_PARAM + i, VibraLFO::MIX_LIGHT + i));
			cvJack(Vec(x, 117.5f), module, VibraLFO::MIX_CV_INPUT + i);
		}

		// Added last so it draws over everything else.
		addChild(new RatioPopup(Vec(35.56f, 7.f), module));
	}
};


Model* modelVibraLFO = createModel<VibraLFO, VibraLFOWidget>("VibraLFO");
