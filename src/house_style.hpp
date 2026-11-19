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


/** Static panel art drawn in code: labels and icons that never change. VCV Rack draws them as
widgets. On the MetaModule they are baked into the faceplate PNGs instead (see
metamodule/bake-panels.sh), because the MetaModule turns every widget that draws into a
"display" and lists it in the module's element list. Add these with addPanelArt(). */
struct PanelArt : TransparentWidget {};


/** Adds static panel art to the module widget in VCV Rack. On the MetaModule the art is already
in the faceplate PNG, so it is discarded. */
static inline void addPanelArt(ModuleWidget* w, PanelArt* art) {
#ifdef METAMODULE
	delete art;
#else
	w->addChild(art);
#endif
}


/** A text label drawn in code, since panel SVG <text> is not rendered by Rack. Positioned by
its centre, in mm. */
struct PanelLabel : PanelArt {
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
/** Silver metallic knobs for the MetaModule, where the stock black knobs are hard to see on the
black panel. The SDK maps res/knobs/NAME.svg to metamodule/assets/knobs/NAME.png. */
struct SilverKnob : app::SvgKnob {
	SilverKnob() {
		minAngle = -0.83f * M_PI;
		maxAngle = 0.83f * M_PI;
	}
};
struct SmallSilverKnob : SilverKnob {
	SmallSilverKnob() {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/silver-knob.svg")));
	}
};
struct MediumSilverKnob : SilverKnob {
	MediumSilverKnob() {
		setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/silver-knob-medium.svg")));
	}
};
/** Small knob (9 mm on MetaModule): silver on MetaModule, the stock small black knob in VCV Rack. */
using SmallKnob = SmallSilverKnob;
/** Medium knob (10 mm on MetaModule): silver on MetaModule, the stock black knob in VCV Rack. */
using MediumKnob = MediumSilverKnob;
#else
/** Small knob: silver on MetaModule, the stock small black knob in VCV Rack. */
using SmallKnob = RoundSmallBlackKnob;
/** Medium knob: silver on MetaModule, the stock black knob in VCV Rack. */
using MediumKnob = RoundBlackKnob;
#endif


/** Vertical fader with a blue-lit handle. Rack's stock track is a black slot that disappears on our
black panels, so this uses res/sliders/fader-track.svg (gray slot, light outline, tick marks).
Create it with createLightParamCentered, passing a light for the handle's brightness. On
MetaModule the track maps to metamodule/assets/sliders/fader-track.png. */
struct BlueFader : VCVLightSlider<BrightBlueLight> {
	BlueFader() {
		setBackgroundSvg(Svg::load(asset::plugin(pluginInstance, "res/sliders/fader-track.svg")));
	}
};


/** Adds the four black corner screws. */
#ifndef METAMODULE
/** For making the MetaModule faceplates only. When the COMINGINSECOND_BAKE_PANELS environment
variable is set, each module widget keeps just its panel and PanelArt, so Rack's screenshot
mode renders the faceplate with its labels and icons. Call at the end of the widget's
constructor. */
static inline void stripForPanelBaking(ModuleWidget* w) {
	if (!std::getenv("COMINGINSECOND_BAKE_PANELS"))
		return;
	std::vector<Widget*> remove;
	for (Widget* child : w->children) {
		if (child != w->getPanel() && !dynamic_cast<PanelArt*>(child))
			remove.push_back(child);
	}
	for (Widget* child : remove) {
		w->removeChild(child);
		delete child;
	}
}
#endif


static inline void addScrews(ModuleWidget* w) {
	w->addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, 0)));
	w->addChild(createWidget<ScrewBlack>(Vec(w->box.size.x - 2 * RACK_GRID_WIDTH, 0)));
	w->addChild(createWidget<ScrewBlack>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
	w->addChild(createWidget<ScrewBlack>(Vec(w->box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
}
