#include "plugin.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

// =====================================================================
// NOTE: the original SynthVoice engine (ported bit-for-bit from Navy
// Arp 2's real JUCE SynthVoice struct) has been removed from this
// module entirely. Spaces Command is now a pure eurorack sequencer --
// no audio, no instruments. The Analog/FM/Supersaw/Pulse oscillators,
// ADSR, one-pole filter, and equal-power layer mixing that used to live
// here are slated to move into a future separate "connector" module
// (MIDI + sound), not yet built. Command's job is just pattern/scene
// logic and CV/Gate output.
// =====================================================================

struct SceneState {
	float faders[8] = { 1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f };
	float rest = 0.1f, legato = 0.5f, rate = 0.5f;
	float entropy = 0.f, harmony = 0.f, chaos = 0.f, octaves = 0.f;
};

struct SpacesCommand : Module {
	enum ParamId {
		ENUMS(FADER_PARAM, 8),
		MELO_PARAM, MELO_NUDGE_DOWN, MELO_NUDGE_UP,
		SCENE_A_PARAM, SCENE_B_PARAM, MORPH_PARAM,
		LATCH_PARAM, ARPSEQ_PARAM, POLY_PARAM, FREEZE_PARAM, ROUTING_PARAM,
		REST_PARAM, DICE_ARTI, ARTI_NUDGE_DOWN, ARTI_NUDGE_UP, LEGATO_PARAM, RATE_PARAM, DICE_TIME, TIME_NUDGE_DOWN, TIME_NUDGE_UP,
		ENTROPY_PARAM, HARMONY_PARAM, CHAOS_PARAM, DICE_NAVY, NAVY_NUDGE_DOWN, NAVY_NUDGE_UP, OCTAVES_PARAM,
		ROOT_KEY_PARAM, SCALE_TYPE_PARAM, DENSITY_PARAM, SWING_PARAM,
		// Per-voice GATE LEN: used to shape a synth envelope's release when
		// this module had an audio engine; now shapes the CV gate pulse
		// width sent out VOICE1/2_GATE_OUTPUT directly (5-100% of the step
		// interval). Kept as two separate knobs even though both voices
		// currently receive the same pitch/gate timing, so each output can
		// still be given a different gate length.
		VOICE1_GATE_LEN_PARAM, VOICE2_GATE_LEN_PARAM,
		PARAMS_LEN
	};
	enum InputId { VOCT_INPUT, GATE_INPUT, VELOCITY_INPUT, CLOCK_INPUT, INPUTS_LEN };
	enum OutputId { VOICE1_PITCH_OUTPUT, VOICE1_GATE_OUTPUT, VOICE2_PITCH_OUTPUT, VOICE2_GATE_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		ENUMS(STEP_LIGHTS, 8), SCENE_A_LIGHT, SCENE_B_LIGHT,
		LATCH_LIGHT, ARPSEQ_LIGHT, POLY_LIGHT, FREEZE_LIGHT, ROUTING_LIGHT,
		LIGHTS_LEN
	};

	SceneState sceneA, sceneB;
	bool focusB = false;
	// Live Scene A/B blend, computed every process() tick for every
	// scene-scoped control (8 faders + the 7 FEEL knobs below), matching
	// the original VST's timerCallback exactly: any control not currently
	// grabbed by the mouse continuously shows interpolate(sceneA, sceneB,
	// morph). The underlying param is untouched by this -- it's the true
	// edit target -- these arrays are display-only.
	float displayFaderValue[8] = {1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f};
	float displayRest = 0.1f, displayLegato = 0.5f, displayRate = 0.5f;
	float displayEntropy = 0.f, displayHarmony = 0.f, displayChaos = 0.f, displayOctaves = 0.f;
	int currentStep = 0;
	bool goingForward = true;
	double phaseAccumSamples = 0.0;
	double samplesSinceLastStep = 0.0;
	double lastStepIntervalSamples = 4410.0;  // ~100ms default until the first real interval is measured
	// Clock-subdivision tracking: matches the original's stepLengthPPQ model
	// (1/4, 1/8, 1/16, 1/32 note lengths, derived from a quarter-note BEAT
	// duration) rather than dividing incoming pulses. An incoming CLOCK
	// pulse is treated as one quarter-note beat (the standard VCV clock
	// convention) -- RATE then selects how many steps subdivide that beat
	// (1/2/4/8 for 1/4-1/8-1/16-1/32), generated from the measured
	// pulse-to-pulse interval rather than from host PPQ, which Rack has
	// no equivalent of.
	double samplesSinceLastClockPulse = 0.0;
	double lastClockPulseIntervalSamples = 0.0;
	int clockSubStepIndex = 0;
	// Per-voice gate-output countdown: samples remaining with the gate
	// held high, counted down from VOICE1/2_GATE_LEN_PARAM * the last
	// measured step interval. Was previously used to schedule a SynthVoice
	// envelope release; now it directly drives the VOICE1/2_GATE_OUTPUT
	// jacks high/low.
	int voice1GateCountdown = -1;  // -1 = gate currently low
	int voice2GateCountdown = -1;

	std::vector<int> heldNotes, latchedNotes;
	float lastPitchVolt = 0.f;
	dsp::SchmittTrigger sceneATrig, sceneBTrig, clockTrig;
	dsp::SchmittTrigger diceArtiTrig, diceTimeTrig, diceNavyTrig, meloTrig;
	dsp::SchmittTrigger meloNudgeDownTrig, meloNudgeUpTrig, artiNudgeDownTrig, artiNudgeUpTrig;
	dsp::SchmittTrigger timeNudgeDownTrig, timeNudgeUpTrig, navyNudgeDownTrig, navyNudgeUpTrig;

	// LATCH/ARP-SEQ/POLY/FREEZE/ROUTING are also momentary buttons that
	// must TOGGLE persisted state -- same bug/fix as the wave buttons.
	// Previously read directly from the momentary param, so they only
	// registered "on" while physically held, never actually latched.
	bool latchOnState = false, arpSeqOnState = false, polyOnState = false;
	bool freezeOnState = false;
	// ROUTING is a stub: it used to select how Voice1/Voice2's two audio
	// streams got mixed (Layered/Split/External-only). With no audio
	// engine on this module anymore, cycling it and lighting ROUTING_LIGHT
	// still works and routingState is still saved, but nothing in this
	// module's own process() reads it -- it's kept for a future connector/
	// voice module to read and interpret.
	int routingState = 0;  // 0=Layered(Voice1), 1=Split A.B, 2=External Out Only
	dsp::SchmittTrigger latchTrig, arpSeqTrig, polyTrig, freezeTrig, routingTrig;

	SpacesCommand() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < 8; i++)
			configParam(FADER_PARAM + i, 0.f, 1.f, 1.f, string::f("Step %d probability", i + 1), "%", 0, 100);
		configButton(MELO_PARAM, "Randomize pattern (MELO)");
		configButton(MELO_NUDGE_DOWN, "Nudge pattern down");
		configButton(MELO_NUDGE_UP, "Nudge pattern up");
		configButton(SCENE_A_PARAM, "Focus Scene A");
		configButton(SCENE_B_PARAM, "Focus Scene B");
		configParam(MORPH_PARAM, 0.f, 1.f, 0.f, "Scene morph", "%", 0, 100);
		configSwitch(LATCH_PARAM, 0.f, 1.f, 0.f, "Latch", {"Off", "On"});
		configSwitch(ARPSEQ_PARAM, 0.f, 1.f, 0.f, "Arp / Seq mode", {"Arp", "Seq"});
		configSwitch(POLY_PARAM, 0.f, 1.f, 0.f, "Poly", {"Off", "On"});
		configSwitch(FREEZE_PARAM, 0.f, 1.f, 0.f, "Freeze", {"Off", "On"});
		configSwitch(ROUTING_PARAM, 0.f, 2.f, 0.f, "Voice routing", {"Layered (Voice 1)", "Split A\u00b7B", "External Out Only"});
		configParam(REST_PARAM, 0.f, 1.f, 0.1f, "Rest probability", "%", 0, 100);
		configButton(DICE_ARTI, "Randomize Rest+Legato (ARTI)");
		configButton(ARTI_NUDGE_DOWN, "Nudge Rest+Legato down");
		configButton(ARTI_NUDGE_UP, "Nudge Rest+Legato up");
		configParam(LEGATO_PARAM, 0.f, 1.f, 0.5f, "Legato", "%", 0, 100);
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate (free-run BPM, or 1/4-1/32 note subdivision of the incoming CLOCK's beat)", " BPM", 0, 200, 40);
		configButton(DICE_TIME, "Randomize Rate+Octaves (TIME)");
		configButton(TIME_NUDGE_DOWN, "Nudge Rate+Octaves down");
		configButton(TIME_NUDGE_UP, "Nudge Rate+Octaves up");
		configParam(ENTROPY_PARAM, -1.f, 1.f, 0.f, "Entropy (play direction)");
		configParam(HARMONY_PARAM, 0.f, 1.f, 0.f, "Harmony");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.f, "Chaos");
		configButton(DICE_NAVY, "Randomize Entropy+Harmony+Chaos (NAVY)");
		configButton(NAVY_NUDGE_DOWN, "Nudge Entropy+Harmony+Chaos down");
		configButton(NAVY_NUDGE_UP, "Nudge Entropy+Harmony+Chaos up");
		configParam(OCTAVES_PARAM, -3.f, 3.f, 0.f, "Octave shift")->snapEnabled = true;
		configParam(ROOT_KEY_PARAM, 0.f, 11.f, 0.f, "Root key");
		getParamQuantity(ROOT_KEY_PARAM)->snapEnabled = true;
		configParam(SCALE_TYPE_PARAM, 0.f, 9.f, 0.f, "Scale");
		getParamQuantity(SCALE_TYPE_PARAM)->snapEnabled = true;
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.5f, "Density", "%", 0, 100);
		configParam(SWING_PARAM, 0.f, 1.f, 0.f, "Swing", "%", 0, 100);

		configParam(VOICE1_GATE_LEN_PARAM, 0.05f, 1.f, 0.95f, "Voice 1 gate length", "%", 0, 100);
		configParam(VOICE2_GATE_LEN_PARAM, 0.05f, 1.f, 0.5f, "Voice 2 gate length", "%", 0, 100);

		configInput(VOCT_INPUT, "1V/oct pitch (poly, held notes)");
		configInput(GATE_INPUT, "Gate (poly, held notes)");
		configInput(VELOCITY_INPUT, "Velocity (poly)");
		configInput(CLOCK_INPUT, "Clock (patched = external run/stop; unpatched = free-run on RATE)");
		configOutput(VOICE1_PITCH_OUTPUT, "Voice 1 pitch (1V/oct)");
		configOutput(VOICE1_GATE_OUTPUT, "Voice 1 gate");
		configOutput(VOICE2_PITCH_OUTPUT, "Voice 2 pitch (1V/oct)");
		configOutput(VOICE2_GATE_OUTPUT, "Voice 2 gate");
	}

	void captureFocusedScene() {
		SceneState& s = focusB ? sceneB : sceneA;
		for (int i = 0; i < 8; i++) s.faders[i] = params[FADER_PARAM + i].getValue();
		s.rest = params[REST_PARAM].getValue();
		s.legato = params[LEGATO_PARAM].getValue();
		s.rate = params[RATE_PARAM].getValue();
		s.entropy = params[ENTROPY_PARAM].getValue();
		s.harmony = params[HARMONY_PARAM].getValue();
		s.chaos = params[CHAOS_PARAM].getValue();
		s.octaves = params[OCTAVES_PARAM].getValue();
	}

	// The missing counterpart to captureFocusedScene(): pushes a scene's
	// STORED data into the live params. Without this, switching focus left
	// the panel showing the PREVIOUS scene's leftover values, and the next
	// captureFocusedScene() call would immediately overwrite the newly
	// focused scene with those leftovers -- silently collapsing both scenes
	// to identical data after a couple of focus switches, which is why
	// crossfading eventually produced no audible/visual difference at all.
	void loadSceneIntoParams(const SceneState& s) {
		for (int i = 0; i < 8; i++) params[FADER_PARAM + i].setValue(s.faders[i]);
		params[REST_PARAM].setValue(s.rest);
		params[LEGATO_PARAM].setValue(s.legato);
		params[RATE_PARAM].setValue(s.rate);
		params[ENTROPY_PARAM].setValue(s.entropy);
		params[HARMONY_PARAM].setValue(s.harmony);
		params[CHAOS_PARAM].setValue(s.chaos);
		params[OCTAVES_PARAM].setValue(s.octaves);
	}

	void randomizeMelo() {
		SceneState& s = focusB ? sceneB : sceneA;
		for (int i = 0; i < 8; i++) {
			s.faders[i] = random::uniform();
			params[FADER_PARAM + i].setValue(s.faders[i]);
		}
	}
	void randomizeArti() {
		SceneState& s = focusB ? sceneB : sceneA;
		s.rest = random::uniform(); s.legato = random::uniform();
		params[REST_PARAM].setValue(s.rest);
		params[LEGATO_PARAM].setValue(s.legato);
	}
	void randomizeTime() {
		SceneState& s = focusB ? sceneB : sceneA;
		s.rate = random::uniform(); s.octaves = random::uniform() * 6.f - 3.f;
		params[RATE_PARAM].setValue(s.rate);
		params[OCTAVES_PARAM].setValue(s.octaves);
	}
	void randomizeNavy() {
		SceneState& s = focusB ? sceneB : sceneA;
		s.entropy = random::uniform() * 2.f - 1.f;
		s.harmony = random::uniform(); s.chaos = random::uniform();
		params[ENTROPY_PARAM].setValue(s.entropy);
		params[HARMONY_PARAM].setValue(s.harmony);
		params[CHAOS_PARAM].setValue(s.chaos);
	}

	// Nudge functions: step the SAME parameter cluster its paired dice
	// button randomizes, by one small fixed increment, clamped to range.
	static float nudgeClamp(float v, float delta, float lo, float hi) {
		return clamp(v + delta, lo, hi);
	}

	void nudgeMelo(float dir) {
		float step = 0.1f * dir;
		for (int i = 0; i < 8; i++) {
			float v = nudgeClamp(params[FADER_PARAM + i].getValue(), step, 0.f, 1.f);
			params[FADER_PARAM + i].setValue(v);
		}
	}
	void nudgeArti(float dir) {
		float step = 0.1f * dir;
		params[REST_PARAM].setValue(nudgeClamp(params[REST_PARAM].getValue(), step, 0.f, 1.f));
		params[LEGATO_PARAM].setValue(nudgeClamp(params[LEGATO_PARAM].getValue(), step, 0.f, 1.f));
	}
	void nudgeTime(float dir) {
		params[RATE_PARAM].setValue(nudgeClamp(params[RATE_PARAM].getValue(), 0.1f * dir, 0.f, 1.f));
		params[OCTAVES_PARAM].setValue(nudgeClamp(params[OCTAVES_PARAM].getValue(), 1.f * dir, -3.f, 3.f));
	}
	void nudgeNavy(float dir) {
		params[ENTROPY_PARAM].setValue(nudgeClamp(params[ENTROPY_PARAM].getValue(), 0.15f * dir, -1.f, 1.f));
		params[HARMONY_PARAM].setValue(nudgeClamp(params[HARMONY_PARAM].getValue(), 0.1f * dir, 0.f, 1.f));
		params[CHAOS_PARAM].setValue(nudgeClamp(params[CHAOS_PARAM].getValue(), 0.1f * dir, 0.f, 1.f));
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "latchOn", json_boolean(latchOnState));
		json_object_set_new(rootJ, "arpSeqOn", json_boolean(arpSeqOnState));
		json_object_set_new(rootJ, "polyOn", json_boolean(polyOnState));
		json_object_set_new(rootJ, "freezeOn", json_boolean(freezeOnState));
		json_object_set_new(rootJ, "routingState", json_integer(routingState));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* latchJ = json_object_get(rootJ, "latchOn");
		if (latchJ) latchOnState = json_boolean_value(latchJ);
		json_t* arpSeqJ = json_object_get(rootJ, "arpSeqOn");
		if (arpSeqJ) arpSeqOnState = json_boolean_value(arpSeqJ);
		json_t* polyJ = json_object_get(rootJ, "polyOn");
		if (polyJ) polyOnState = json_boolean_value(polyJ);
		json_t* freezeJ = json_object_get(rootJ, "freezeOn");
		if (freezeJ) freezeOnState = json_boolean_value(freezeJ);
		json_t* routingJ = json_object_get(rootJ, "routingState");
		if (routingJ) routingState = clamp((int)json_integer_value(routingJ), 0, 2);
	}

	void process(const ProcessArgs& args) override {
		if (sceneATrig.process(params[SCENE_A_PARAM].getValue())) { focusB = false; loadSceneIntoParams(sceneA); }
		if (sceneBTrig.process(params[SCENE_B_PARAM].getValue())) { focusB = true; loadSceneIntoParams(sceneB); }
		lights[SCENE_A_LIGHT].setBrightness(!focusB);
		lights[SCENE_B_LIGHT].setBrightness(focusB);

		// Mode toggle LEDs: navy when on, unlit when off
		if (latchTrig.process(params[LATCH_PARAM].getValue())) latchOnState = !latchOnState;
		if (arpSeqTrig.process(params[ARPSEQ_PARAM].getValue())) arpSeqOnState = !arpSeqOnState;
		if (polyTrig.process(params[POLY_PARAM].getValue())) polyOnState = !polyOnState;
		if (freezeTrig.process(params[FREEZE_PARAM].getValue())) freezeOnState = !freezeOnState;
		if (routingTrig.process(params[ROUTING_PARAM].getValue())) routingState = (routingState + 1) % 3;
		lights[LATCH_LIGHT].setBrightness(latchOnState ? 1.f : 0.f);
		lights[ARPSEQ_LIGHT].setBrightness(arpSeqOnState ? 1.f : 0.f);
		lights[POLY_LIGHT].setBrightness(polyOnState ? 1.f : 0.f);
		lights[FREEZE_LIGHT].setBrightness(freezeOnState ? 1.f : 0.f);
		lights[ROUTING_LIGHT].setBrightness(routingState / 2.f);

		if (meloTrig.process(params[MELO_PARAM].getValue())) randomizeMelo();
		if (diceArtiTrig.process(params[DICE_ARTI].getValue())) randomizeArti();
		if (diceTimeTrig.process(params[DICE_TIME].getValue())) randomizeTime();
		if (diceNavyTrig.process(params[DICE_NAVY].getValue())) randomizeNavy();
		if (meloNudgeDownTrig.process(params[MELO_NUDGE_DOWN].getValue())) nudgeMelo(-1.f);
		if (meloNudgeUpTrig.process(params[MELO_NUDGE_UP].getValue())) nudgeMelo(1.f);
		if (artiNudgeDownTrig.process(params[ARTI_NUDGE_DOWN].getValue())) nudgeArti(-1.f);
		if (artiNudgeUpTrig.process(params[ARTI_NUDGE_UP].getValue())) nudgeArti(1.f);
		if (timeNudgeDownTrig.process(params[TIME_NUDGE_DOWN].getValue())) nudgeTime(-1.f);
		if (timeNudgeUpTrig.process(params[TIME_NUDGE_UP].getValue())) nudgeTime(1.f);
		if (navyNudgeDownTrig.process(params[NAVY_NUDGE_DOWN].getValue())) nudgeNavy(-1.f);
		if (navyNudgeUpTrig.process(params[NAVY_NUDGE_UP].getValue())) nudgeNavy(1.f);

		captureFocusedScene();

		float morph = params[MORPH_PARAM].getValue();
		float restRaw = crossfade(sceneA.rest, sceneB.rest, morph);
		float legato = crossfade(sceneA.legato, sceneB.legato, morph);
		float rest = restRaw;
		// Matches the original VST exactly: high legato suppresses rest
		// probability for DSP purposes only -- the REST knob's on-screen
		// value still shows the raw blend, unaffected by this.
		if (legato >= 0.8f) rest = rest * clamp((1.f - legato) / 0.2f, 0.f, 1.f);
		float rate01 = crossfade(sceneA.rate, sceneB.rate, morph);
		float entropy = crossfade(sceneA.entropy, sceneB.entropy, morph);
		float harmonyF = crossfade(sceneA.harmony, sceneB.harmony, morph);
		float chaosF = crossfade(sceneA.chaos, sceneB.chaos, morph);
		float octavesF = crossfade(sceneA.octaves, sceneB.octaves, morph);
		int octaveShift = (int)std::round(octavesF);

		// Live-morph display values -- see displayFaderValue etc. declaration above.
		for (int i = 0; i < 8; i++)
			displayFaderValue[i] = crossfade(sceneA.faders[i], sceneB.faders[i], morph);
		displayRest = restRaw;
		displayLegato = legato;
		displayRate = rate01;
		displayEntropy = entropy;
		displayHarmony = harmonyF;
		displayChaos = chaosF;
		displayOctaves = octavesF;

		heldNotes.clear();
		int channels = std::max(inputs[VOCT_INPUT].getChannels(), 1);
		for (int c = 0; c < channels; c++) {
			if (inputs[GATE_INPUT].getVoltage(c) >= 1.f) {
				int pitch = 60 + (int)std::round(inputs[VOCT_INPUT].getVoltage(c) * 12.f);
				heldNotes.push_back(pitch);
			}
		}
		bool latchOn = latchOnState;
		bool freezeOn = freezeOnState;
		if (!heldNotes.empty() && latchOn) latchedNotes = heldNotes;
		std::vector<int>& notesToPlay = latchOn ? latchedNotes : heldNotes;

		// Clock-presence run logic: patched CLOCK drives steps (patching/
		// unpatching IS start/stop); unpatched free-runs off RATE, gated
		// by held notes / FREEZE, same as before.
		bool clockPatched = inputs[CLOCK_INPUT].isConnected();
		bool stepTriggered = false;
		float swingParam = params[SWING_PARAM].getValue();

		if (clockPatched) {
			bool rawClockEdge = clockTrig.process(inputs[CLOCK_INPUT].getVoltage());
			samplesSinceLastClockPulse += 1.0;

			// RATE selects how many steps subdivide each incoming beat,
			// matching the original's stepLengthPPQ ratio (1.0/0.5/0.25/
			// 0.125 for 1/4, 1/8, 1/16, 1/32) -- a FINER subdivision means
			// MORE steps per incoming pulse, not fewer. Index 2 (1/16) is
			// the baseline, needing 4 steps per quarter-note pulse.
			int rateIdx = clamp((int)std::round(rate01 * 3.f), 0, 3);
			int subdivisionCount = (rateIdx == 0) ? 1 : (rateIdx == 1) ? 2 : (rateIdx == 2) ? 4 : 8;

			if (rawClockEdge) {
				if (samplesSinceLastClockPulse > 1.0) lastClockPulseIntervalSamples = samplesSinceLastClockPulse;
				samplesSinceLastClockPulse = 0.0;
				clockSubStepIndex = 0;
				stepTriggered = true;  // the incoming edge itself is always a step boundary
			} else if (lastClockPulseIntervalSamples > 1.0 && clockSubStepIndex + 1 < subdivisionCount) {
				// Generate the remaining subdivisions within the measured
				// beat period. Swing (same +-45%-of-interval alternation as
				// free-run, below) staggers alternating sub-steps -- there's
				// no host PPQ here to run the original's absolute-position
				// swing formula against, so this is the closest faithful
				// adaptation to a raw pulse-derived beat.
				double subStepInterval = lastClockPulseIntervalSamples / subdivisionCount;
				double swingOffset = 0.45 * swingParam * subStepInterval;
				double target = subStepInterval * (clockSubStepIndex + 1);
				target += (clockSubStepIndex % 2 == 0) ? swingOffset : -swingOffset;
				if (samplesSinceLastClockPulse >= target) {
					clockSubStepIndex++;
					stepTriggered = true;
				}
			}
		} else {
			bool playing = !notesToPlay.empty() || freezeOn;
			double bpm = 40.0 + rate01 * 200.0;  // matches original: 40-240 BPM free-run
			double stepSamples = args.sampleRate * (60.0 / std::max(1.0, bpm)) * 0.25;
			// Swing: matches original exactly -- even-indexed steps get
			// lengthened, odd-indexed steps get shortened, up to 45% of
			// the step interval.
			double swingAmtSamples = 0.45 * swingParam * stepSamples;
			double activeStepSamples = (currentStep % 2 == 0) ? stepSamples + swingAmtSamples : stepSamples - swingAmtSamples;
			if (playing) {
				phaseAccumSamples += 1.0;
				if (phaseAccumSamples >= activeStepSamples) { phaseAccumSamples = 0.0; stepTriggered = true; }
			}
		}

		// Track the real interval between steps (works for both free-run
		// and external clock) so note-off timing can match the original's
		// stepSamples*0.95 gate regardless of what's driving the clock.
		samplesSinceLastStep += 1.0;
		if (stepTriggered) {
			if (samplesSinceLastStep > 1.0) lastStepIntervalSamples = samplesSinceLastStep;
			samplesSinceLastStep = 0.0;
		}

		// Count down each voice's gate-high duration; when it reaches zero
		// the gate output drops low (see the end of process() below).
		if (voice1GateCountdown > 0) { voice1GateCountdown--; }
		if (voice2GateCountdown > 0) { voice2GateCountdown--; }

		if (stepTriggered) {
			int playDirection = 0;
			if (entropy >= -0.1f && entropy <= 0.1f) playDirection = 0;
			else if (entropy > 0.1f && entropy <= 0.5f) playDirection = 1;
			else if (entropy > 0.5f) playDirection = 2;
			else if (entropy < -0.1f && entropy >= -0.5f) playDirection = 3;
			else playDirection = 4;

			int localStep = currentStep;
			if (playDirection == 1) {
				if (goingForward) { localStep++; if (localStep >= 7) { localStep = 7; goingForward = false; } }
				else { localStep--; if (localStep <= 0) { localStep = 0; goingForward = true; } }
			} else if (playDirection == 2) {
				float r = random::uniform();
				if (r < 0.7f) localStep = (localStep + 1) % 8;
				else if (r < 0.9f) localStep = (localStep - 1 + 8) % 8;
			} else if (playDirection == 3) {
				localStep = (localStep - 1 + 8) % 8;
			} else if (playDirection == 4) {
				localStep = (random::uniform() < 0.2f) ? (localStep + 2) % 8 : (localStep + 1) % 8;
			} else {
				localStep = (localStep + 1) % 8;
			}
			currentStep = localStep;

			float morphedFader = crossfade(sceneA.faders[localStep], sceneB.faders[localStep], morph);
			float density = params[DENSITY_PARAM].getValue();
			float faderProb = morphedFader;
			if (density < 0.5f) faderProb = morphedFader * (density / 0.5f);
			else if (density > 0.5f) faderProb = morphedFader + (1.f - morphedFader) * ((density - 0.5f) / 0.5f);

			if (random::uniform() <= faderProb && !(random::uniform() <= rest)) {
				int rootKeyIdx = (int)std::round(params[ROOT_KEY_PARAM].getValue());
				int scaleIdx = (int)std::round(params[SCALE_TYPE_PARAM].getValue());
				static const std::vector<std::vector<int>> scales = {
					{0,2,4,5,7,9,11,12}, {0,2,3,5,7,8,10,12}, {0,3,5,7,10,12,15,17}, {0,2,4,7,9,12,14,16},
					{0,2,3,5,7,9,10,12}, {0,1,3,5,7,8,10,12}, {0,2,4,6,7,9,11,12}, {0,2,4,5,7,9,10,12},
					{0,2,3,5,7,8,11,12}, {0,2,3,5,7,9,11,12}
				};
				const std::vector<int>& scaleOffsets = scales[clamp(scaleIdx, 0, 9)];
				int pitch;
				if (arpSeqOnState && !notesToPlay.empty())
					pitch = notesToPlay[localStep % notesToPlay.size()] + 12 * octaveShift;
				else
					pitch = 48 + rootKeyIdx + scaleOffsets[localStep % (int)scaleOffsets.size()] + 12 * octaveShift;
				// CHAOS: independent random +-1 octave jump on the primary note, matching
				// the original exactly -- this was computed (chaosF/displayChaos) but never
				// actually applied anywhere. Not gated by POLY (unlike HARMONY).
				if (chaosF > 0.2f && random::uniform() <= chaosF)
					pitch += (random::uniform() < 0.5f) ? 12 : -12;
				// Matches the original VST's juce::jlimit(0, 127, ...) -- our port was
				// missing this clamp entirely, allowing pitch to run arbitrarily high
				// when root+scale+octave stacked up.
				pitch = clamp(pitch, 0, 127);

				float pitchVolt = (pitch - 60) / 12.f;
				lastPitchVolt = pitchVolt;

				// Both voices always receive the same triggered note, every
				// step -- matches the real original exactly (routing never
				// gated which voice fires, only how audio got mixed, back
				// when this module had audio). Each voice's own GATE LEN
				// knob decides how long ITS gate output stays high.
				voice1GateCountdown = std::max(1, (int)std::round(lastStepIntervalSamples * params[VOICE1_GATE_LEN_PARAM].getValue()));
				voice2GateCountdown = std::max(1, (int)std::round(lastStepIntervalSamples * params[VOICE2_GATE_LEN_PARAM].getValue()));
			}
			for (int i = 0; i < 8; i++)
				lights[STEP_LIGHTS + i].setBrightness(i == localStep ? 1.f : 0.f);
		}

		// CV Pitch/Gate outputs, one pair per voice. Pitch is identical on
		// both (both voices always receive the same note); gate high/low
		// is independent per voice, driven by each voice's own GATE LEN
		// knob via the countdown decremented above.
		outputs[VOICE1_PITCH_OUTPUT].setVoltage(lastPitchVolt);
		outputs[VOICE2_PITCH_OUTPUT].setVoltage(lastPitchVolt);
		outputs[VOICE1_GATE_OUTPUT].setVoltage(voice1GateCountdown > 0 ? 10.f : 0.f);
		outputs[VOICE2_GATE_OUTPUT].setVoltage(voice2GateCountdown > 0 ? 10.f : 0.f);
	}
};

// =====================================================================
// Custom horizontal crossfader handle -- a real fader CAP that slides
// along the SCENE track, per explicit request that it must not be a
// knob. Drag horizontally; position maps directly to MORPH_PARAM.
// Static maroon body with an amber stroke/LED -- a fixed cap color,
// not a live per-position blend (position itself is shown by where it
// sits on the track).
// =====================================================================
struct HCrossfaderHandle : ParamWidget {
	float trackX0Px = 0.f, trackX1Px = 0.f;
	float centerY = 0.f;

	HCrossfaderHandle() {
		box.size = mm2px(Vec(9.0, 5.9));  // 30% thinner per explicit request (was 8.4mm tall)
	}

	void onButton(const ButtonEvent& e) override {
		ParamWidget::onButton(e);
		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			e.consume(this);
		}
	}

	void onDragMove(const DragMoveEvent& e) override {
		ParamWidget::onDragMove(e);
		ParamQuantity* pq = getParamQuantity();
		if (!pq) return;
		float zoom = getAbsoluteZoom();
		float trackWidthPx = std::max(1.f, trackX1Px - trackX0Px);
		float delta = e.mouseDelta.x / zoom / trackWidthPx;
		pq->setScaledValue(clamp((float)pq->getScaledValue() + delta, 0.f, 1.f));
	}

	// THE ACTUAL FIX: onDragMove only updated the param VALUE -- nothing
	// ever repositioned the widget itself, so it visually never moved no
	// matter how the value changed. step() runs every frame and now
	// re-derives box.pos.x from the current value each time.
	void step() override {
		ParamWidget::step();
		ParamQuantity* pq = getParamQuantity();
		float v = pq ? (float)pq->getScaledValue() : 0.f;
		float centerXPx = trackX0Px + (trackX1Px - trackX0Px) * v;
		box.pos.x = centerXPx - box.size.x / 2.f;
		box.pos.y = centerY - box.size.y / 2.f;
	}

	void draw(const DrawArgs& args) override {
		// distinct maroon/amber cap -- different from the faders' black/amber
		NVGcolor body = nvgRGB(0x3A, 0x14, 0x14);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, body);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0xB8, 0x72, 0x0A));
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
		// grip notches
		for (int i = -1; i <= 1; i++) {
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, box.size.x/2 + i*2.2f, box.size.y*0.22f);
			nvgLineTo(args.vg, box.size.x/2 + i*2.2f, box.size.y*0.78f);
			nvgStrokeColor(args.vg, nvgRGBA(0xC8, 0x8A, 0x2A, 180));
			nvgStrokeWidth(args.vg, 0.6f);
			nvgStroke(args.vg);
		}
		// embedded LED, glowing amber
		float cx = box.size.x/2, cy = box.size.y*0.5f;
		NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, 0.5f, 3.5f, nvgRGBA(0xFF, 0xB0, 0x4A, 220), nvgRGBA(0xFF, 0xB0, 0x4A, 0));
		nvgBeginPath(args.vg);
		nvgRect(args.vg, cx-4, cy-4, 8, 8);
		nvgFillPaint(args.vg, glow);
		nvgFill(args.vg);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, 1.3f);
		nvgFillColor(args.vg, nvgRGB(0xB0, 0xE0, 0xFF));
		nvgFill(args.vg);
	}
};

// Renders a single letter centered over another widget (e.g. inside a
// square button) using NanoVG's runtime text rendering -- this is
// different from the static panel SVG, which cannot render <text> at
// all; nvgText() at runtime has no such limitation.
struct LetterOverlay : TransparentWidget {
	std::string letter;
	NVGcolor color;

	LetterOverlay(std::string l, NVGcolor c) : letter(l), color(c) {}

	void draw(const DrawArgs& args) override {
		std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
		if (!font || !font->handle) return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, 10.f);
		nvgFillColor(args.vg, color);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
		nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, letter.c_str(), NULL);
	}
};

// Custom vertical fader -- bigger, more visible cap than the stock
// LEDSliderGreen, with an embedded LED. Uses the same step()-based
// repositioning technique as HCrossfaderHandle (drag updates the param
// value; step() re-derives screen position from that value every frame).
// Rack's stock RoundBlackKnob is a fixed-size SVG asset -- there's no
// built-in "85% size" variant. This wraps it with a NanoVG scale
// transform applied at draw time so the rendered knob is genuinely
// smaller, while box.size (used for hit-testing/click area) is scaled
// to match so clicking still lands correctly.
struct SmallKnob85 : RoundBlackKnob {
	static constexpr float SCALE = 0.85f;
	SmallKnob85() {
		box.size = box.size.mult(SCALE);
	}
	void draw(const DrawArgs& args) override {
		nvgSave(args.vg);
		nvgTranslate(args.vg, box.size.x / 2.f, box.size.y / 2.f);
		nvgScale(args.vg, SCALE, SCALE);
		nvgTranslate(args.vg, -box.size.x / 2.f / SCALE, -box.size.y / 2.f / SCALE);
		RoundBlackKnob::draw(args);
		nvgRestore(args.vg);
	}
};

// Voice ADSR/Timbre knobs need to be smaller than FEEL's -- an
// additional 85% on top of SmallKnob85 (0.85*0.85 = ~72% of stock size).
// Custom rotary knob for the 7 scene-scoped FEEL knobs (REST, LEGATO,
// RATE, ENTROPY, HARMONY, CHAOS, OCTAVES). Same reasoning as
// VFaderHandle: a stock knob has no hook for an externally-driven
// display value without corrupting the true param, so this is fully
// custom-drawn. displayValuePtr holds a RAW (engineering-unit) value;
// normalized against this knob's own param range at draw time, since
// FEEL knobs have varying ranges (0..1, -1..1, -3..3).
struct MorphKnob : ParamWidget {
	float* displayValuePtr = nullptr;
	bool dragging = false;

	// quantizeSteps: functional -- when >0, the knob's SCALED value (0..1)
	// is snapped to the nearest of N evenly-spaced positions as it's
	// turned. Used instead of ParamQuantity::snapEnabled for RATE, whose
	// raw 0..1 value is dual-purpose (BPM in free-run mode, but a 4-way
	// clock-division selector when CLOCK is patched) -- engine-level
	// snapping would round the raw value itself and break the division
	// selector. Widget-level snapping in scaled-value space works for
	// both uses at once. OCTAVES doesn't have this dual-purpose problem
	// so it uses ParamQuantity::snapEnabled directly instead.
	int quantizeSteps = 0;
	// numTicks: cosmetic only -- small reference marks drawn around the
	// rim, independent of quantizeSteps (RATE has 361 functional steps,
	// far too many to draw individually, so it gets a smaller decorative
	// set of reference ticks instead).
	int numTicks = 0;

	MorphKnob() {
		box.size = mm2px(Vec(7.3, 7.3));
	}

	void onButton(const ButtonEvent& e) override {
		ParamWidget::onButton(e);
		if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
			dragging = true;
			e.consume(this);
		}
	}

	void onDragEnd(const DragEndEvent& e) override {
		ParamWidget::onDragEnd(e);
		dragging = false;
	}

	void onDragMove(const DragMoveEvent& e) override {
		ParamWidget::onDragMove(e);
		ParamQuantity* pq = getParamQuantity();
		if (!pq) return;
		float zoom = getAbsoluteZoom();
		float delta = -e.mouseDelta.y / zoom / 200.f;  // standard Rack knob feel
		float v = clamp((float)pq->getScaledValue() + delta, 0.f, 1.f);
		if (quantizeSteps > 1) v = std::round(v * (quantizeSteps - 1)) / (quantizeSteps - 1);
		pq->setScaledValue(v);
	}

	void draw(const DrawArgs& args) override {
		ParamQuantity* pq = getParamQuantity();
		float v;
		if (dragging || !displayValuePtr || !pq) {
			v = pq ? (float)pq->getScaledValue() : 0.f;
		} else {
			// displayValuePtr is a raw blended value -- normalize against
			// this knob's actual param range (varies per FEEL knob)
			float range = pq->maxValue - pq->minValue;
			v = (range > 0.0001f) ? clamp((*displayValuePtr - pq->minValue) / range, 0.f, 1.f) : 0.f;
		}
		float cx = box.size.x / 2.f, cy = box.size.y / 2.f;
		float r = box.size.x / 2.f - 1.0f;
		float minAngle = -0.75f * (float)M_PI;
		float maxAngle = 0.75f * (float)M_PI;
		if (numTicks > 1) {
			for (int i = 0; i < numTicks; i++) {
				float ta = minAngle + (maxAngle - minAngle) * ((float)i / (float)(numTicks - 1));
				float x0 = cx + std::sin(ta) * (r + 0.6f), y0 = cy - std::cos(ta) * (r + 0.6f);
				float x1 = cx + std::sin(ta) * (r + 2.2f), y1 = cy - std::cos(ta) * (r + 2.2f);
				nvgBeginPath(args.vg);
				nvgMoveTo(args.vg, x0, y0);
				nvgLineTo(args.vg, x1, y1);
				nvgStrokeColor(args.vg, nvgRGB(0x8A, 0x7F, 0x6A));
				nvgStrokeWidth(args.vg, 0.6f);
				nvgLineCap(args.vg, NVG_ROUND);
				nvgStroke(args.vg);
			}
		}
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, r);
		nvgFillColor(args.vg, nvgRGB(0x18, 0x18, 0x18));
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x40, 0x40, 0x40));
		nvgStrokeWidth(args.vg, 1.0f);
		nvgStroke(args.vg);
		float angle = minAngle + (maxAngle - minAngle) * v;
		float ix = cx + std::sin(angle) * r * 0.75f;
		float iy = cy - std::cos(angle) * r * 0.75f;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, cx, cy);
		nvgLineTo(args.vg, ix, iy);
		nvgStrokeColor(args.vg, nvgRGB(0xE8, 0xE8, 0xE8));
		nvgStrokeWidth(args.vg, 1.4f);
		nvgLineCap(args.vg, NVG_ROUND);
		nvgStroke(args.vg);
	}
};

struct SmallKnobVoice : RoundBlackKnob {
	static constexpr float SCALE = 0.85f * 0.85f;
	SmallKnobVoice() {
		box.size = box.size.mult(SCALE);
	}
	void draw(const DrawArgs& args) override {
		nvgSave(args.vg);
		nvgTranslate(args.vg, box.size.x / 2.f, box.size.y / 2.f);
		nvgScale(args.vg, SCALE, SCALE);
		nvgTranslate(args.vg, -box.size.x / 2.f / SCALE, -box.size.y / 2.f / SCALE);
		RoundBlackKnob::draw(args);
		nvgRestore(args.vg);
	}
};

// Custom square momentary button widget. LEDBezel (used everywhere in
// earlier passes for "square" buttons) is actually a ROUND stock
// component -- that was a real mistake, not a style choice. This draws
// a genuine square, optionally shows a letter baked into the same
// widget (avoiding a separate overlay's z-order/position risk), and
// its fill color reflects a paired light's brightness read directly
// from the module.
struct SquareButton : ParamWidget {
	Module* mod = nullptr;
	int lightId = -1;
	NVGcolor litColor = nvgRGB(0xE0, 0x40, 0x40);
	NVGcolor unlitColor = nvgRGB(0x2A, 0x28, 0x24);
	std::string letter;
	NVGcolor letterColor = nvgRGB(0xE8, 0xE8, 0xE8);

	SquareButton() {
		box.size = mm2px(Vec(6.5, 6.5));
	}

	void onButton(const ButtonEvent& e) override {
		ParamWidget::onButton(e);
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (e.action == GLFW_PRESS) {
				ParamQuantity* pq = getParamQuantity();
				if (pq) pq->setValue(1.f);
				e.consume(this);
			}
		}
	}

	void onDragEnd(const DragEndEvent& e) override {
		ParamWidget::onDragEnd(e);
		ParamQuantity* pq = getParamQuantity();
		if (pq) pq->setValue(0.f);
	}

	void draw(const DrawArgs& args) override {
		bool lit = false;
		if (mod && lightId >= 0) lit = mod->lights[lightId].getBrightness() > 0.5f;
		NVGcolor fill = lit ? litColor : unlitColor;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 1.2f);
		nvgFillColor(args.vg, fill);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x1A, 0x18, 0x14));
		nvgStrokeWidth(args.vg, 1.0f);
		nvgStroke(args.vg);
		if (!letter.empty()) {
			std::shared_ptr<window::Font> font = APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
			if (font && font->handle) {
				nvgFontFaceId(args.vg, font->handle);
				nvgFontSize(args.vg, box.size.y * 0.55f);
				nvgFillColor(args.vg, letterColor);
				nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
				nvgText(args.vg, box.size.x / 2.f, box.size.y / 2.f, letter.c_str(), NULL);
			}
		}
	}
};

struct VFaderHandle : ParamWidget {
	float trackY0Px = 0.f, trackY1Px = 0.f;  // Y at value=1 (top), value=0 (bottom)
	float centerX = 0.f;
	float* displayValuePtr = nullptr;  // live Scene A/B blend, computed every frame by the module
	bool dragging = false;  // whether THIS fader is currently being dragged

	VFaderHandle() {
		box.size = mm2px(Vec(9.5, 3.5));  // height halved per explicit request
	}

	void onButton(const ButtonEvent& e) override {
		ParamWidget::onButton(e);
		if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
			if (e.action == GLFW_PRESS) {
				dragging = true;
				e.consume(this);
			}
		}
	}

	void onDragEnd(const DragEndEvent& e) override {
		ParamWidget::onDragEnd(e);
		dragging = false;
	}

	void onDragMove(const DragMoveEvent& e) override {
		ParamWidget::onDragMove(e);
		ParamQuantity* pq = getParamQuantity();
		if (!pq) return;
		float zoom = getAbsoluteZoom();
		float trackLenPx = std::max(1.f, trackY1Px - trackY0Px);
		// dragging UP (negative mouseDelta.y) should INCREASE the value
		float delta = -e.mouseDelta.y / zoom / trackLenPx;
		pq->setScaledValue(clamp((float)pq->getScaledValue() + delta, 0.f, 1.f));
	}

	void step() override {
		ParamWidget::step();
		// Matches the original VST's timerCallback exactly: every fader not
		// currently grabbed by the mouse continuously shows interpolate
		// (sceneA, sceneB, morph) -- a live, constant sweep as the
		// crossfader moves, not just an update on scene-focus-switch. The
		// underlying param is untouched by this (stays the true edit
		// target); only the on-screen position is display-only here.
		float v;
		if (dragging || !displayValuePtr) {
			ParamQuantity* pq = getParamQuantity();
			v = pq ? (float)pq->getScaledValue() : 0.f;
		} else {
			v = *displayValuePtr;
		}
		float centerYPx = trackY1Px + (trackY0Px - trackY1Px) * v;
		box.pos.x = centerX - box.size.x / 2.f;
		box.pos.y = centerYPx - box.size.y / 2.f;
	}

	void draw(const DrawArgs& args) override {
		// black body, translucent glowing LED, per explicit request
		NVGcolor body = nvgRGBA(0x0A, 0x0A, 0x0C, 255);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 1.2f);
		nvgFillColor(args.vg, body);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x2A, 0x2A, 0x2A));
		nvgStrokeWidth(args.vg, 0.9f);
		nvgStroke(args.vg);
		float cx = box.size.x/2, cy = box.size.y/2;
		NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, 0.3f, 3.0f, nvgRGBA(0xFF, 0xB0, 0x4A, 130), nvgRGBA(0xFF, 0xB0, 0x4A, 0));
		nvgBeginPath(args.vg);
		nvgRect(args.vg, cx-4, cy-2.5f, 8, 5);
		nvgFillPaint(args.vg, glow);
		nvgFill(args.vg);
	}
};

struct SpacesCommandWidget : ModuleWidget {
	SpacesCommandWidget(SpacesCommand* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SpacesCommand.svg")));

// I/O
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.4, 27.54)), module, SpacesCommand::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(42.44, 27.54)), module, SpacesCommand::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(65.47, 27.54)), module, SpacesCommand::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(88.5, 27.54)), module, SpacesCommand::VELOCITY_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(111.54, 27.54)), module, SpacesCommand::VOICE1_PITCH_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(134.57, 27.54)), module, SpacesCommand::VOICE1_GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(157.6, 27.54)), module, SpacesCommand::VOICE2_PITCH_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(180.64, 27.54)), module, SpacesCommand::VOICE2_GATE_OUTPUT));
		// Per-voice GATE LEN knobs, sitting directly above their GATE jack
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(134.57, 18.67)), module, SpacesCommand::VOICE1_GATE_LEN_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(180.64, 18.67)), module, SpacesCommand::VOICE2_GATE_LEN_PARAM));

		// FEEL: macro knobs -- custom MorphKnob for live scene-blend display, matching original VST
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(21.0, 51.36)), module, SpacesCommand::REST_PARAM);
			if (module) k->displayValuePtr = &module->displayRest;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(35.27, 51.36)), module, SpacesCommand::LEGATO_PARAM);
			if (module) k->displayValuePtr = &module->displayLegato;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(49.54, 51.36)), module, SpacesCommand::RATE_PARAM);
			if (module) k->displayValuePtr = &module->displayRate;
			k->quantizeSteps = 201;  // 1 BPM per detent across the real 40-240 BPM range
			k->numTicks = 9;  // decorative reference marks (too many BPM steps to tick individually)
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(63.81, 51.36)), module, SpacesCommand::ENTROPY_PARAM);
			if (module) k->displayValuePtr = &module->displayEntropy;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(78.08, 51.36)), module, SpacesCommand::HARMONY_PARAM);
			if (module) k->displayValuePtr = &module->displayHarmony;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(92.35, 51.36)), module, SpacesCommand::CHAOS_PARAM);
			if (module) k->displayValuePtr = &module->displayChaos;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(106.62, 51.36)), module, SpacesCommand::OCTAVES_PARAM);
			if (module) k->displayValuePtr = &module->displayOctaves;
			k->numTicks = 7;  // one tick per whole-octave position (-3..+3); snapping itself is engine-level (snapEnabled)
			addParam(k);
		}

		// CONTROLS: square toggle buttons, navy when engaged (real toggle now, not momentary-read)
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(136.12, 51.36)), module, SpacesCommand::LATCH_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::LATCH_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(147.1, 51.36)), module, SpacesCommand::ARPSEQ_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::ARPSEQ_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(158.08, 51.36)), module, SpacesCommand::POLY_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::POLY_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(169.06, 51.36)), module, SpacesCommand::FREEZE_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::FREEZE_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}
		{
			// Stub: cycles/lights as before, but has no effect on this
			// module's own output anymore -- reserved for a future
			// connector/voice module to read.
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(180.04, 51.36)), module, SpacesCommand::ROUTING_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::ROUTING_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}

		// SCENE: A/B focus (square, letter baked in, red glow when focused) + crossfader fader cap
		{
			auto* btnA = createParamCentered<SquareButton>(mm2px(Vec(20.0, 74.88)), module, SpacesCommand::SCENE_A_PARAM);
			btnA->mod = module;
			btnA->lightId = SpacesCommand::SCENE_A_LIGHT;
			btnA->litColor = nvgRGB(0xE0, 0x40, 0x40);
			btnA->letter = "A";
			addParam(btnA);
		}
		{
			auto* btnB = createParamCentered<SquareButton>(mm2px(Vec(111.24, 74.88)), module, SpacesCommand::SCENE_B_PARAM);
			btnB->mod = module;
			btnB->lightId = SpacesCommand::SCENE_B_LIGHT;
			btnB->litColor = nvgRGB(0xE0, 0x40, 0x40);
			btnB->letter = "B";
			addParam(btnB);
		}
		{
			auto* xfHandle = createParamCentered<HCrossfaderHandle>(mm2px(Vec((30.75+100.49)/2.f, 74.88)), module, SpacesCommand::MORPH_PARAM);
			xfHandle->trackX0Px = mm2px(Vec(30.75, 0)).x;
			xfHandle->trackX1Px = mm2px(Vec(100.49, 0)).x;
			xfHandle->centerY = mm2px(Vec(0, 74.88)).y;
			addParam(xfHandle);
		}

		// KEY: genuinely smaller stock component (Trimpot)
		addParam(createParamCentered<Trimpot>(mm2px(Vec(138.74, 73.38)), module, SpacesCommand::ROOT_KEY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(152.84, 73.38)), module, SpacesCommand::SCALE_TYPE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(166.94, 73.38)), module, SpacesCommand::DENSITY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(181.04, 73.38)), module, SpacesCommand::SWING_PARAM));

		// PATTERN: custom vertical faders (live scene-morph display) + step lights + DICE box
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(23.0, 92.08)), module, SpacesCommand::STEP_LIGHTS + 0));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(23.0, 105.88)), module, SpacesCommand::FADER_PARAM + 0);
			fader->trackY0Px = mm2px(Vec(0, 93.88)).y;
			fader->trackY1Px = mm2px(Vec(0, 117.88)).y;
			fader->centerX = mm2px(Vec(23.0, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[0];
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(36.72, 92.08)), module, SpacesCommand::STEP_LIGHTS + 1));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(36.72, 105.88)), module, SpacesCommand::FADER_PARAM + 1);
			fader->trackY0Px = mm2px(Vec(0, 93.88)).y;
			fader->trackY1Px = mm2px(Vec(0, 117.88)).y;
			fader->centerX = mm2px(Vec(36.72, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[1];
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(50.44, 92.08)), module, SpacesCommand::STEP_LIGHTS + 2));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(50.44, 105.88)), module, SpacesCommand::FADER_PARAM + 2);
			fader->trackY0Px = mm2px(Vec(0, 93.88)).y;
			fader->trackY1Px = mm2px(Vec(0, 117.88)).y;
			fader->centerX = mm2px(Vec(50.44, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[2];
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(64.15, 92.08)), module, SpacesCommand::STEP_LIGHTS + 3));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(64.15, 105.88)), module, SpacesCommand::FADER_PARAM + 3);
			fader->trackY0Px = mm2px(Vec(0, 93.88)).y;
			fader->trackY1Px = mm2px(Vec(0, 117.88)).y;
			fader->centerX = mm2px(Vec(64.15, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[3];
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(77.87, 92.08)), module, SpacesCommand::STEP_LIGHTS + 4));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(77.87, 105.88)), module, SpacesCommand::FADER_PARAM + 4);
			fader->trackY0Px = mm2px(Vec(0, 93.88)).y;
			fader->trackY1Px = mm2px(Vec(0, 117.88)).y;
			fader->centerX = mm2px(Vec(77.87, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[4];
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(91.59, 92.08)), module, SpacesCommand::STEP_LIGHTS + 5));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(91.59, 105.88)), module, SpacesCommand::FADER_PARAM + 5);
			fader->trackY0Px = mm2px(Vec(0, 93.88)).y;
			fader->trackY1Px = mm2px(Vec(0, 117.88)).y;
			fader->centerX = mm2px(Vec(91.59, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[5];
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(105.31, 92.08)), module, SpacesCommand::STEP_LIGHTS + 6));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(105.31, 105.88)), module, SpacesCommand::FADER_PARAM + 6);
			fader->trackY0Px = mm2px(Vec(0, 93.88)).y;
			fader->trackY1Px = mm2px(Vec(0, 117.88)).y;
			fader->centerX = mm2px(Vec(105.31, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[6];
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(119.03, 92.08)), module, SpacesCommand::STEP_LIGHTS + 7));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(119.03, 105.88)), module, SpacesCommand::FADER_PARAM + 7);
			fader->trackY0Px = mm2px(Vec(0, 93.88)).y;
			fader->trackY1Px = mm2px(Vec(0, 117.88)).y;
			fader->centerX = mm2px(Vec(119.03, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[7];
			addParam(fader);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(140.53, 103.78)), module, SpacesCommand::MELO_PARAM);
			btn->unlitColor = nvgRGB(0x6A, 0x42, 0x08);  // amber family -- matches the 8 pattern faders it randomizes
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(136.43, 112.88)), module, SpacesCommand::MELO_NUDGE_DOWN);
			btn->box.size = mm2px(Vec(3.6, 3.6));
			btn->box.pos = mm2px(Vec(136.43, 112.88)).minus(btn->box.size.div(2));
			btn->letter = "-";
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(144.63, 112.88)), module, SpacesCommand::MELO_NUDGE_UP);
			btn->box.size = mm2px(Vec(3.6, 3.6));
			btn->box.pos = mm2px(Vec(144.63, 112.88)).minus(btn->box.size.div(2));
			btn->letter = "+";
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(153.53, 103.78)), module, SpacesCommand::DICE_ARTI);
			btn->unlitColor = nvgRGB(0x5A, 0x1E, 0x1E);  // maroon family -- matches REST+LEGATO knob rings
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(149.43, 112.88)), module, SpacesCommand::ARTI_NUDGE_DOWN);
			btn->box.size = mm2px(Vec(3.6, 3.6));
			btn->box.pos = mm2px(Vec(149.43, 112.88)).minus(btn->box.size.div(2));
			btn->letter = "-";
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(157.63, 112.88)), module, SpacesCommand::ARTI_NUDGE_UP);
			btn->box.size = mm2px(Vec(3.6, 3.6));
			btn->box.pos = mm2px(Vec(157.63, 112.88)).minus(btn->box.size.div(2));
			btn->letter = "+";
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(166.53, 103.78)), module, SpacesCommand::DICE_TIME);
			btn->unlitColor = nvgRGB(0x5A, 0x40, 0x18);  // brass/ochre family -- matches RATE+OCTAVES knob rings
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(162.43, 112.88)), module, SpacesCommand::TIME_NUDGE_DOWN);
			btn->box.size = mm2px(Vec(3.6, 3.6));
			btn->box.pos = mm2px(Vec(162.43, 112.88)).minus(btn->box.size.div(2));
			btn->letter = "-";
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(170.63, 112.88)), module, SpacesCommand::TIME_NUDGE_UP);
			btn->box.size = mm2px(Vec(3.6, 3.6));
			btn->box.pos = mm2px(Vec(170.63, 112.88)).minus(btn->box.size.div(2));
			btn->letter = "+";
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(179.53, 103.78)), module, SpacesCommand::DICE_NAVY);
			btn->unlitColor = nvgRGB(0x1E, 0x30, 0x48);  // navy family -- matches ENTROPY+HARMONY+CHAOS knob rings
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(175.43, 112.88)), module, SpacesCommand::NAVY_NUDGE_DOWN);
			btn->box.size = mm2px(Vec(3.6, 3.6));
			btn->box.pos = mm2px(Vec(175.43, 112.88)).minus(btn->box.size.div(2));
			btn->letter = "-";
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(183.63, 112.88)), module, SpacesCommand::NAVY_NUDGE_UP);
			btn->box.size = mm2px(Vec(3.6, 3.6));
			btn->box.pos = mm2px(Vec(183.63, 112.88)).minus(btn->box.size.div(2));
			btn->letter = "+";
			addParam(btn);
		}

	}
};

Model* modelSpacesCommand = createModel<SpacesCommand, SpacesCommandWidget>("SpacesCommand");
