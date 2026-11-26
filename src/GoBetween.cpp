#include "house_style.hpp"


// GOBETWEEN: a gain stage. One knob turns the input down or up by as much as 24 dB, with unity
// gain at noon.

static const float MAX_DB = 24.f;
/** How long the gain stays on the panel after the knob moves. */
static const float GAIN_POPUP_SECONDS = 1.f;
/** Gain smoothing time, so turning the knob doesn't zipper. */
static const float SMOOTH_SECONDS = 0.005f;


struct GoBetween : Module {
	enum ParamId {
		GAIN_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		IN_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	float gain = 1.f;

	GoBetween() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(GAIN_PARAM, -MAX_DB, MAX_DB, 0.f, "Gain", " dB");
		configInput(IN_INPUT, "Signal");
		configOutput(OUT_OUTPUT, "Signal");
		configBypass(IN_INPUT, OUT_OUTPUT);
	}

	void onReset() override {
		gain = 1.f;
	}

	void process(const ProcessArgs& args) override {
		float target = std::pow(10.f, params[GAIN_PARAM].getValue() / 20.f);
		gain += (target - gain) * std::min(1.f, args.sampleTime / SMOOTH_SECONDS);

		int channels = inputs[IN_INPUT].getChannels();
		for (int c = 0; c < channels; c++)
			outputs[OUT_OUTPUT].setVoltage(gain * inputs[IN_INPUT].getVoltage(c), c);
		outputs[OUT_OUTPUT].setChannels(channels);
	}
};


/** Shows the gain for a moment after the knob moves: large black text in a white box, like
VibraLFo's ratio popup. It is drawn with nanovg, so it works on the MetaModule too (as a graphics
display). */
struct GainPopup : TransparentWidget {
	GoBetween* module;
	float lastDb = NAN;
	double shownAt = -1e9;

	GainPopup(Vec centerMm, GoBetween* module) {
		this->module = module;
		box.size = mm2px(Vec(18.f, 12.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	void step() override {
		TransparentWidget::step();
		if (!module)
			return;
		float db = module->params[GoBetween::GAIN_PARAM].getValue();
		// Show it only when the knob moves, not when the module is first loaded.
		if (!std::isnan(lastDb) && db != lastDb)
			shownAt = system::getTime();
		lastDb = db;
	}

	void draw(const DrawArgs& args) override {
		if (!module || std::isnan(lastDb) || system::getTime() - shownAt > GAIN_POPUP_SECONDS)
			return;
		std::shared_ptr<window::Font> font = loadPanelFont();
		if (!font || font->handle < 0)
			return;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, box.size.x, box.size.y, 3.f);
		nvgFillColor(args.vg, nvgRGB(0xff, 0xff, 0xff));
		nvgFill(args.vg);

		// The value, rounded to 0.1 dB, with "dB" under it.
		float db = std::round(lastDb * 10.f) / 10.f;
		if (db == 0.f)
			db = 0.f;   // no "-0.0"
		std::string value = string::f(db > 0.f ? "+%.1f" : "%.1f", db);
		nvgFontFaceId(args.vg, font->handle);
		nvgFillColor(args.vg, nvgRGB(0x00, 0x00, 0x00));
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgFontSize(args.vg, 19.f);
		nvgText(args.vg, box.size.x / 2, box.size.y * 0.38f, value.c_str(), NULL);
		nvgFontSize(args.vg, 10.f);
		nvgText(args.vg, box.size.x / 2, box.size.y * 0.8f, "dB", NULL);
	}
};


/** A zero with a diagonal slash through it, drawn as lines so it doesn't depend on the font. Sized
to match a LABEL_SIZE label; positioned by its centre, in mm. */
struct SlashedZero : PanelArt {
	SlashedZero(Vec centerMm) {
		box.size = mm2px(Vec(4.f, 4.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	void draw(const DrawArgs& args) override {
		// Cap height of the label font at LABEL_SIZE, and a digit's width.
		const float h = 0.72f * LABEL_SIZE, w = 0.5f * LABEL_SIZE;
		float cx = box.size.x / 2, cy = box.size.y / 2;
		nvgStrokeColor(args.vg, COLOR_TEXT);
		nvgStrokeWidth(args.vg, 1.1f);
		nvgLineCap(args.vg, NVG_ROUND);
		nvgBeginPath(args.vg);
		nvgEllipse(args.vg, cx, cy, w / 2, h / 2);
		nvgStroke(args.vg);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, cx - w * 0.3f, cy + h * 0.32f);
		nvgLineTo(args.vg, cx + w * 0.3f, cy - h * 0.32f);
		nvgStroke(args.vg);
	}
};


/** A plus or minus sign drawn as lines, so the two match exactly. Positioned by its centre, in mm. */
struct SignMark : PanelArt {
	bool plus;

	SignMark(Vec centerMm, bool plus) {
		this->plus = plus;
		box.size = mm2px(Vec(5.f, 5.f));
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	void draw(const DrawArgs& args) override {
		const float half = 3.25f;   // half the arm length, px
		float cx = box.size.x / 2, cy = box.size.y / 2;
		nvgStrokeColor(args.vg, COLOR_TEXT);
		nvgStrokeWidth(args.vg, 1.4f);
		nvgLineCap(args.vg, NVG_BUTT);
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, cx - half, cy);
		nvgLineTo(args.vg, cx + half, cy);
		if (plus) {
			nvgMoveTo(args.vg, cx, cy - half);
			nvgLineTo(args.vg, cx, cy + half);
		}
		nvgStroke(args.vg);
	}
};


struct GoBetweenWidget : ModuleWidget {
	void label(float xMm, float yMm, std::string text, float size = LABEL_SIZE) {
		addPanelArt(this, new PanelLabel(Vec(xMm, yMm), text, size));
	}

	GoBetweenWidget(GoBetween* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/GoBetween.svg")));
		// 4HP has room for one screw at the top and one at the bottom.
		addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewBlack>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		const float x = 10.16f;
		// The title runs up the panel, reading from bottom to top.
		addPanelArt(this, new PanelLabel(Vec(x, 32.f), "GOBETWEEN", TITLE_SIZE, -90.f));

		// Knob scale: - at the bottom left (-24 dB), 0 at the top (unity), + at the bottom right
		// (+24 dB). The knob turns 150 degrees each way from the top.
		const float knobY = 66.f;
		label(x, 55.5f, "GAIN");
		addParam(createParamCentered<MediumKnob>(mm2px(Vec(x, knobY)), module, GoBetween::GAIN_PARAM));
		addPanelArt(this, new SlashedZero(Vec(x, knobY - 6.8f)));
		addPanelArt(this, new SignMark(Vec(x - 3.6f, knobY + 6.f), false));
		addPanelArt(this, new SignMark(Vec(x + 3.6f, knobY + 6.f), true));

		label(x, 82.f, "IN");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, 90.f)), module, GoBetween::IN_INPUT));
		label(x, 104.f, "OUT");
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 112.f)), module, GoBetween::OUT_OUTPUT));

		// Added last so it draws over everything else: over the title.
		addChild(new GainPopup(Vec(x, 32.f), module));
#ifndef METAMODULE
		stripForPanelBaking(this);
#endif
	}
};


Model* modelGoBetween = createModel<GoBetween, GoBetweenWidget>("GoBetween");
