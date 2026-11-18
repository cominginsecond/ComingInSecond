#include "plugin.hpp"
#include <cmath>


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


/** One latching piano key. Clicking toggles the note in or out of that step's scale. */
struct PianoKey : app::Switch {
	QuantizerSequencer* module = nullptr;
	int step = 0;
	int note = 0;

	void draw(const DrawArgs& args) override {
		bool black = IS_BLACK[note];
		ParamQuantity* pq = getParamQuantity();
		bool on = pq ? pq->getValue() > 0.5f : DEFAULT_SCALE[note];

		NVGcolor fill;
		if (black)
			fill = on ? nvgRGB(0xd9, 0x7a, 0x00) : nvgRGB(0x26, 0x28, 0x2e);
		else
			fill = on ? nvgRGB(0xff, 0xb8, 0x4d) : nvgRGB(0xf6, 0xf3, 0xec);

		// Columns beyond the sequence length are faded out.
		if (module && step >= module->displayLength)
			nvgGlobalAlpha(args.vg, 0.3f);

		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.4f, 0.4f, box.size.x - 0.8f, box.size.y - 0.8f, 1.2f);
		nvgFillColor(args.vg, fill);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x4a, 0x4d, 0x55));
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);

		// Mark the note currently being output, in the active step's column.
		if (module && module->displayStep == step && module->displayNote == note && on) {
			float r = std::min(box.size.x, box.size.y) * 0.18f;
			float cx = black ? box.size.x / 2 : box.size.x - r * 2.2f;
			nvgBeginPath(args.vg);
			nvgCircle(args.vg, cx, box.size.y / 2, r);
			nvgFillColor(args.vg, nvgRGB(0xd0, 0x1c, 0x1c));
			nvgFill(args.vg);
		}
	}
};


/** A text label drawn with Rack's built-in font, since panel SVG <text> is not rendered. */
struct PanelLabel : TransparentWidget {
	std::string text;
	float fontSize;
	NVGcolor color;

	PanelLabel(Vec centerMm, std::string text, float fontSize, NVGcolor color) {
		this->text = text;
		this->fontSize = fontSize;
		this->color = color;
		box.size = mm2px(Vec(20.f, 5.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, fontSize);
		nvgFillColor(args.vg, color);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, box.size.x / 2, box.size.y / 2, text.c_str(), NULL);
	}
};


struct QuantizerSequencerWidget : ModuleWidget {
	// Keyboard geometry (mm). Each column is a piano keyboard rotated so C is at the bottom
	// and the black keys sit on the left.
	static constexpr float COL_X0 = 4.72f;      // left edge of column 1
	static constexpr float COL_PITCH = 10.5f;   // distance between columns
	static constexpr float KEY_W = 8.5f;        // white key width
	static constexpr float KEY_H = 7.f;         // white key height
	static constexpr float BLACK_W = 5.2f;
	static constexpr float BLACK_H = 4.4f;
	static constexpr float KB_TOP = 22.f;       // top of the keyboard (the B key)

	void label(float xMm, float yMm, std::string text, float size = 7.f, bool light = false) {
		NVGcolor color = light ? nvgRGB(0xe8, 0xe4, 0xdc) : nvgRGB(0x2b, 0x2f, 0x36);
		addChild(new PanelLabel(Vec(xMm, yMm), text, size, color));
	}

	void addKey(QuantizerSequencer* module, int s, int n, Vec posMm, Vec sizeMm) {
		PianoKey* key = createParam<PianoKey>(mm2px(posMm), module, QuantizerSequencer::KEY_PARAM + s * NUM_NOTES + n);
		key->box.size = mm2px(sizeMm);
		key->module = module;
		key->step = s;
		key->note = n;
		addParam(key);
	}

	QuantizerSequencerWidget(QuantizerSequencer* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/QuantizerSequencer.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		label(45.72f, 6.f, "QSEQ 8", 11.f, true);

		const float kbBottom = KB_TOP + 7 * KEY_H;
		for (int s = 0; s < NUM_STEPS; s++) {
			float x = COL_X0 + s * COL_PITCH;
			float cx = x + KEY_W / 2;
			label(cx, 14.5f, std::to_string(s + 1), 6.5f);
			addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(cx, 18.5f)), module, QuantizerSequencer::STEP_LIGHT + s));

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
		label(2.4f, kbBottom - KEY_H / 2, "C", 5.f);

		// Jacks.
		label(10.f, 92.f, "CLOCK");
		label(22.f, 92.f, "RESET");
		label(34.f, 92.f, "LENGTH", 6.f);
		label(47.f, 92.f, "CV IN");
		label(59.f, 92.f, "S&H");
		label(78.f, 92.f, "OUT", 7.f, true);
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.f, 101.f)), module, QuantizerSequencer::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.f, 101.f)), module, QuantizerSequencer::RESET_INPUT));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(34.f, 101.f)), module, QuantizerSequencer::LENGTH_PARAM));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(47.f, 101.f)), module, QuantizerSequencer::CV_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(59.f, 101.f)), module, QuantizerSequencer::SH_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(78.f, 101.f)), module, QuantizerSequencer::CV_OUTPUT));
	}
};


Model* modelQuantizerSequencer = createModel<QuantizerSequencer, QuantizerSequencerWidget>("QuantizerSequencer");
