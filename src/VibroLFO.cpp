#include "house_style.hpp"


static const int NUM_WAVES = 5;
enum Wave { TRIANGLE, SINE, SQUARE, RAMP_UP, RAMP_DOWN };
static const char* WAVE_NAMES[NUM_WAVES] = {"Triangle", "Sine", "Square", "Ramp up", "Ramp down"};

// Clock-sync ratios: LFO cycles per clock pulse, chosen with the SPEED knob when CLOCK is patched.
static const int NUM_RATIOS = 7;
static const float RATIOS[NUM_RATIOS] = {1.f / 8, 1.f / 4, 1.f / 2, 1.f, 2.f, 4.f, 8.f};
static const char* RATIO_LABELS[NUM_RATIOS] = {"1/8x", "1/4x", "1/2x", "1x", "2x", "4x", "8x"};

static const float MAX_TIME = 5.f;       // delay, rise and release knobs: 0-5 s
static const float MAX_AMPLITUDE = 5.f;  // peak volts
static const float MIN_HZ = 0.05f;
static const float MAX_HZ = 20.f;


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


struct VibroLFO : Module {
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

	/** SPEED shows Hz when free-running, and the clock ratio when CLOCK is patched. */
	struct SpeedQuantity : ParamQuantity {
		bool clocked() {
			return module && module->inputs[CLOCK_INPUT].isConnected();
		}
		std::string getDisplayValueString() override {
			if (clocked())
				return RATIO_LABELS[static_cast<VibroLFO*>(module)->ratioIndex(0.f)];
			return ParamQuantity::getDisplayValueString();
		}
		std::string getUnit() override {
			return clocked() ? " of clock" : ParamQuantity::getUnit();
		}
	};

	/** AMPLITUDE is squared so small vibrato depths are easy to set: 0-5 V peak. */
	struct AmplitudeQuantity : ParamQuantity {
		float getDisplayValue() override {
			float x = getValue();
			return MAX_AMPLITUDE * x * x;
		}
		void setDisplayValue(float v) override {
			setValue(std::sqrt(clamp(v, 0.f, MAX_AMPLITUDE) / MAX_AMPLITUDE));
		}
	};

	VibroLFO() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(DELAY_PARAM, 0.f, MAX_TIME, 0.3f, "Delay (gate start to LFO start)", " s");
		configParam(RISE_PARAM, 0.f, MAX_TIME, 0.5f, "Rise (fade-in to full amplitude)", " s");
		configParam(RELEASE_PARAM, 0.f, MAX_TIME, 0.2f, "Release (fade-out after the gate ends)", " s");
		// Free-running: 2^value Hz, so the knob sweeps 0.05-20 Hz exponentially.
		configParam<SpeedQuantity>(SPEED_PARAM, std::log2(MIN_HZ), std::log2(MAX_HZ), std::log2(5.f), "Speed", " Hz", 2.f);
		configParam<AmplitudeQuantity>(AMPLITUDE_PARAM, 0.f, 1.f, std::sqrt(1.f / MAX_AMPLITUDE), "Amplitude", " V");
		configParam(OFFSET_PARAM, -5.f, 5.f, 0.f, "Offset", " V");
		for (int i = 0; i < NUM_WAVES; i++)
			configParam(MIX_PARAM + i, 0.f, 1.f, i == SINE ? 1.f : 0.f, WAVE_NAMES[i], "%", 0.f, 100.f);

		configInput(GATE_INPUT, "Gate (LFO runs while high)");
		configInput(CLOCK_INPUT, "Clock (sync; SPEED then picks the ratio)");
		// Each CV adds to its control. 10 V sweeps the whole range (SPEED: 1 V/oct, OFFSET: 1 V/V).
		configInput(DELAY_CV_INPUT, "Delay CV (+10 V = +5 s)");
		configInput(RISE_CV_INPUT, "Rise CV (+10 V = +5 s)");
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
			// The knob is log2(Hz), so adding the CV gives 1 V/oct.
			freq = std::pow(2.f, clamp(params[SPEED_PARAM].getValue() + speedCv, std::log2(MIN_HZ), std::log2(MAX_HZ)));
		}

		const float timePerVolt = MAX_TIME / 10.f;
		float delay = withCv(DELAY_PARAM, DELAY_CV_INPUT, timePerVolt, 0.f, MAX_TIME);
		float rise = withCv(RISE_PARAM, RISE_CV_INPUT, timePerVolt, 0.f, MAX_TIME);
		float release = withCv(RELEASE_PARAM, RELEASE_CV_INPUT, timePerVolt, 0.f, MAX_TIME);

		// Gate stages.
		bool wasHigh = gateTrigger.isHigh();
		if (gateTrigger.process(inputs[GATE_INPUT].getVoltage(), 0.1f, 1.f)) {
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

		// Squared taper, matching AmplitudeQuantity's display.
		float amp = withCv(AMPLITUDE_PARAM, AMPLITUDE_CV_INPUT, 0.1f, 0.f, 1.f);
		float amplitude = MAX_AMPLITUDE * amp * amp;
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


struct VibroLFOWidget : ModuleWidget {
	void label(float xMm, float yMm, std::string text, float size = LABEL_SIZE) {
		addChild(new PanelLabel(Vec(xMm, yMm), text, size));
	}

	void cvJack(Vec posMm, Module* module, int input) {
		addInput(createInputCentered<PJ301MPort>(mm2px(posMm), module, input));
	}

	VibroLFOWidget(VibroLFO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VibroLFO.svg")));
		addScrews(this);

		label(35.56f, 6.f, "VIBROLFO", TITLE_SIZE);

		// Three columns of controls. Each knob and fader has its CV jack right under it.
		const float colX[3] = {14.f, 35.56f, 57.12f};
		const float cvBelowKnob = 9.5f;

		// Envelope: delay, rise, release.
		const float envY = 23.f;
		label(colX[0], envY - 7.5f, "DELAY");
		label(colX[1], envY - 7.5f, "RISE");
		label(colX[2], envY - 7.5f, "RELEASE");
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[0], envY)), module, VibroLFO::DELAY_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[1], envY)), module, VibroLFO::RISE_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[2], envY)), module, VibroLFO::RELEASE_PARAM));
		cvJack(Vec(colX[0], envY + cvBelowKnob), module, VibroLFO::DELAY_CV_INPUT);
		cvJack(Vec(colX[1], envY + cvBelowKnob), module, VibroLFO::RISE_CV_INPUT);
		cvJack(Vec(colX[2], envY + cvBelowKnob), module, VibroLFO::RELEASE_CV_INPUT);

		// LFO: speed, amplitude, offset.
		const float lfoY = 48.5f;
		label(colX[0], lfoY - 7.5f, "SPEED");
		label(colX[1], lfoY - 7.5f, "AMP");
		label(colX[2], lfoY - 7.5f, "OFFSET");
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[0], lfoY)), module, VibroLFO::SPEED_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[1], lfoY)), module, VibroLFO::AMPLITUDE_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[2], lfoY)), module, VibroLFO::OFFSET_PARAM));
		cvJack(Vec(colX[0], lfoY + cvBelowKnob), module, VibroLFO::SPEED_CV_INPUT);
		cvJack(Vec(colX[1], lfoY + cvBelowKnob), module, VibroLFO::AMPLITUDE_CV_INPUT);
		cvJack(Vec(colX[2], lfoY + cvBelowKnob), module, VibroLFO::OFFSET_CV_INPUT);

		// Jacks: gate and clock in, LFO out.
		const float ioY = 75.f;
		label(colX[0], ioY - 8.f, "GATE");
		label(colX[1], ioY - 8.f, "CLOCK");
		label(colX[2], ioY - 8.f, "OUT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[0], ioY)), module, VibroLFO::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[1], ioY)), module, VibroLFO::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colX[2], ioY)), module, VibroLFO::LFO_OUTPUT));
		addChild(createLightCentered<SmallLight<BrightBlueLight>>(mm2px(Vec(colX[2] + 7.f, ioY - 7.5f)), module, VibroLFO::OUT_LIGHT));

		// Mix faders along the bottom, one per waveform: a drawing of the wave above each fader,
		// and its CV jack below.
		for (int i = 0; i < NUM_WAVES; i++) {
			float x = 9.56f + i * 13.f;
			addChild(new WaveIcon(Vec(x, 84.f), i));
			addParam(createLightParamCentered<BlueFader>(mm2px(Vec(x, 100.f)), module, VibroLFO::MIX_PARAM + i, VibroLFO::MIX_LIGHT + i));
			cvJack(Vec(x, 117.5f), module, VibroLFO::MIX_CV_INPUT + i);
		}
	}
};


Model* modelVibroLFO = createModel<VibroLFO, VibroLFOWidget>("VibroLFO");
