#include "house_style.hpp"


// BLOOTH: a waveshape animator. Each cycle of the input wave makes two pulses: the first comes
// from a comparator, the second from a window comparator that a DISTANCE control moves away from
// the first. The second pulse goes through a VCA that is open only while that pulse is high, so
// its height (AMP, plus AMP CV) can be positive or negative.
//
// The DSP models an analog circuit stage by stage, with its resistor values. Op amps clip at
// RAIL. Component names (IC1C, R13...) label the parts of that circuit each stage models.

static const float RAIL = 10.5f;             // TL074 output swing on +/-12 V
static const float DIODE_DROP = 0.6f;        // 1N4148 after each pulse comparator (D3, D4)
static const float PULSE_HIGH = RAIL - DIODE_DROP;
static const float KNOB_VOLTS = 5.f;         // WIDTH and DISTANCE pots sit across the 5 V regulator

// IC1C: input buffer. The input (R11 100K) and a +5 V offset (R12 100K) sum into R13 47K, so a
// +/-5 V wave becomes 0..-4.7 V.
static const float IN_GAIN = 47.f / 100.f;
// IC3C (width) and IC3B (distance): pot and CV each through 220K into 100K.
static const float CTRL_GAIN = 100.f / 220.f;
// IC4C output mixer. Pulse 1 arrives through R28 100K / R29 120K on the + input, with R30 100K to
// ground and R44 100K feedback on the - input. Pulse 2 arrives through R31 120K on the - input.
static const float P1_GAIN = (120.f / 220.f) * (1.f + 100.f / (100.f * 120.f / 220.f));
static const float P2_GAIN = 100.f / 120.f;
// R32 100K / R45 100K output divider.
static const float OUT_DIVIDER = 0.5f;
// Pulse 2 VCA. IC1A sums the AMP pot wiper and AMP CV (R33, R34 150K) into R36 220K. R37 220K /
// R39 1K divide that down to the LM13700 input, and T2 (R42 27K) sets its bias current from the
// inverted pulse, so the VCA is open only during pulse 2. R43 47K turns the output current into
// a voltage.
static const float AMP_SUM_GAIN = 220.f / 150.f;
static const float OTA_IN_DIVIDER = 1.f / 221.f;
static const float OTA_2VT = 2.f * 0.02585f;
static const float OTA_IABC = (PULSE_HIGH - 0.65f) / 27000.f;
static const float OTA_LOAD = 47000.f;
// AMP knob range. In the circuit the pot spans -PULSE_HIGH..+PULSE_HIGH, but the VCA reaches the
// rail at about 61% of that, leaving the rest of the knob dead. Here a full turn stops exactly at
// that point, so the whole sweep changes pulse 2. AMP CV keeps the circuit's 1:1 scaling, so a
// large CV can still drive the VCA into saturation.
static const float AMP_KNOB_VOLTS =
	std::atanh(RAIL / (OTA_IABC * OTA_LOAD)) * OTA_2VT / OTA_IN_DIVIDER / AMP_SUM_GAIN;


/** Fraction of the straight line from s0 to s1 that lies above threshold a. This is the
time-average of a comparator over one sample, which keeps the pulse edges from aliasing (a
first-order antiderivative anti-aliasing of the step). */
static inline float fractionAbove(float s0, float s1, float a) {
	bool above0 = s0 > a;
	bool above1 = s1 > a;
	if (above0 == above1)
		return above0 ? 1.f : 0.f;
	return above1 ? (s1 - a) / (s1 - s0) : (s0 - a) / (s0 - s1);
}

/** Fraction of the line from s0 to s1 that lies inside the window (lo, hi). Empty when lo >= hi. */
static inline float fractionInside(float s0, float s1, float lo, float hi) {
	if (lo >= hi)
		return 0.f;
	return fractionAbove(s0, s1, lo) - fractionAbove(s0, s1, hi);
}


struct Blooth : Module {
	enum ParamId {
		WIDTH_PARAM,
		WIDTH_ATT_PARAM,
		DISTANCE_PARAM,
		DISTANCE_ATT_PARAM,
		AMP_PARAM,
		AMP_ATT_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		WAVE_INPUT,
		WIDTH_CV_INPUT,
		DISTANCE_CV_INPUT,
		AMP_CV_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		PULSE1_LIGHT,
		PULSE2_LIGHT,
		LIGHTS_LEN
	};

	// The buffered input at the previous sample, per channel, for the anti-aliasing.
	float lastS[16] = {};

	Blooth() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(WIDTH_PARAM, 0.f, 1.f, 0.4f, "Width (both pulses)", "%", 0.f, 100.f);
		configParam(WIDTH_ATT_PARAM, 0.f, 1.f, 0.f, "Width CV amount", "%", 0.f, 100.f);
		configParam(DISTANCE_PARAM, 0.f, 1.f, 0.6f, "Distance (pulse 2 from pulse 1)", "%", 0.f, 100.f);
		configParam(DISTANCE_ATT_PARAM, 0.f, 1.f, 0.f, "Distance CV amount", "%", 0.f, 100.f);
		configParam(AMP_PARAM, -1.f, 1.f, 0.5f, "Pulse 2 amplitude", "%", 0.f, 100.f);
		configParam(AMP_ATT_PARAM, 0.f, 1.f, 0.f, "Amplitude CV amount", "%", 0.f, 100.f);
		configInput(WAVE_INPUT, "Wave (saw, triangle or sine, +/-5 V)");
		configInput(WIDTH_CV_INPUT, "Width CV (+5 V = full knob turn)");
		configInput(DISTANCE_CV_INPUT, "Distance CV (+5 V = full knob turn)");
		configInput(AMP_CV_INPUT, "Amplitude CV (about +6 V = full knob turn)");
		configOutput(OUT_OUTPUT, "Double pulse");
		configLight(PULSE1_LIGHT, "Pulse 1");
		configLight(PULSE2_LIGHT, "Pulse 2");
	}

	void onReset() override {
		for (int c = 0; c < 16; c++)
			lastS[c] = 0.f;
	}

	/** Pulse 2's level at the output mixer, from the VCA (IC1A, LM13700, IC4D). x is the AMP pot
	wiper plus AMP CV, in volts. */
	static float pulse2Level(float x) {
		float summed = clamp(AMP_SUM_GAIN * x, -RAIL, RAIL);
		float current = OTA_IABC * std::tanh(summed * OTA_IN_DIVIDER / OTA_2VT);
		float v = clamp(current * OTA_LOAD, -RAIL, RAIL);
		// In the circuit, the pot end fed by the non-inverted pulse makes pulse 2 negative. Here
		// clockwise (and positive CV) makes it positive.
		return P2_GAIN * v;
	}

	void process(const ProcessArgs& args) override {
		int channels = std::max(1, inputs[WAVE_INPUT].getChannels());
		channels = std::max(channels, inputs[WIDTH_CV_INPUT].getChannels());
		channels = std::max(channels, inputs[DISTANCE_CV_INPUT].getChannels());
		channels = std::max(channels, inputs[AMP_CV_INPUT].getChannels());

		float width = params[WIDTH_PARAM].getValue() * KNOB_VOLTS;
		float widthAtt = params[WIDTH_ATT_PARAM].getValue();
		float distance = params[DISTANCE_PARAM].getValue() * KNOB_VOLTS;
		float distanceAtt = params[DISTANCE_ATT_PARAM].getValue();
		float amp = params[AMP_PARAM].getValue() * AMP_KNOB_VOLTS;
		float ampAtt = params[AMP_ATT_PARAM].getValue();

		float p1Light = 0.f, p2Light = 0.f;
		for (int c = 0; c < channels; c++) {
			float in = inputs[WAVE_INPUT].getPolyVoltage(c);
			// IC1C. Inverted, so the bottom of the input wave is the top of s.
			float s = clamp(-IN_GAIN * (in + 5.f), -RAIL, RAIL);
			// IC3C: pulse 1 threshold. Pulse 1 is high while s is above it.
			float w1 = clamp(-CTRL_GAIN * (width + widthAtt * inputs[WIDTH_CV_INPUT].getPolyVoltage(c)), -RAIL, RAIL);
			// IC3B: pulse 2's top edge.
			float d = clamp(-CTRL_GAIN * (distance + distanceAtt * inputs[DISTANCE_CV_INPUT].getPolyVoltage(c)), -RAIL, RAIL);
			// IC3A (gain 2 on the R18/R19 average): pulse 2's bottom edge. Pulse 2 is high while s
			// is between w2 and d (IC4A and IC3D, ANDed by IC4B).
			float w2 = clamp(w1 + d, -RAIL, RAIL);

			float level2 = pulse2Level(amp + ampAtt * inputs[AMP_CV_INPUT].getPolyVoltage(c));
			// Output for each combination of pulses. IC4C clips, so where the pulses overlap the
			// second can only pull the output down from pulse 1's level.
			float v10 = clamp(P1_GAIN * PULSE_HIGH, -RAIL, RAIL);
			float v01 = clamp(level2, -RAIL, RAIL);
			float v11 = clamp(P1_GAIN * PULSE_HIGH + level2, -RAIL, RAIL);

			// Time-average each pulse over the sample, from the last s to this one.
			float s0 = lastS[c];
			float p1 = fractionAbove(s0, s, w1);
			float p2 = fractionInside(s0, s, w2, d);
			float both = fractionInside(s0, s, std::max(w1, w2), d);
			lastS[c] = s;

			float out = v10 * p1 + v01 * p2 + (v11 - v10 - v01) * both;
			outputs[OUT_OUTPUT].setVoltage(OUT_DIVIDER * out, c);

			if (c == 0) {
				p1Light = p1;
				p2Light = p2;
			}
		}
		outputs[OUT_OUTPUT].setChannels(channels);

		lights[PULSE1_LIGHT].setBrightnessSmooth(p1Light, args.sampleTime);
		lights[PULSE2_LIGHT].setBrightnessSmooth(p2Light, args.sampleTime);
	}
};


/** A white line drawing of a zoetrope: a slotted drum on a spindle, seen from a little above.
Through the open top, the inside of the far wall shows a strip of frames of the double pulse,
with pulse 2 moving along from frame to frame. Arrows on either side show the drum spinning.
Drawn in design units: x runs 0..100 across the box and y uses the same scale. */
struct ZoetropeIcon : PanelArt {
	ZoetropeIcon(Vec centerMm, Vec sizeMm) {
		box.size = mm2px(sizeMm);
		box.pos = mm2px(centerMm).minus(box.size.div(2));
	}

	// The drum, in design units.
	static constexpr float CX = 50.f;
	static constexpr float RX = 30.f, RY = 7.f;         // the rim ellipse
	static constexpr float TOP = 9.f, BOTTOM = 34.f;    // centers of the top and bottom ellipses

	/** A point on an ellipse around the drum's axis. phi is the angle around the drum: 0 is the
	front (nearest the viewer, lowest on the page), +pi/2 the right side, pi the back. */
	static Vec onEllipse(float cy, float rx, float ry, float phi) {
		return Vec(CX + rx * std::sin(phi), cy + ry * std::cos(phi));
	}

	/** Strokes part of an ellipse, from phi0 to phi1. */
	static void arc(NVGcontext* vg, float cy, float rx, float ry, float phi0, float phi1) {
		const int steps = 40;
		nvgBeginPath(vg);
		for (int i = 0; i <= steps; i++) {
			Vec p = onEllipse(cy, rx, ry, phi0 + (phi1 - phi0) * i / steps);
			if (i == 0)
				nvgMoveTo(vg, p.x, p.y);
			else
				nvgLineTo(vg, p.x, p.y);
		}
		nvgStroke(vg);
	}

	/** An arrowhead at the end of an arc, pointing along the arc's direction at phi. */
	static void arrowhead(NVGcontext* vg, float cy, float rx, float ry, float phi, float dir) {
		Vec tip = onEllipse(cy, rx, ry, phi);
		Vec back = onEllipse(cy, rx, ry, phi - dir * 0.18f);
		Vec d = tip.minus(back).normalize();
		Vec n = Vec(-d.y, d.x);
		const float len = 3.2f, half = 2.f;
		nvgBeginPath(vg);
		nvgMoveTo(vg, tip.x - d.x * len + n.x * half, tip.y - d.y * len + n.y * half);
		nvgLineTo(vg, tip.x, tip.y);
		nvgLineTo(vg, tip.x - d.x * len - n.x * half, tip.y - d.y * len - n.y * half);
		nvgStroke(vg);
	}

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		float s = box.size.x / 100.f;
		const float PI = M_PI;

		nvgSave(vg);
		nvgScale(vg, s, s);
		nvgStrokeColor(vg, COLOR_TEXT);
		nvgLineJoin(vg, NVG_ROUND);
		nvgLineCap(vg, NVG_ROUND);
		// Stroke widths are in screen pixels, so divide by the scale.
		auto width = [&](float px) { nvgStrokeWidth(vg, px / s); };

		// Spin arrows, one each side, outside the drum: the left one coming round to the front,
		// the right one going round to the back.
		const float AR = 40.f, ARY = 10.f, ACY = 22.f;
		width(1.f);
		arc(vg, ACY, AR, ARY, -2.2f, -1.0f);
		arrowhead(vg, ACY, AR, ARY, -1.0f, 1.f);
		arc(vg, ACY, AR, ARY, 1.0f, 2.2f);
		arrowhead(vg, ACY, AR, ARY, 2.2f, 1.f);

		// Spindle and base.
		width(1.2f);
		nvgBeginPath(vg);
		nvgMoveTo(vg, CX - 1.6f, BOTTOM + RY);
		nvgLineTo(vg, CX - 1.6f, 45.f);
		nvgMoveTo(vg, CX + 1.6f, BOTTOM + RY);
		nvgLineTo(vg, CX + 1.6f, 45.f);
		nvgStroke(vg);
		nvgBeginPath(vg);
		nvgEllipse(vg, CX, 46.5f, 13.f, 3.f);
		nvgFillColor(vg, nvgRGB(0, 0, 0));
		nvgFill(vg);
		nvgStroke(vg);
		arc(vg, 48.5f, 13.f, 3.f, -PI / 2, PI / 2);
		nvgBeginPath(vg);
		nvgMoveTo(vg, CX - 13.f, 46.5f);
		nvgLineTo(vg, CX - 13.f, 48.5f);
		nvgMoveTo(vg, CX + 13.f, 46.5f);
		nvgLineTo(vg, CX + 13.f, 48.5f);
		nvgStroke(vg);

		// Drum: the sides, the front of the bottom edge, and the whole top rim.
		width(1.3f);
		nvgBeginPath(vg);
		nvgMoveTo(vg, CX - RX, TOP);
		nvgLineTo(vg, CX - RX, BOTTOM);
		nvgMoveTo(vg, CX + RX, TOP);
		nvgLineTo(vg, CX + RX, BOTTOM);
		nvgStroke(vg);
		arc(vg, BOTTOM, RX, RY, -PI / 2, PI / 2);
		nvgBeginPath(vg);
		nvgEllipse(vg, CX, TOP, RX, RY);
		nvgStroke(vg);

		// Frames on the inside of the far wall, seen through the open top. Each is a small
		// double pulse, with pulse 2 a little further along than in the frame before.
		width(0.8f);
		nvgLineJoin(vg, NVG_MITER);
		const int frames = 5;
		for (int i = 0; i < frames; i++) {
			float phi = PI + (i - (frames - 1) / 2.f) * 0.42f;   // around the back
			float c = std::fabs(std::cos(phi));                   // foreshortening
			Vec at = onEllipse(TOP, RX * 0.9f, RY, phi);
			float fw = 9.5f * c;
			float x0 = at.x - fw / 2;
			float base = at.y + 7.5f;
			float h1 = 5.5f, h2 = 3.f;
			float p2 = 0.42f + 0.09f * i;
			auto X = [&](float f) { return x0 + f * fw; };
			nvgBeginPath(vg);
			nvgMoveTo(vg, X(0.f), base);
			nvgLineTo(vg, X(0.08f), base);
			nvgLineTo(vg, X(0.08f), base - h1);
			nvgLineTo(vg, X(0.3f), base - h1);
			nvgLineTo(vg, X(0.3f), base);
			nvgLineTo(vg, X(p2), base);
			nvgLineTo(vg, X(p2), base - h2);
			nvgLineTo(vg, X(p2 + 0.18f), base - h2);
			nvgLineTo(vg, X(p2 + 0.18f), base);
			nvgLineTo(vg, X(1.f), base);
			nvgStroke(vg);
		}
		nvgLineJoin(vg, NVG_ROUND);

		// Viewing slots around the front of the wall, narrowing towards the sides.
		width(0.9f);
		const int slots = 9;
		for (int i = 0; i < slots; i++) {
			float phi = (i - (slots - 1) / 2.f) * (PI / (slots + 1));
			float c = std::cos(phi);
			Vec at = onEllipse(TOP, RX, RY, phi);
			float sw = 1.8f * c;
			nvgBeginPath(vg);
			nvgRoundedRect(vg, at.x - sw / 2, at.y + 2.f, sw, 10.f, std::min(0.6f, sw / 2));
			nvgFillColor(vg, COLOR_TEXT);
			nvgFill(vg);
		}

		// A band around the lower wall.
		width(0.8f);
		arc(vg, TOP + 15.f, RX, RY, -PI / 2, PI / 2);

		nvgRestore(vg);
	}
};


struct BloothWidget : ModuleWidget {
	void label(float xMm, float yMm, std::string text, float size = LABEL_SIZE) {
		addPanelArt(this, new PanelLabel(Vec(xMm, yMm), text, size));
	}

	BloothWidget(Blooth* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Blooth.svg")));
		addScrews(this);

		label(25.4f, 6.f, "BLOOTH", TITLE_SIZE);

		// Three columns, one per control: knob, CV amount, CV jack.
		const float colX[3] = {9.6f, 25.4f, 41.2f};
		const int knobs[3] = {Blooth::WIDTH_PARAM, Blooth::DISTANCE_PARAM, Blooth::AMP_PARAM};
		const int atts[3] = {Blooth::WIDTH_ATT_PARAM, Blooth::DISTANCE_ATT_PARAM, Blooth::AMP_ATT_PARAM};
		const int cvs[3] = {Blooth::WIDTH_CV_INPUT, Blooth::DISTANCE_CV_INPUT, Blooth::AMP_CV_INPUT};
		const char* names[3] = {"WIDTH", "DIST", "AMP"};
		const float knobY = 23.f, attY = 40.f, cvY = 53.f;
		for (int i = 0; i < 3; i++) {
			label(colX[i], knobY - 8.f, names[i]);
			addParam(createParamCentered<MediumKnob>(mm2px(Vec(colX[i], knobY)), module, knobs[i]));
			addParam(createParamCentered<SmallKnob>(mm2px(Vec(colX[i], attY)), module, atts[i]));
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[i], cvY)), module, cvs[i]));
		}

		// Zoetrope drawing, with a light for each pulse under it.
		addPanelArt(this, new ZoetropeIcon(Vec(25.4f, 78.f), Vec(45.6f, 24.f)));
		addChild(createLightCentered<SmallLight<BrightBlueLight>>(mm2px(Vec(14.f, 94.5f)), module, Blooth::PULSE1_LIGHT));
		addChild(createLightCentered<SmallLight<BrightBlueLight>>(mm2px(Vec(34.f, 94.5f)), module, Blooth::PULSE2_LIGHT));
		label(17.f, 94.5f, "1", 8.f);
		label(37.f, 94.5f, "2", 8.f);

		// Jacks: wave in, double pulse out.
		const float ioY = 112.f;
		label(12.f, ioY - 8.f, "IN");
		label(38.8f, ioY - 8.f, "OUT");
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.f, ioY)), module, Blooth::WAVE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(38.8f, ioY)), module, Blooth::OUT_OUTPUT));
#ifndef METAMODULE
		stripForPanelBaking(this);
#endif
	}
};


Model* modelBlooth = createModel<Blooth, BloothWidget>("Blooth");
