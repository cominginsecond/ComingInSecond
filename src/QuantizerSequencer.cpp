#include "plugin.hpp"
#include <cmath>
#include <cstdlib>


static const int NUM_STEPS = 8;
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


// Panel colours.
static const NVGcolor COLOR_TEXT = nvgRGB(0xff, 0xff, 0xff);
static const NVGcolor COLOR_BLUE = nvgRGB(0x1e, 0x90, 0xff);       // LEDs and lit white keys
static const NVGcolor COLOR_BLUE_DARK = nvgRGB(0x10, 0x60, 0xd8);  // lit black keys


/** Bright blue LED used for the step indicators. */
template <typename TBase = GrayModuleLightWidget>
struct TBrightBlueLight : TBase {
	TBrightBlueLight() {
		this->addBaseColor(COLOR_BLUE);
	}
};
using BrightBlueLight = TBrightBlueLight<>;


/** Franklin Gothic Medium ships with Windows. Fall back to Rack's own font if it can't be loaded. */
static std::shared_ptr<window::Font> loadPanelFont() {
	std::shared_ptr<window::Font> font;
#if defined ARCH_WIN && !defined METAMODULE
	const char* windir = std::getenv("WINDIR");
	std::string fontsDir = std::string(windir ? windir : "C:/Windows") + "/Fonts/framd.ttf";
	font = APP->window->loadFont(fontsDir);
#endif
	if (!font)
		font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
	return font;
}


#ifdef METAMODULE
/** MetaModule version of a piano key. The MetaModule engine draws param widgets itself and
ignores draw() overrides, so the key uses two image frames (off/on). The SDK maps
res/keys/NAME.svg to the PNGs in metamodule/assets/keys. The playing-note dot and the dimming of
unused columns are drawn by KeyboardOverlay instead. */
struct PianoKey : app::SvgSwitch {
	QuantizerSequencer* module = nullptr;
	int step = 0;
	int note = 0;

	void initFrames() {
		const char* color = IS_BLACK[note] ? "black" : "white";
		addFrame(Svg::load(asset::plugin(pluginInstance, string::f("res/keys/%s-off.svg", color))));
		addFrame(Svg::load(asset::plugin(pluginInstance, string::f("res/keys/%s-on.svg", color))));
	}
};


/** Draws on top of the keyboard: the white dot on the note being output, and darkening over
columns past the sequence length. */
struct KeyboardOverlay : TransparentWidget {
	QuantizerSequencer* module = nullptr;
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
			module->params[QuantizerSequencer::KEY_PARAM + step * NUM_NOTES + note].getValue() > 0.5f) {
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
struct PianoKey : app::Switch {
	QuantizerSequencer* module = nullptr;
	int step = 0;
	int note = 0;

	// Only the MetaModule key uses image frames.
	void initFrames() {}

	void draw(const DrawArgs& args) override {
		bool black = IS_BLACK[note];
		ParamQuantity* pq = getParamQuantity();
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

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.4f, 0.4f, box.size.x - 0.8f, box.size.y - 0.8f, 1.2f);
		nvgFillColor(args.vg, fill);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, stroke);
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);

		// Mark the note currently being output, in the active step's column.
		if (module && module->displayStep == step && module->displayNote == note && on) {
			float r = std::min(box.size.x, box.size.y) * 0.18f;
			float cx = black ? box.size.x / 2 : box.size.x - r * 2.2f;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, box.size.y / 2, r);
			nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
			nvgFill(args.vg);
		}
	}
};
#endif


/** A text label drawn in code, since panel SVG <text> is not rendered by Rack. */
struct PanelLabel : TransparentWidget {
	std::string text;
	float fontSize;

	PanelLabel(Vec centerMm, std::string text, float fontSize) {
		this->text = text;
		this->fontSize = fontSize;
		box.size = mm2px(Vec(20.f, 6.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = loadPanelFont();
		if (!font || font->handle < 0)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, fontSize);
		nvgFillColor(args.vg, COLOR_TEXT);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, box.size.x / 2, box.size.y / 2, text.c_str(), NULL);
	}
};


struct QuantizerSequencerWidget : ModuleWidget {
	// Keyboard geometry (mm). Each column is a piano keyboard rotated so C is at the bottom
	// and the black keys sit on the left.
	static constexpr float COL_X0 = 5.8f;       // left edge of column 1
	static constexpr float COL_PITCH = 11.5f;   // distance between columns
	static constexpr float KEY_W = 9.5f;        // white key width
	static constexpr float KEY_H = 7.f;         // white key height
	static constexpr float BLACK_W = 5.8f;
	static constexpr float BLACK_H = 4.4f;
	static constexpr float KB_TOP = 23.f;       // top of the keyboard (the B key)

	// Text sizes (px).
	static constexpr float TITLE_SIZE = 15.f;
	static constexpr float LABEL_SIZE = 10.f;

	void label(float xMm, float yMm, std::string text, float size = LABEL_SIZE) {
		addChild(new PanelLabel(Vec(xMm, yMm), text, size));
	}

#ifdef METAMODULE
	KeyboardOverlay* overlay = nullptr;
#endif

	void addKey(QuantizerSequencer* module, int s, int n, Vec posMm, Vec sizeMm) {
		PianoKey* key = createParam<PianoKey>(mm2px(posMm), module, QuantizerSequencer::KEY_PARAM + s * NUM_NOTES + n);
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

	QuantizerSequencerWidget(QuantizerSequencer* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/QuantizerSequencer.svg")));

		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		label(50.8f, 6.f, "QSEQ 8", TITLE_SIZE);

#ifdef METAMODULE
		// Created now so addKey() can record key positions; added after the keys so it draws on top.
		overlay = new KeyboardOverlay;
		overlay->module = module;
		overlay->box = Rect(Vec(0, 0), box.size);
#endif

		const float kbBottom = KB_TOP + 7 * KEY_H;
		for (int s = 0; s < NUM_STEPS; s++) {
			float x = COL_X0 + s * COL_PITCH;
			float cx = x + KEY_W / 2;
			label(cx, 14.5f, std::to_string(s + 1));
			addChild(createLightCentered<MediumLight<BrightBlueLight>>(mm2px(Vec(cx, 19.f)), module, QuantizerSequencer::STEP_LIGHT + s));

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
		label(2.9f, kbBottom - KEY_H / 2, "C", 8.f);

		// Jacks: step controls | quantizer inputs | output.
		const float labelY = 90.5f;
		const float jackY = 100.5f;
		label(10.f, labelY, "CLOCK");
		label(22.f, labelY, "RESET");
		label(34.f, labelY, "LENGTH");
		label(46.f, labelY, "SCALE");
		label(60.f, labelY, "CV IN");
		label(72.f, labelY, "S&H");
		label(89.f, labelY, "OUT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.f, jackY)), module, QuantizerSequencer::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.f, jackY)), module, QuantizerSequencer::RESET_INPUT));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(34.f, jackY)), module, QuantizerSequencer::LENGTH_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(46.f, jackY)), module, QuantizerSequencer::SELECT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(60.f, jackY)), module, QuantizerSequencer::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(72.f, jackY)), module, QuantizerSequencer::SH_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(89.f, jackY)), module, QuantizerSequencer::CV_OUTPUT));
	}
};


Model* modelQuantizerSequencer = createModel<QuantizerSequencer, QuantizerSequencerWidget>("QuantizerSequencer");
