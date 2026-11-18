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
				return RATIO_LABELS[static_cast<VibroLFO*>(module)->ratioIndex()];
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

	int ratioIndex() {
		ParamQuantity* pq = getParamQuantity(SPEED_PARAM);
		float t = (params[SPEED_PARAM].getValue() - pq->getMinValue()) / (pq->getMaxValue() - pq->getMinValue());
		return clamp((int) std::round(t * (NUM_RATIOS - 1)), 0, NUM_RATIOS - 1);
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;

		// LFO rate: free-running from SPEED, or a ratio of the measured clock tempo.
		bool clocked = inputs[CLOCK_INPUT].isConnected();
		float freq;
		if (clocked) {
			float ratio = RATIOS[ratioIndex()];
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
			freq = std::pow(2.f, params[SPEED_PARAM].getValue());
		}

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
			if (delayElapsed >= params[DELAY_PARAM].getValue()) {
				stage = RUN;
				// Free-running, each gate starts a fresh cycle. Clocked, the phase stays on the beat.
				if (!clocked)
					phase = 0.f;
			}
		}
		if (stage == RUN) {
			float rise = params[RISE_PARAM].getValue();
			envelope = (rise > 0.f) ? std::min(envelope + dt / rise, 1.f) : 1.f;
		}
		else if (stage == RELEASE) {
			float release = params[RELEASE_PARAM].getValue();
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
			float level = params[MIX_PARAM + i].getValue();
			total += level;
			mix += level * waveValue(i, phase);
			lights[MIX_LIGHT + i].setBrightness(level);
		}
		if (total > 0.f)
			mix /= total;

		// Squared taper, matching AmplitudeQuantity's display.
		float amp = params[AMPLITUDE_PARAM].getValue();
		float amplitude = MAX_AMPLITUDE * amp * amp;
		float lfo = amplitude * envelope * mix;
		outputs[LFO_OUTPUT].setVoltage(params[OFFSET_PARAM].getValue() + lfo);
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

	VibroLFOWidget(VibroLFO* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/VibroLFO.svg")));
		addScrews(this);

		label(35.56f, 6.f, "VIBROLFO", TITLE_SIZE);

		// Three columns of controls.
		const float colX[3] = {14.f, 35.56f, 57.12f};

		// Envelope: delay, rise, release.
		label(colX[0], 15.5f, "DELAY");
		label(colX[1], 15.5f, "RISE");
		label(colX[2], 15.5f, "RELEASE");
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[0], 23.5f)), module, VibroLFO::DELAY_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[1], 23.5f)), module, VibroLFO::RISE_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[2], 23.5f)), module, VibroLFO::RELEASE_PARAM));

		// LFO: speed, amplitude, offset.
		label(colX[0], 33.5f, "SPEED");
		label(colX[1], 33.5f, "AMP");
		label(colX[2], 33.5f, "OFFSET");
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[0], 41.5f)), module, VibroLFO::SPEED_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[1], 41.5f)), module, VibroLFO::AMPLITUDE_PARAM));
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[2], 41.5f)), module, VibroLFO::OFFSET_PARAM));

		// Jacks: gate and clock in, LFO out.
		label(colX[0], 52.f, "GATE");
		label(colX[1], 52.f, "CLOCK");
		label(colX[2], 52.f, "OUT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[0], 60.5f)), module, VibroLFO::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[1], 60.5f)), module, VibroLFO::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(colX[2], 60.5f)), module, VibroLFO::LFO_OUTPUT));
		addChild(createLightCentered<SmallLight<BrightBlueLight>>(mm2px(Vec(colX[2] + 7.f, 55.f)), module, VibroLFO::OUT_LIGHT));

		// Mix faders along the bottom, one per waveform, with a drawing of the wave under each.
		label(35.56f, 73.f, "MIX");
		for (int i = 0; i < NUM_WAVES; i++) {
			float x = 9.56f + i * 13.f;
			addParam(createLightParamCentered<BlueFader>(mm2px(Vec(x, 92.f)), module, VibroLFO::MIX_PARAM + i, VibroLFO::MIX_LIGHT + i));
			addChild(new WaveIcon(Vec(x, 110.f), i));
		}
	}
};


Model* modelVibroLFO = createModel<VibroLFO, VibroLFOWidget>("VibroLFO");
