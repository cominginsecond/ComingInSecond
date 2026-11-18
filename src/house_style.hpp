#pragma once
// Coming in Second house style, shared by every module in the plugin: colours, fonts,
// labels, lights, knobs and screws. Include this from each module's .cpp file.
#include "plugin.hpp"
#include <cmath>
#include <cstdlib>


// Colours.
static const NVGcolor COLOR_TEXT = nvgRGB(0xff, 0xff, 0xff);       // panel labels (on black)
static const NVGcolor COLOR_BLUE = nvgRGB(0x1e, 0x90, 0xff);       // LEDs and lit controls
static const NVGcolor COLOR_BLUE_DARK = nvgRGB(0x10, 0x60, 0xd8);  // lit controls on dark parts

// Text sizes (px).
static constexpr float TITLE_SIZE = 15.f;
static constexpr float LABEL_SIZE = 10.f;


/** Bright blue LED. */
template <typename TBase = GrayModuleLightWidget>
struct TBrightBlueLight : TBase {
	TBrightBlueLight() {
		this->addBaseColor(COLOR_BLUE);
	}
};
using BrightBlueLight = TBrightBlueLight<>;


/** Franklin Gothic Medium ships with Windows. Fall back to Rack's own font if it can't be loaded
(macOS, Linux, MetaModule). */
static inline std::shared_ptr<window::Font> loadPanelFont() {
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


/** A text label drawn in code, since panel SVG <text> is not rendered by Rack. Positioned by
its centre, in mm. */
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


#ifdef METAMODULE
/** Silver metallic knob for the MetaModule, where the stock black knob is hard to see on the black
panel. The SDK maps res/knobs/silver-knob.svg to metamodule/assets/knobs/silver-knob.png. */
struct SilverKnob : app::SvgKnob {
	SilverKnob() {
		minAngle = -0.83f * M_PI;
		maxAngle = 0.83f * M_PI;
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/silver-knob.svg")));
	}
};
/** Small knob: silver on MetaModule, the stock small black knob in VCV Rack. */
using SmallKnob = SilverKnob;
#else
/** Small knob: silver on MetaModule, the stock small black knob in VCV Rack. */
using SmallKnob = RoundSmallBlackKnob;
#endif


/** Adds the four black corner screws. */
static inline void addScrews(ModuleWidget* w) {
	w->addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
	w->addChild(createWidget<ScrewBlack>(Vec(w->box.size.x - 2 * RACK_GRID_WIDTH, 0)));
	w->addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
	w->addChild(createWidget<ScrewBlack>(Vec(w->box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
}
