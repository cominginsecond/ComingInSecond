#include "house_style.hpp"


static const int NUM_NOTES = 12;
static const int MAX_CHANNELS = 16;

static const char* NOTE_NAMES[NUM_NOTES] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const bool IS_BLACK[NUM_NOTES] = {false, true, false, true, false, false, true, false, true, false, true, false};
// Every column starts out as C major.
static const bool DEFAULT_SCALE[NUM_NOTES] = {true, false, true, false, true, true, false, true, false, true, false, true};

/** Snaps a 1V/oct voltage to the nearest enabled note (bit n of `mask` = pitch class n, 0 = C).
An empty scale passes the voltage through unchanged. */
static float quantize(float voltage, uint16_t mask) {
	if (mask == 0)
		return voltage;
	float semis = voltage * 12.f;
	int baseOctave = (int) std::floor(semis / 12.f);
	float best = 0.f;
	float bestDist = INFINITY;
	// Searching the neighbouring octaves too lets values near an octave boundary snap across it.
	for (int octave = baseOctave - 1; octave <= baseOctave + 1; octave++) {
		for (int note = 0; note < NUM_NOTES; note++) {
			if (!(mask & (1 << note)))
				continue;
			float candidate = octave * 12.f + note;
			float dist = std::fabs(semis - candidate);
			if (dist < bestDist) {
				bestDist = dist;
				best = candidate;
			}
		}
	}
	return best / 12.f;
}


/** A sequencer of NUM_STEPS quantizers. QSeq 4, QSeq 8 and QSeq 16 are this module with
different step counts. */
template <int NUM_STEPS>
struct QuantizerSequencer : Module {
	enum ParamId {
		// Key for note n of step s is KEY_PARAM + s * NUM_NOTES + n.
		ENUMS(KEY_PARAM, NUM_STEPS * NUM_NOTES),
		LENGTH_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CLOCK_INPUT,
		RESET_INPUT,
		CV_INPUT,
		SH_INPUT,
		SELECT_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		CV_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(STEP_LIGHT, NUM_STEPS),
		LIGHTS_LEN
	};

	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger resetTrigger;
	dsp::SchmittTrigger shTriggers[MAX_CHANNELS];
	float held[MAX_CHANNELS] = {};
	// -1 means "before the first step": the next clock goes to step 0.
	int step = -1;

	// Read by the panel to highlight the note being output (channel 1).
	int displayStep = 0;
	int displayNote = -1;
	int displayLength = NUM_STEPS;

	QuantizerSequencer() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int s = 0; s < NUM_STEPS; s++) {
			for (int n = 0; n < NUM_NOTES; n++) {
				configSwitch(KEY_PARAM + s * NUM_NOTES + n, 0.f, 1.f, DEFAULT_SCALE[n] ? 1.f : 0.f,
					string::f("Step %d %s", s + 1, NOTE_NAMES[n]), {"Off", "On"});
			}
		}
		configParam(LENGTH_PARAM, 1.f, NUM_STEPS, NUM_STEPS, "Length", " steps");
		getParamQuantity(LENGTH_PARAM)->snapEnabled = true;

		configInput(CLOCK_INPUT, "Clock (advance to next step)");
		configInput(RESET_INPUT, "Reset (next clock goes to step 1)");
		configInput(CV_INPUT, "CV to quantize (1V/oct)");
		configInput(SH_INPUT, "Sample & hold gate");
		configInput(SELECT_INPUT, "Scale select (0-10V across the length; overrides the clock)");
		configOutput(CV_OUTPUT, "Quantized CV (1V/oct)");
		configBypass(CV_INPUT, CV_OUTPUT);
	}

	void onReset() override {
		step = -1;
		for (int c = 0; c < MAX_CHANNELS; c++)
			held[c] = 0.f;
	}

	uint16_t scaleMask(int s) {
		uint16_t mask = 0;
		for (int n = 0; n < NUM_NOTES; n++) {
			if (params[KEY_PARAM + s * NUM_NOTES + n].getValue() > 0.5f)
				mask |= 1 << n;
		}
		return mask;
	}

	void process(const ProcessArgs& args) override {
		if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f))
			step = -1;
		int length = clamp((int) params[LENGTH_PARAM].getValue(), 1, NUM_STEPS);
		if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f))
			step++;
		// Wrap after the last step, or right away if the length was turned down below the current step.
		if (step >= length)
			step = 0;
		// A patched select CV picks the step directly: 0-10V is split evenly across steps 1..length.
		// Writing it into `step` means the clock carries on from here once the cable is removed.
		if (inputs[SELECT_INPUT].isConnected()) {
			float v = inputs[SELECT_INPUT].getVoltage();
			step = clamp((int) std::floor(v / 10.f * length), 0, length - 1);
		}

		int current = std::max(step, 0);
		uint16_t mask = scaleMask(current);

		// Polyphonic: one output channel per CV input channel.
		int channels = std::max(1, inputs[CV_INPUT].getChannels());
		bool shConnected = inputs[SH_INPUT].isConnected();
		for (int c = 0; c < channels; c++) {
			// With nothing patched to S&H, the output tracks the input continuously.
			bool sample = true;
			if (shConnected) {
				// A mono S&H gate applies to every channel.
				sample = shTriggers[c].process(inputs[SH_INPUT].getPolyVoltage(c), 0.1f, 1.f);
			}
			if (sample)
				held[c] = quantize(inputs[CV_INPUT].getVoltage(c), mask);
			outputs[CV_OUTPUT].setVoltage(held[c], c);
		}
		outputs[CV_OUTPUT].setChannels(channels);

		displayStep = current;
		displayLength = length;
		int semis = (int) std::round(held[0] * 12.f);
		displayNote = ((semis % NUM_NOTES) + NUM_NOTES) % NUM_NOTES;

		for (int s = 0; s < NUM_STEPS; s++)
			lights[STEP_LIGHT + s].setBrightness(s == current ? 1.f : 0.f);
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "step", json_integer(step));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* stepJ = json_object_get(rootJ, "step");
		if (stepJ)
			step = clamp((int) json_integer_value(stepJ), -1, NUM_STEPS - 1);
	}
};


#ifdef METAMODULE
/** MetaModule version of a piano key. The MetaModule engine draws param widgets itself and
ignores draw() overrides, so the key uses two image frames (off/on). The SDK maps
res/keys/NAME.svg to the PNGs in metamodule/assets/keys. The playing-note dot and the dimming of
unused columns are drawn by KeyboardOverlay instead. */
template <int NUM_STEPS>
struct PianoKey : app::SvgSwitch {
	QuantizerSequencer<NUM_STEPS>* module = nullptr;
	int step = 0;
	int note = 0;

	void initFrames() {
		const char* color = IS_BLACK[note] ? "black" : "white";
		this->addFrame(Svg::load(asset::plugin(pluginInstance, string::f("res/keys/%s-off.svg", color))));
		this->addFrame(Svg::load(asset::plugin(pluginInstance, string::f("res/keys/%s-on.svg", color))));
	}
};


/** Draws on top of the keyboard: the white dot on the note being output, and darkening over
columns past the sequence length. */
template <int NUM_STEPS>
struct KeyboardOverlay : TransparentWidget {
	QuantizerSequencer<NUM_STEPS>* module = nullptr;
	// Key rectangles in px, relative to this widget.
	Rect keyRect[NUM_STEPS][NUM_NOTES];

	void draw(const DrawArgs& args) override {
		if (!module)
			return;
		int length = module->displayLength;
		int step = module->displayStep;
		int note = module->displayNote;

		for (int s = length; s < NUM_STEPS; s++) {
			// The C key is the bottom of the column and the B key the top.
			Rect bottom = keyRect[s][0];
			Rect top = keyRect[s][NUM_NOTES - 1];
			nvgBeginPath(args.vg);
			nvgRect(args.vg, top.pos.x, top.pos.y, bottom.size.x, bottom.pos.y + bottom.size.y - top.pos.y);
			nvgFillColor(args.vg, nvgRGBA(0x00, 0x00, 0x00, 0xb0));
			nvgFill(args.vg);
		}

		if (step >= 0 && step < NUM_STEPS && note >= 0 && note < NUM_NOTES &&
			module->params[QuantizerSequencer<NUM_STEPS>::KEY_PARAM + step * NUM_NOTES + note].getValue() > 0.5f) {
			Rect r = keyRect[step][note];
			float radius = std::min(r.size.x, r.size.y) * 0.18f;
			float cx = IS_BLACK[note] ? r.pos.x + r.size.x / 2 : r.pos.x + r.size.x - radius * 2.2f;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, r.pos.y + r.size.y / 2, radius);
			nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
			nvgFill(args.vg);
		}
	}
};
#else
/** One latching piano key. Clicking toggles the note in or out of that step's scale. */
template <int NUM_STEPS>
struct PianoKey : app::Switch {
	QuantizerSequencer<NUM_STEPS>* module = nullptr;
	int step = 0;
	int note = 0;

	// Only the MetaModule key uses image frames.
	void initFrames() {}

	void draw(const DrawArgs& args) override {
		bool black = IS_BLACK[note];
		ParamQuantity* pq = this->getParamQuantity();
		bool on = pq ? pq->getValue() > 0.5f : DEFAULT_SCALE[note];

		NVGcolor fill;
		NVGcolor stroke;
		if (black) {
			fill = on ? COLOR_BLUE_DARK : nvgRGB(0x2a, 0x2a, 0x2a);
			stroke = nvgRGB(0x80, 0x80, 0x80);
		}
		else {
			fill = on ? COLOR_BLUE : nvgRGB(0xea, 0xea, 0xea);
			stroke = nvgRGB(0x00, 0x00, 0x00);
		}

		// Columns beyond the sequence length are faded out by darkening toward the black panel.
		// The colours stay opaque so the white keys' edges don't show through the black keys.
		if (module && step >= module->displayLength) {
			const NVGcolor panel = nvgRGB(0x00, 0x00, 0x00);
			fill = nvgLerpRGBA(panel, fill, 0.3f);
			stroke = nvgLerpRGBA(panel, stroke, 0.3f);
		}

		Vec size = this->box.size;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.4f, 0.4f, size.x - 0.8f, size.y - 0.8f, 1.2f);
		nvgFillColor(args.vg, fill);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, stroke);
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);

		// Mark the note currently being output, in the active step's column.
		if (module && module->displayStep == step && module->displayNote == note && on) {
			float r = std::min(size.x, size.y) * 0.18f;
			float cx = black ? size.x / 2 : size.x - r * 2.2f;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, size.y / 2, r);
			nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
			nvgFill(args.vg);
		}
	}
};
#endif


/** Where the jacks and the length knob go on each panel size (mm, centres). */
struct JackLayout {
	Vec clock, reset, length, select, cv, sh, out;
};

/** QSeq 8 (20HP) and QSeq 16 (38HP): one row of jacks along the bottom, centred on the panel. */
static JackLayout singleRowLayout(float panelWidth) {
	const float dx = (panelWidth - 101.6f) / 2;
	const float y = 100.5f;
	return {
		Vec(10.f + dx, y), Vec(22.f + dx, y), Vec(34.f + dx, y), Vec(46.f + dx, y),
		Vec(60.f + dx, y), Vec(72.f + dx, y), Vec(89.f + dx, y),
	};
}

/** QSeq 4 (12HP) is too narrow for one row: step controls above, quantizer jacks below. */
static JackLayout twoRowLayout() {
	const float y1 = 89.5f;
	const float y2 = 115.f;
	return {
		Vec(12.48f, y1), Vec(24.48f, y1), Vec(36.48f, y1), Vec(48.48f, y1),
		Vec(14.23f, y2), Vec(26.23f, y2), Vec(43.23f, y2),
	};
}


template <int NUM_STEPS>
struct QuantizerSequencerWidget : ModuleWidget {
	using QSeq = QuantizerSequencer<NUM_STEPS>;

	// Keyboard geometry (mm). Each column is a piano keyboard rotated so C is at the bottom
	// and the black keys sit on the left.
	static constexpr float COL_PITCH = 11.5f;   // distance between columns
	static constexpr float KEY_W = 9.5f;        // white key width
	static constexpr float KEY_H = 7.f;         // white key height
	static constexpr float BLACK_W = 5.8f;
	static constexpr float BLACK_H = 4.4f;
	static constexpr float KB_TOP = 23.f;       // top of the keyboard (the B key)

	// Panel width (mm) to match res/QuantizerSequencer*.svg: 12HP, 20HP or 38HP.
	static constexpr float PANEL_W = NUM_STEPS <= 4 ? 60.96f : NUM_STEPS <= 8 ? 101.6f : 193.04f;
	// Left edge of column 1, centring the keyboard on the panel.
	static constexpr float COL_X0 = (PANEL_W - ((NUM_STEPS - 1) * COL_PITCH + KEY_W)) / 2;

	void label(float xMm, float yMm, std::string text, float size = LABEL_SIZE) {
		addChild(new PanelLabel(Vec(xMm, yMm), text, size));
	}

#ifdef METAMODULE
	KeyboardOverlay<NUM_STEPS>* overlay = nullptr;
#endif

	void addKey(QSeq* module, int s, int n, Vec posMm, Vec sizeMm) {
		PianoKey<NUM_STEPS>* key = createParam<PianoKey<NUM_STEPS>>(mm2px(posMm), module, QSeq::KEY_PARAM + s * NUM_NOTES + n);
		key->module = module;
		key->step = s;
		key->note = n;
		key->initFrames();
		// Set the size after the frames, since adding a frame resizes the widget to the image.
		key->box.size = mm2px(sizeMm);
		addParam(key);
#ifdef METAMODULE
		overlay->keyRect[s][n] = Rect(key->box.pos.minus(overlay->box.pos), key->box.size);
#endif
	}

	static std::string panelPath() {
		return NUM_STEPS == 8 ? "res/QuantizerSequencer.svg" : string::f("res/QuantizerSequencer%d.svg", NUM_STEPS);
	}

	QuantizerSequencerWidget(QSeq* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, panelPath())));

		addScrews(this);

		label(PANEL_W / 2, 6.f, string::f("QSEQ %d", NUM_STEPS), TITLE_SIZE);

#ifdef METAMODULE
		// Created now so addKey() can record key positions; added after the keys so it draws on top.
		overlay = new KeyboardOverlay<NUM_STEPS>;
		overlay->module = module;
		overlay->box = Rect(Vec(0, 0), box.size);
#endif

		const float kbBottom = KB_TOP + 7 * KEY_H;
		for (int s = 0; s < NUM_STEPS; s++) {
			float x = COL_X0 + s * COL_PITCH;
			float cx = x + KEY_W / 2;
			label(cx, 14.5f, std::to_string(s + 1));
			addChild(createLightCentered<MediumLight<BrightBlueLight>>(mm2px(Vec(cx, 19.f)), module, QSeq::STEP_LIGHT + s));

			// White keys first, then black keys on top, so the black keys get clicks where they overlap.
			int whiteIndex = 0;
			for (int n = 0; n < NUM_NOTES; n++) {
				if (IS_BLACK[n])
					continue;
				float y = kbBottom - (whiteIndex + 1) * KEY_H;
				addKey(module, s, n, Vec(x, y), Vec(KEY_W, KEY_H));
				whiteIndex++;
			}
			whiteIndex = 0;
			for (int n = 0; n < NUM_NOTES; n++) {
				if (!IS_BLACK[n]) {
					whiteIndex++;
					continue;
				}
				// A black key sits on the boundary between the white key below it and the one above.
				float boundaryY = kbBottom - whiteIndex * KEY_H;
				addKey(module, s, n, Vec(x, boundaryY - BLACK_H / 2), Vec(BLACK_W, BLACK_H));
			}
		}
#ifdef METAMODULE
		addChild(overlay);
#endif
		label(COL_X0 / 2, kbBottom - KEY_H / 2, "C", 8.f);

		// Jacks: step controls | quantizer inputs | output. Each label sits 10 mm above its jack.
		JackLayout j = NUM_STEPS <= 4 ? twoRowLayout() : singleRowLayout(PANEL_W);
		auto jackLabel = [&](Vec pos, const char* text) {
			label(pos.x, pos.y - 10.f, text);
		};
		jackLabel(j.clock, "CLOCK");
		jackLabel(j.reset, "RESET");
		jackLabel(j.length, "LENGTH");
		jackLabel(j.select, "SCALE");
		jackLabel(j.cv, "CV IN");
		jackLabel(j.sh, "S&H");
		jackLabel(j.out, "OUT");
		addInput(createInputCentered<PJ301MPort>(mm2px(j.clock), module, QSeq::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(j.reset), module, QSeq::RESET_INPUT));
		addParam(createParamCentered<SmallKnob>(mm2px(j.length), module, QSeq::LENGTH_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(j.select), module, QSeq::SELECT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(j.cv), module, QSeq::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(j.sh), module, QSeq::SH_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(j.out), module, QSeq::CV_OUTPUT));
	}
};


// QSeq 8 keeps the original slug so existing patches still load.
Model* modelQuantizerSequencer4 = createModel<QuantizerSequencer<4>, QuantizerSequencerWidget<4>>("QuantizerSequencer4");
Model* modelQuantizerSequencer = createModel<QuantizerSequencer<8>, QuantizerSequencerWidget<8>>("QuantizerSequencer");
Model* modelQuantizerSequencer16 = createModel<QuantizerSequencer<16>, QuantizerSequencerWidget<16>>("QuantizerSequencer16");
