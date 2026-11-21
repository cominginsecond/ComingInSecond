#include "house_style.hpp"


// SynnDrum: a one-voice analog-style drum synth modelled on one channel of the Pearl Syncussion SY-1
// (as recreated in the Michigan Synth Works SY0.5). Two oscillators and a noise source, combined by
// the OSC MODE (A-F), with a pitch sweep, an LFO / sample-and-hold on pitch, a low-pass filter that
// follows the decay, and a VCA whose level follows how hard the trigger hits.


// ---- Ranges. Starting points, to be tuned by ear. ----

// TUNE: 30 Hz at the bottom, six octaves of travel (to 1920 Hz). PITCH CV adds 1 V/oct.
static const float TUNE_MIN_HZ = 30.f;
static const float TUNE_OCTAVES = 6.f;
// DECAY: time to fall 60 dB, 30 ms to 6 s (exponential fader).
static const float DECAY_MIN_S = 0.03f;
static const float DECAY_RANGE = 200.f;
// SWEEP: the fader sets the final pitch, up to this many octaves from the struck pitch.
static const float SWEEP_MAX_OCT = 3.f;
// SWEEP SPEED: glide time constant, 2 s fully left to 4 ms fully right.
static const float SWEEP_SLOW_S = 2.f;
static const float SWEEP_SPEED_RANGE = 0.002f;
// LFO SPEED: 0.2 Hz to 80 Hz.
static const float LFO_MIN_HZ = 0.2f;
static const float LFO_SPEED_RANGE = 400.f;
// LFO fader: vibrato depth up to this many octaves (square law, so small depths are easy to set).
static const float LFO_MAX_OCT = 2.f;
// S/H: each new random value moves the pitch by up to this many octaves either way.
static const float SH_OCT = 1.f;
// WIDTH: how far above its resting point the filter opens at the start of a hit, in octaves.
static const float WIDTH_OCT = 7.f;
// Filter rest point, in octaves above the tuned pitch.
static const float FILTER_REST_OCT = 0.5f;
static const float FILTER_Q = 0.9f;
// Mode D's velocity sweep: rises from up to this many octaves below the pitch, with this time constant.
static const float MODE_D_SWEEP_OCT = 2.f;
static const float MODE_D_SWEEP_S = 0.1f;
// VCA attack, long enough to avoid a click.
static const float ATTACK_S = 0.0005f;
// After a trigger edge, the input is watched this long for its peak (the hit's velocity).
static const float VELOCITY_WINDOW_S = 0.001f;
// Output level, volts peak.
static const float OUTPUT_VOLTS = 5.f;
// Holding both MODE buttons this long toggles the MODE jack between CV select and gate advance.
static const float BOTH_BUTTONS_HOLD_S = 3.f;


// ---- OSC MODE table. ----

enum Wave { WAVE_TRIANGLE, WAVE_SQUARE };

struct OscMode {
	char letter;
	Wave wave1;
	Wave wave2;
	float ratio2;    // oscillator 2's frequency as a multiple of oscillator 1's
	float fmIndex;   // > 0: oscillator 2 frequency-modulates oscillator 1 (and is not heard itself)
	float level1;
	float level2;
	float noise;
	bool velocitySweep;  // mode D: velocity drives a fixed sweep instead of the SWEEP controls
	const char* description;
};

static const int NUM_MODES = 6;
static const OscMode MODES[NUM_MODES] = {
	// letter wave1          wave2          ratio2 fm    lvl1  lvl2  noise velSweep
	{'A', WAVE_SQUARE,   WAVE_SQUARE,   1.f,   0.f,  1.f,  0.f,  0.f,  false, "One oscillator"},
	{'B', WAVE_TRIANGLE, WAVE_SQUARE,   1.47f, 1.6f, 1.f,  0.f,  0.f,  false, "FM pair (metallic)"},
	{'C', WAVE_TRIANGLE, WAVE_TRIANGLE, 2.76f, 0.f,  0.6f, 0.4f, 0.f,  false, "Two oscillators (vibraphone)"},
	{'D', WAVE_SQUARE,   WAVE_SQUARE,   1.5f,  0.f,  0.5f, 0.5f, 0.f,  true,  "Two oscillators, sweep by velocity"},
	{'E', WAVE_TRIANGLE, WAVE_SQUARE,   1.47f, 1.6f, 0.6f, 0.f,  0.5f, false, "FM pair with noise (thin metal)"},
	{'F', WAVE_SQUARE,   WAVE_SQUARE,   1.f,   0.f,  0.f,  0.f,  1.f,  false, "Noise only"},
};


/** PolyBLEP correction for a step at phase 0, for a phase increment of dt per sample. */
static inline float polyBlep(float t, float dt) {
	if (dt <= 0.f || dt >= 0.5f)
		return 0.f;
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.f;
	}
	if (t > 1.f - dt) {
		t = (t - 1.f) / dt;
		return t * t + t + t + 1.f;
	}
	return 0.f;
}

/** A free-running oscillator. Phase is kept in [0, 1) and may run backwards under deep FM. */
struct Oscillator {
	float phase = 0.f;

	/** Advances by `dt` cycles and returns the waveform, -1..1. */
	float process(Wave wave, float dt) {
		phase += dt;
		phase -= std::floor(phase);
		if (wave == WAVE_TRIANGLE)
			return phase < 0.5f ? 4.f * phase - 1.f : 3.f - 4.f * phase;
		float v = phase < 0.5f ? 1.f : -1.f;
		float p2 = phase + 0.5f;
		p2 -= std::floor(p2);
		return v + polyBlep(phase, dt) - polyBlep(p2, dt);
	}
};

/** Two-pole state-variable low-pass (trapezoidal integration, stable at any cutoff). */
struct LowPass {
	float ic1 = 0.f, ic2 = 0.f;

	float process(float in, float cutoffHz, float sampleRate) {
		float g = std::tan(float(M_PI) * cutoffHz / sampleRate);
		float k = 1.f / FILTER_Q;
		float a1 = 1.f / (1.f + g * (g + k));
		float a2 = g * a1;
		float a3 = g * a2;
		float v3 = in - ic2;
		float v1 = a1 * ic1 + a2 * v3;
		float v2 = ic2 + a2 * ic1 + a3 * v3;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		return v2;
	}

	void reset() {
		ic1 = ic2 = 0.f;
	}
};


struct SynnDrum : Module {
	enum ParamId {
		SWEEP_PARAM,
		TUNE_PARAM,
		DECAY_PARAM,
		WIDTH_PARAM,
		LFO_PARAM,
		SWEEP_SPEED_PARAM,
		LFO_SPEED_PARAM,
		SWEEP_DIR_PARAM,
		SH_PARAM,
		LFO_SHAPE_PARAM,
		MODE_UP_PARAM,
		MODE_DOWN_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		DECAY_CV_INPUT,
		FILTER_CV_INPUT,
		TRIGGER_INPUT,
		PITCH_INPUT,
		MODE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		AUDIO_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		TRIGGER_LIGHT,
		LFO_LIGHT,
		ENUMS(FADER_LIGHT, 5),  // fader handles, lit by their position (+ CV)
		LIGHTS_LEN
	};

	// Switch positions.
	enum SweepDir { SWEEP_DOWN, SWEEP_UP };
	enum LfoShape { LFO_SQUARE, LFO_TRIANGLE };

	// OSC MODE: the mode chosen with the buttons, and the mode in use (after the MODE jack).
	int manualMode = 0;
	int mode = 0;
	bool gateAdvance = false;  // MODE jack: false = 0-5 V selects the mode, true = each gate steps it
	int lastDirection = 1;     // the last button pressed: +1 increment, -1 decrement
	dsp::BooleanTrigger upButton, downButton;
	dsp::SchmittTrigger modeGate;
	float bothHeld = 0.f;
	bool bothToggled = false;
	int modeBeforePress = 0;   // restored if the press turns into a both-buttons hold

	// Trigger and velocity.
	dsp::SchmittTrigger trigger;
	float velocity = 0.f;        // 0..1, set by the trigger's peak voltage
	float velocityWindow = 0.f;  // time left to watch for the peak

	// Envelopes.
	bool attacking = false;
	float envelope = 0.f;    // 0..1, filter shape
	float amp = 0.f;         // 0..1, VCA level (envelope times velocity)
	float sweep = 0.f;       // 0..1, glide from the struck pitch to the SWEEP pitch
	float modeDSweep = 0.f;  // 0..1, mode D's velocity sweep
	float modeDDepth = 0.f;  // octaves, set at each hit

	// Sound sources.
	Oscillator osc1, osc2;
	LowPass filter;
	float lfoPhase = 0.f;
	float sampleHold = 0.f;  // -1..1

	SynnDrum() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SWEEP_PARAM, 0.f, 1.f, 0.f, "Sweep (final pitch; 0 = off)", " oct", 0.f, SWEEP_MAX_OCT);
		configParam(TUNE_PARAM, 0.f, 1.f, 0.4f, "Tune", " Hz", std::pow(2.f, TUNE_OCTAVES), TUNE_MIN_HZ);
		configParam(DECAY_PARAM, 0.f, 1.f, 0.4f, "Decay", " s", DECAY_RANGE, DECAY_MIN_S);
		configParam(WIDTH_PARAM, 0.f, 1.f, 0.3f, "Width (filter range: bright at the hit, closing as it decays)", "%", 0.f, 100.f);
		configParam(LFO_PARAM, 0.f, 1.f, 0.f, "LFO depth (0 = off)", "%", 0.f, 100.f);
		configParam(SWEEP_SPEED_PARAM, 0.f, 1.f, 0.5f, "Sweep speed (glide time)", " s", SWEEP_SPEED_RANGE, SWEEP_SLOW_S);
		configParam(LFO_SPEED_PARAM, 0.f, 1.f, 0.5f, "LFO speed (also the S/H rate)", " Hz", LFO_SPEED_RANGE, LFO_MIN_HZ);
		configSwitch(SWEEP_DIR_PARAM, 0.f, 1.f, SWEEP_DOWN, "Sweep direction", {"Down", "Up"});
		configSwitch(SH_PARAM, 0.f, 1.f, 0.f, "Sample and hold (random pitch at the LFO speed and on each hit)", {"Off", "On"});
		configSwitch(LFO_SHAPE_PARAM, 0.f, 1.f, LFO_TRIANGLE, "LFO shape", {"Square", "Triangle"});
		configButton(MODE_UP_PARAM, "Mode increment (hold both buttons 3 s to switch the MODE jack function)");
		configButton(MODE_DOWN_PARAM, "Mode decrement (hold both buttons 3 s to switch the MODE jack function)");

		configInput(DECAY_CV_INPUT, "Decay CV (±10 V, adds to DECAY)");
		configInput(FILTER_CV_INPUT, "Filter CV (±10 V, adds to WIDTH)");
		configInput(TRIGGER_INPUT, "Trigger (dynamic: the pulse height sets the level, 10 V = full)");
		configInput(PITCH_INPUT, "Pitch (1 V/oct, adds to TUNE)");
		configInput(MODE_INPUT, "Mode (0-5 V selects A-F, or each gate steps the mode)");
		configOutput(AUDIO_OUTPUT, "Audio");
		configLight(TRIGGER_LIGHT, "Trigger");
		configLight(LFO_LIGHT, "LFO");
		const char* faderNames[5] = {"Sweep", "Tune", "Decay", "Width", "LFO depth"};
		for (int i = 0; i < 5; i++)
			configLight(FADER_LIGHT + i, faderNames[i]);
	}

	void onReset() override {
		manualMode = mode = 0;
		gateAdvance = false;
		lastDirection = 1;
		envelope = amp = velocity = 0.f;
		attacking = false;
		filter.reset();
	}

	static int wrapMode(int m) {
		return ((m % NUM_MODES) + NUM_MODES) % NUM_MODES;
	}

	/** Mode buttons and the MODE jack. */
	void processMode(float dt) {
		bool up = params[MODE_UP_PARAM].getValue() > 0.5f;
		bool down = params[MODE_DOWN_PARAM].getValue() > 0.5f;
		if (upButton.process(up)) {
			if (!down)
				modeBeforePress = manualMode;
			manualMode = wrapMode(manualMode + 1);
			lastDirection = 1;
		}
		if (downButton.process(down)) {
			if (!up)
				modeBeforePress = manualMode;
			manualMode = wrapMode(manualMode - 1);
			lastDirection = -1;
		}
		if (up && down) {
			bothHeld += dt;
			if (bothHeld >= BOTH_BUTTONS_HOLD_S && !bothToggled) {
				gateAdvance = !gateAdvance;
				bothToggled = true;
				// The two presses that started the hold shouldn't change the mode.
				manualMode = modeBeforePress;
			}
		}
		else {
			bothHeld = 0.f;
			bothToggled = false;
		}

		mode = manualMode;
		if (inputs[MODE_INPUT].isConnected()) {
			float v = inputs[MODE_INPUT].getVoltage();
			if (gateAdvance) {
				if (modeGate.process(v, 0.5f, 2.f))
					manualMode = mode = wrapMode(manualMode + lastDirection);
			}
			else {
				// 0-5 V spans the six modes, counted up from the mode set with the buttons.
				int offset = clamp((int) std::floor(v * NUM_MODES / 5.f), 0, NUM_MODES - 1);
				mode = std::min(manualMode + offset, NUM_MODES - 1);
			}
		}
	}

	void process(const ProcessArgs& args) override {
		float dt = args.sampleTime;
		processMode(dt);
		const OscMode& m = MODES[mode];

		// Trigger: the edge starts the hit; the peak over the next millisecond sets its velocity.
		float trigVolts = inputs[TRIGGER_INPUT].getVoltage();
		bool hit = trigger.process(trigVolts, 0.2f, 0.6f);
		if (hit) {
			velocity = clamp(trigVolts / 10.f, 0.f, 1.f);
			velocityWindow = VELOCITY_WINDOW_S;
			attacking = true;
			sweep = 0.f;
			modeDSweep = 0.f;
			sampleHold = random::uniform() * 2.f - 1.f;
		}
		else if (velocityWindow > 0.f) {
			velocityWindow -= dt;
			velocity = std::max(velocity, clamp(trigVolts / 10.f, 0.f, 1.f));
		}
		if (hit || velocityWindow > 0.f)
			modeDDepth = MODE_D_SWEEP_OCT * velocity;
		// Envelopes: a short attack, then an exponential decay (DECAY + DECAY CV, 10 V = full fader).
		// `envelope` is the 0..1 shape the filter follows. `amp` is the VCA: its attack ramps from
		// wherever it is to this hit's velocity, up or down, so a soft hit after a loud one neither
		// clicks nor overshoots.
		float decayKnob = clamp(params[DECAY_PARAM].getValue() + inputs[DECAY_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float decayT60 = DECAY_MIN_S * std::pow(DECAY_RANGE, decayKnob);
		float decayFactor = std::exp(-6.91f * dt / decayT60);
		float attackStep = dt / ATTACK_S;
		if (attacking) {
			envelope = std::min(envelope + attackStep, 1.f);
			amp = (amp < velocity) ? std::min(amp + attackStep, velocity) : std::max(amp - attackStep, velocity);
			if (envelope >= 1.f && amp == velocity && velocityWindow <= 0.f)
				attacking = false;
		}
		else {
			envelope *= decayFactor;
			amp *= decayFactor;
		}

		// Pitch, in octaves above TUNE_MIN_HZ.
		float pitch = params[TUNE_PARAM].getValue() * TUNE_OCTAVES + inputs[PITCH_INPUT].getVoltage();
		if (m.velocitySweep) {
			modeDSweep += (1.f - modeDSweep) * (1.f - std::exp(-dt / MODE_D_SWEEP_S));
			pitch -= modeDDepth * (1.f - modeDSweep);
		}
		else {
			float sweepDepth = params[SWEEP_PARAM].getValue() * SWEEP_MAX_OCT;
			if (sweepDepth > 0.f) {
				float sweepTime = SWEEP_SLOW_S * std::pow(SWEEP_SPEED_RANGE, params[SWEEP_SPEED_PARAM].getValue());
				sweep += (1.f - sweep) * (1.f - std::exp(-dt / sweepTime));
				float direction = params[SWEEP_DIR_PARAM].getValue() > 0.5f ? 1.f : -1.f;
				pitch += direction * sweepDepth * sweep;
			}
		}
		float filterPitch = pitch;

		// LFO and sample-and-hold on pitch.
		float lfoHz = LFO_MIN_HZ * std::pow(LFO_SPEED_RANGE, params[LFO_SPEED_PARAM].getValue());
		lfoPhase += lfoHz * dt;
		if (lfoPhase >= 1.f) {
			lfoPhase -= std::floor(lfoPhase);
			sampleHold = random::uniform() * 2.f - 1.f;
		}
		float lfo;
		if (params[LFO_SHAPE_PARAM].getValue() > 0.5f)
			lfo = lfoPhase < 0.5f ? 4.f * lfoPhase - 1.f : 3.f - 4.f * lfoPhase;
		else
			lfo = lfoPhase < 0.5f ? 1.f : -1.f;
		float lfoDepth = params[LFO_PARAM].getValue();
		pitch += lfo * lfoDepth * lfoDepth * LFO_MAX_OCT;
		if (params[SH_PARAM].getValue() > 0.5f)
			pitch += sampleHold * SH_OCT;

		// Oscillators and noise, mixed by the mode.
		float freq = TUNE_MIN_HZ * std::pow(2.f, clamp(pitch, -4.f, 12.f));
		float nyquist = 0.5f * args.sampleRate;
		float mod = osc2.process(m.wave2, std::min(freq * m.ratio2, nyquist) * dt);
		float freq1 = freq;
		if (m.fmIndex > 0.f)
			freq1 = freq * (1.f + m.fmIndex * mod);
		float carrier = osc1.process(m.wave1, clamp(freq1, -nyquist, nyquist) * dt);
		float mix = m.level1 * carrier;
		if (m.fmIndex <= 0.f)
			mix += m.level2 * mod;
		if (m.noise > 0.f)
			mix += m.noise * (random::uniform() * 2.f - 1.f);

		// Filter: TUNE sets where it rests; WIDTH (+ FILTER CV) opens it at the hit, closing with the decay.
		float width = clamp(params[WIDTH_PARAM].getValue() + inputs[FILTER_CV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
		float cutoffOct = filterPitch + FILTER_REST_OCT + width * WIDTH_OCT * envelope;
		float cutoff = clamp(TUNE_MIN_HZ * std::pow(2.f, cutoffOct), 20.f, 0.45f * args.sampleRate);
		float out = filter.process(mix, cutoff, args.sampleRate);

		outputs[AUDIO_OUTPUT].setVoltage(OUTPUT_VOLTS * out * amp);
		lights[TRIGGER_LIGHT].setBrightness(amp);
		lights[LFO_LIGHT].setBrightness((lfo + 1.f) / 2.f);
		lights[FADER_LIGHT + 0].setBrightness(params[SWEEP_PARAM].getValue());
		lights[FADER_LIGHT + 1].setBrightness(params[TUNE_PARAM].getValue());
		lights[FADER_LIGHT + 2].setBrightness(decayKnob);
		lights[FADER_LIGHT + 3].setBrightness(width);
		lights[FADER_LIGHT + 4].setBrightness(lfoDepth);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "mode", json_integer(manualMode));
		json_object_set_new(rootJ, "gateAdvance", json_boolean(gateAdvance));
		json_object_set_new(rootJ, "lastDirection", json_integer(lastDirection));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		if (json_t* j = json_object_get(rootJ, "mode"))
			manualMode = mode = wrapMode((int) json_integer_value(j));
		if (json_t* j = json_object_get(rootJ, "gateAdvance"))
			gateAdvance = json_is_true(j);
		if (json_t* j = json_object_get(rootJ, "lastDirection"))
			lastDirection = json_integer_value(j) < 0 ? -1 : 1;
	}
};


/** The OSC MODE letter, lit blue on a dark window, like the SY0.5's LED display. A small dot in the
corner shows that the MODE jack is in gate-advance. Drawn with nanovg, so it is a display on the
MetaModule. */
struct ModeDisplay : TransparentWidget {
	SynnDrum* module;

	ModeDisplay(Vec centerMm, SynnDrum* module) {
		this->module = module;
		box.size = mm2px(Vec(9.f, 10.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, nvgRGB(0x14, 0x14, 0x14));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x5a, 0x5a, 0x5a));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);

		std::shared_ptr<window::Font> font = loadPanelFont();
		if (font && font->handle >= 0) {
			char text[2] = {MODES[module ? module->mode : 0].letter, 0};
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, 30.f);
			nvgFillColor(args.vg, COLOR_BLUE);
			nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
			nvgText(args.vg, box.size.x / 2, box.size.y / 2 + 1.f, text, NULL);
		}
		if (module && module->gateAdvance) {
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, box.size.x - 3.5f, box.size.y - 3.5f, 1.6f);
			nvgFillColor(args.vg, COLOR_BLUE);
			nvgFill(args.vg);
		}
	}
};


/** Small white drawings for the switch positions: sweep up/down ramps and the LFO shapes. */
struct SwitchIcon : PanelArt {
	enum Kind { RAMP_UP, RAMP_DOWN, TRIANGLE, SQUARE };
	Kind kind;

	SwitchIcon(Vec centerMm, Kind kind) {
		this->kind = kind;
		box.size = mm2px(Vec(5.f, 3.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	void draw(const DrawArgs& args) override {
		float l = 1.f, r = box.size.x - 1.f, t = 1.f, b = box.size.y - 1.f;
		float mid = (l + r) / 2;
		nvgBeginPath(args.vg);
		switch (kind) {
			case RAMP_UP:
				nvgMoveTo(args.vg, l, b);
				nvgLineTo(args.vg, r, t);
				break;
			case RAMP_DOWN:
				nvgMoveTo(args.vg, l, t);
				nvgLineTo(args.vg, r, b);
				break;
			case TRIANGLE:
				nvgMoveTo(args.vg, l, b);
				nvgLineTo(args.vg, (l + mid) / 2, t);
				nvgLineTo(args.vg, mid, b);
				nvgLineTo(args.vg, (mid + r) / 2, t);
				nvgLineTo(args.vg, r, b);
				break;
			case SQUARE:
				nvgMoveTo(args.vg, l, b);
				nvgLineTo(args.vg, l, t);
				nvgLineTo(args.vg, mid, t);
				nvgLineTo(args.vg, mid, b);
				nvgLineTo(args.vg, r, b);
				nvgLineTo(args.vg, r, t);
				break;
		}
		nvgStrokeColor(args.vg, COLOR_TEXT);
		nvgStrokeWidth(args.vg, 1.2f);
		nvgLineJoin(args.vg, NVG_MITER);
		nvgStroke(args.vg);
	}
};


struct SynnDrumWidget : ModuleWidget {
	void label(float xMm, float yMm, std::string text, float size = LABEL_SIZE) {
		addPanelArt(this, new PanelLabel(Vec(xMm, yMm), text, size));
	}

	SynnDrumWidget(SynnDrum* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SynnDrum.svg")));
		addScrews(this);

		label(25.4f, 6.f, "SynnDrum", TITLE_SIZE);

		// OSC MODE: increment on the left, the letter, decrement on the right.
		const float modeY = 20.3f;
		label(25.4f, 13.6f, "MODE", 8.f);
		addChild(new ModeDisplay(Vec(25.4f, modeY), module));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(13.f, modeY)), module, SynnDrum::MODE_UP_PARAM));
		addParam(createParamCentered<VCVButton>(mm2px(Vec(37.8f, modeY)), module, SynnDrum::MODE_DOWN_PARAM));
		label(13.f, modeY + 5.f, "+", 12.f);
		label(37.8f, modeY + 5.f, "–", 12.f);

		// Five faders.
		const float colX[5] = {6.6f, 16.f, 25.4f, 34.8f, 44.2f};
		const char* faderNames[5] = {"SWEEP", "TUNE", "DECAY", "WIDTH", "LFO"};
		const int faderParams[5] = {SynnDrum::SWEEP_PARAM, SynnDrum::TUNE_PARAM, SynnDrum::DECAY_PARAM,
			SynnDrum::WIDTH_PARAM, SynnDrum::LFO_PARAM};
		const float faderY = 47.f;
		for (int i = 0; i < 5; i++) {
			label(colX[i], 30.f, faderNames[i], 8.5f);
			addParam(createLightParamCentered<BlueFader>(mm2px(Vec(colX[i], faderY)), module, faderParams[i], SynnDrum::FADER_LIGHT + i));
		}

		// Speed knobs under the SWEEP and LFO faders, and the three switches between them.
		const float rowY = 72.f;
		addParam(createParamCentered<SmallKnob>(mm2px(Vec(colX[0], rowY)), module, SynnDrum::SWEEP_SPEED_PARAM));
		addParam(createParamCentered<SmallKnob>(mm2px(Vec(colX[4], rowY)), module, SynnDrum::LFO_SPEED_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(colX[1], rowY)), module, SynnDrum::SWEEP_DIR_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(colX[2], rowY)), module, SynnDrum::SH_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(colX[3], rowY)), module, SynnDrum::LFO_SHAPE_PARAM));
		addPanelArt(this, new SwitchIcon(Vec(colX[1], rowY - 6.f), SwitchIcon::RAMP_UP));
		addPanelArt(this, new SwitchIcon(Vec(colX[1], rowY + 6.f), SwitchIcon::RAMP_DOWN));
		label(colX[2], rowY - 6.f, "ON", 7.f);
		label(colX[2], rowY + 6.f, "OFF", 7.f);
		addPanelArt(this, new SwitchIcon(Vec(colX[3], rowY - 6.f), SwitchIcon::TRIANGLE));
		addPanelArt(this, new SwitchIcon(Vec(colX[3], rowY + 6.f), SwitchIcon::SQUARE));
		const float rowLabelY = 82.5f;
		label(colX[0], rowLabelY, "SPEED", 7.5f);
		label(colX[1], rowLabelY, "DIR", 7.5f);
		label(colX[2], rowLabelY, "S/H", 7.5f);
		label(colX[3], rowLabelY, "SHAPE", 7.5f);
		label(colX[4], rowLabelY, "SPEED", 7.5f);

		// Lights: trigger (left) and LFO (right).
		addChild(createLightCentered<SmallLight<BrightBlueLight>>(mm2px(Vec(colX[0], 86.5f)), module, SynnDrum::TRIGGER_LIGHT));
		addChild(createLightCentered<SmallLight<BrightBlueLight>>(mm2px(Vec(colX[4], 86.5f)), module, SynnDrum::LFO_LIGHT));

		// Jacks: DECAY and FILTER CV, then TRIGGER, PITCH, MODE and OUT along the bottom.
		const float cvY = 99.5f;
		label(colX[0], cvY - 6.8f, "DECAY", 8.f);
		label(colX[4], cvY - 6.8f, "FILTER", 8.f);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[0], cvY)), module, SynnDrum::DECAY_CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[4], cvY)), module, SynnDrum::FILTER_CV_INPUT));

		const float ioY = 115.5f;
		const float ioX[4] = {6.6f, 19.13f, 31.67f, 44.2f};
		label(ioX[0], ioY - 6.8f, "TRIG", 8.f);
		label(ioX[1], ioY - 6.8f, "PITCH", 8.f);
		label(ioX[2], ioY - 6.8f, "MODE", 8.f);
		label(ioX[3], ioY - 6.8f, "OUT", 8.f);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(ioX[0], ioY)), module, SynnDrum::TRIGGER_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(ioX[1], ioY)), module, SynnDrum::PITCH_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(ioX[2], ioY)), module, SynnDrum::MODE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(ioX[3], ioY)), module, SynnDrum::AUDIO_OUTPUT));

#ifndef METAMODULE
		stripForPanelBaking(this);
#endif
	}

	void appendContextMenu(Menu* menu) override {
		SynnDrum* m = dynamic_cast<SynnDrum*>(this->module);
		if (!m)
			return;
		menu->addChild(new MenuSeparator);
		std::vector<std::string> modeNames;
		for (int i = 0; i < NUM_MODES; i++)
			modeNames.push_back(std::string(1, MODES[i].letter) + ": " + MODES[i].description);
		menu->addChild(createIndexSubmenuItem("OSC mode", modeNames,
			[=]() { return m->manualMode; },
			[=](size_t i) { m->manualMode = (int) i; }));
		menu->addChild(createIndexSubmenuItem("MODE jack", {"CV selects the mode (0-5 V)", "Gate advances the mode"},
			[=]() { return m->gateAdvance ? 1 : 0; },
			[=](size_t i) { m->gateAdvance = (i == 1); }));
	}
};


Model* modelSynnDrum = createModel<SynnDrum, SynnDrumWidget>("SynnDrum");
