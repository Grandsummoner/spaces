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
		MELO_PARAM,
		SCENE_A_PARAM, SCENE_B_PARAM, MORPH_PARAM,
		LATCH_PARAM, ARPSEQ_PARAM, STRUM_PARAM, FREEZE_PARAM, ROUTING_PARAM,
		REST_PARAM, DICE_ARTI, LEGATO_PARAM, RATE_PARAM, DICE_TIME,
		ENTROPY_PARAM, HARMONY_PARAM, CHAOS_PARAM, DICE_NAVY, OCTAVES_PARAM,
		OCT_BIAS_DOWN_PARAM, OCT_BIAS_UP_PARAM,
		ROOT_KEY_PARAM, SCALE_TYPE_PARAM, DENSITY_PARAM, SWING_PARAM,
		PARAMS_LEN
	};
	enum InputId { VOCT_INPUT, GATE_INPUT, VELOCITY_INPUT, CLOCK_INPUT, RESET_INPUT, INPUTS_LEN };
	enum OutputId { A_PITCH_OUTPUT, A_GATE_OUTPUT, A_VEL_OUTPUT, B_PITCH_OUTPUT, B_GATE_OUTPUT, B_VEL_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		ENUMS(STEP_LIGHTS, 8), SCENE_A_LIGHT, SCENE_B_LIGHT,
		LATCH_LIGHT, ARPSEQ_LIGHT, STRUM_LIGHT, FREEZE_LIGHT, ROUTING_LIGHT,
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
	// Per-stream gate-output countdown: samples remaining with the gate
	// held high. GATE LEN is no longer a per-stream knob (removed to reduce
	// panel clutter -- finer per-stream gate control belongs on a future
	// downstream module, not stacked onto Command); both streams now use a
	// fixed 95% of the step interval, matching the original single-voice
	// default exactly.
	static constexpr float kGateLenFraction = 0.95f;
	int aGateCountdown = -1;  // -1 = gate currently low
	int bGateCountdown = -1;
	int aGateTotalSamples = 1, bGateTotalSamples = 1;  // full gate length, for STRUM sub-timing

	std::vector<int> heldNotes, latchedNotes;
	std::vector<float> heldVelocities, latchedVelocities;  // 0-1, parallel to heldNotes/latchedNotes by index
	float lastAPitchVolt = 0.f, lastBPitchVolt = 0.f;
	float lastAVelVolt = 0.f, lastBVelVolt = 0.f;
	// STRUM: chord tones for the currently-sounding note on each stream,
	// rolled one at a time across the gate window instead of sounding
	// together (this module has no polyphonic wires -- see design notes).
	std::vector<int> aStrumPitches, bStrumPitches;
	// FREEZE snapshot state: captured once on FREEZE's rising edge,
	// matching the original exactly -- while engaged, the engine reads
	// these frozen values instead of live knobs/held notes, so playback
	// continues exactly as it was the instant FREEZE was engaged, even if
	// knobs keep moving or notes get released/changed. The panel's live
	// display values (displayRest etc.) are unaffected either way, same
	// as the original's UI.
	bool freezeWasOn = false;
	// Whole-scene snapshots (plus the MORPH position itself) rather than
	// individual blended scalars -- needed so Split mode's independent
	// per-scene values are ALSO protected by FREEZE, not just Blend
	// mode's crossfaded ones. Everything effective is recomputed from
	// these frozen copies + frozenMorph while engaged, which reproduces
	// the old single-scalar-snapshot behavior exactly for Blend mode
	// (crossfade of two frozen things at a frozen position == a frozen
	// crossfade result) while also covering Split mode for free.
	SceneState frozenSceneA, frozenSceneB;
	float frozenMorph = 0.f;
	std::vector<int> frozenHeldNotes, frozenLatchedNotes;
	std::vector<float> frozenHeldVelocities, frozenLatchedVelocities;
	dsp::SchmittTrigger sceneATrig, sceneBTrig, clockTrig, resetTrig;
	dsp::SchmittTrigger diceArtiTrig, diceTimeTrig, diceNavyTrig, meloTrig;

	// LATCH/ARP-SEQ/POLY/FREEZE/ROUTING are also momentary buttons that
	// must TOGGLE persisted state -- same bug/fix as the wave buttons.
	// Previously read directly from the momentary param, so they only
	// registered "on" while physically held, never actually latched.
	bool latchOnState = false, arpSeqOnState = false, strumOnState = false;
	bool freezeOnState = false;
	// ROUTING: BLEND (0) -- A Out and B Out carry the same single morphed
	// stream, as they always have. SPLIT (1) -- A Out plays Scene A's own
	// pattern and B Out plays Scene B's own pattern, genuinely
	// independently (see the split-bucket params below), with the
	// crossfader controlling each stream's presence (fades to true
	// silence at its own extreme, folded into DENSITY rather than a
	// separate roll -- see process()).
	int routingState = 0;  // 0=Blend, 1=Split
	dsp::SchmittTrigger latchTrig, arpSeqTrig, strumTrig, freezeTrig, routingTrig;
	dsp::SchmittTrigger octBiasDownTrig, octBiasUpTrig;

	SpacesCommand() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		for (int i = 0; i < 8; i++)
			configParam(FADER_PARAM + i, 0.f, 1.f, 1.f, string::f("Step %d probability", i + 1), "%", 0, 100);
		configButton(MELO_PARAM, "Randomize pattern (MELO)");
		configButton(SCENE_A_PARAM, "Focus Scene A");
		configButton(SCENE_B_PARAM, "Focus Scene B");
		configParam(MORPH_PARAM, 0.f, 1.f, 0.f, "Scene morph", "%", 0, 100);
		configSwitch(LATCH_PARAM, 0.f, 1.f, 0.f, "Latch (holds the last note fed into V/OCT+GATE after release)", {"Off", "On"});
		configSwitch(ARPSEQ_PARAM, 0.f, 1.f, 0.f, "Arp (cycles held/latched notes) / Seq (fixed root+scale pattern, runs on its own)", {"Arp", "Seq"});
		configSwitch(STRUM_PARAM, 0.f, 1.f, 0.f, "Strum (rolls a HARMONY chord's tones one at a time within the step's gate window, instead of only the root)", {"Off", "On"});
		configSwitch(FREEZE_PARAM, 0.f, 1.f, 0.f, "Freeze (snapshots the pattern and keeps playing it regardless of knob/note changes)", {"Off", "On"});
		configSwitch(ROUTING_PARAM, 0.f, 1.f, 0.f, "Routing", {"Blend", "Split"});
		configParam(REST_PARAM, 0.f, 1.f, 0.1f, "Rest probability", "%", 0, 100);
		configButton(DICE_ARTI, "Randomize Rest+Legato (ARTI)");
		configParam(LEGATO_PARAM, 0.f, 1.f, 0.5f, "Legato", "%", 0, 100);
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate (free-run BPM, or 1/4-1/32 note subdivision of the incoming CLOCK's beat)", " BPM", 0, 200, 40);
		configButton(DICE_TIME, "Randomize Rate+Octaves (TIME)");
		configParam(ENTROPY_PARAM, -1.f, 1.f, 0.f, "Entropy (play direction)");
		configParam(HARMONY_PARAM, 0.f, 1.f, 0.f, "Harmony (chord size: 0.25-0.5=2 notes, 0.5-0.75=3, 0.75+=4 -- with STRUM on, rolled one at a time within the gate window)");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.f, "Chaos");
		configButton(DICE_NAVY, "Randomize Entropy+Harmony+Chaos (NAVY)");
		configParam(OCTAVES_PARAM, -3.f, 3.f, 0.f, "Octave shift")->snapEnabled = true;
		configButton(OCT_BIAS_DOWN_PARAM, "Bias unfocused scene's octave DOWN from focused (0-3 octaves, random)");
		configButton(OCT_BIAS_UP_PARAM, "Bias unfocused scene's octave UP from focused (0-3 octaves, random)");
		configParam(ROOT_KEY_PARAM, 0.f, 11.f, 0.f, "Root key");
		getParamQuantity(ROOT_KEY_PARAM)->snapEnabled = true;
		configParam(SCALE_TYPE_PARAM, 0.f, 9.f, 0.f, "Scale");
		getParamQuantity(SCALE_TYPE_PARAM)->snapEnabled = true;
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.5f, "Density", "%", 0, 100);
		configParam(SWING_PARAM, 0.f, 1.f, 0.f, "Swing", "%", 0, 100);


		configInput(VOCT_INPUT, "1V/oct pitch (poly, held notes)");
		configInput(GATE_INPUT, "Gate (poly, held notes)");
		configInput(VELOCITY_INPUT, "Velocity (poly, from a MIDI-CV module -- replaces the generated 3-tier velocity outright when patched, per-channel matched to V/OCT+GATE)");
		configInput(CLOCK_INPUT, "Clock (patched = external run/stop; unpatched = free-run on RATE)");
		configInput(RESET_INPUT, "Reset (rising edge jumps the sequencer back to step 1)");
		configOutput(A_PITCH_OUTPUT, "A pitch (1V/oct)");
		configOutput(A_GATE_OUTPUT, "A gate");
		configOutput(A_VEL_OUTPUT, "A velocity");
		configOutput(B_PITCH_OUTPUT, "B pitch (1V/oct)");
		configOutput(B_GATE_OUTPUT, "B gate");
		configOutput(B_VEL_OUTPUT, "B velocity");
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

	// OCT bias buttons: sets the UNFOCUSED scene's octave relative to the
	// FOCUSED scene (the focused scene's own octave is always just the
	// anchor -- never modified here). dir is -1 (bias down) or +1 (bias
	// up); magnitude is a surprise 0-3 octaves, clamped to the hard
	// -3..3 range. This is a one-shot generation action, not a
	// continuously-enforced invariant -- you can still turn the
	// unfocused scene's OCTAVES knob apart afterward if you want to.
	void applyOctaveBias(int dir) {
		SceneState& focused = focusB ? sceneB : sceneA;
		SceneState& unfocused = focusB ? sceneA : sceneB;
		int magnitude = (int)std::floor(random::uniform() * 4.f);  // 0,1,2,3
		magnitude = clamp(magnitude, 0, 3);
		unfocused.octaves = clamp(focused.octaves + dir * (float)magnitude, -3.f, 3.f);
		// Focused scene's own octaves (and its live OCTAVES_PARAM display)
		// are untouched -- only the unfocused scene's stored value changes,
		// invisibly until you switch focus onto it.
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

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "latchOn", json_boolean(latchOnState));
		json_object_set_new(rootJ, "arpSeqOn", json_boolean(arpSeqOnState));
		json_object_set_new(rootJ, "strumOn", json_boolean(strumOnState));
		json_object_set_new(rootJ, "freezeOn", json_boolean(freezeOnState));
		json_object_set_new(rootJ, "routingState", json_integer(routingState));
		// Fix: which scene was focused, and BOTH scenes' full stored data,
		// were never saved -- only the live params were (Rack's own native
		// serialization), which only reflects whichever scene happened to
		// be focused at save time. The unfocused scene's data silently
		// reverted to construction defaults on every reload, and the
		// SCENE A/B focus light could show the wrong one entirely.
		json_object_set_new(rootJ, "focusB", json_boolean(focusB));
		auto sceneToJson = [](const SceneState& s) {
			json_t* sJ = json_object();
			json_t* fJ = json_array();
			for (int i = 0; i < 8; i++) json_array_append_new(fJ, json_real(s.faders[i]));
			json_object_set_new(sJ, "faders", fJ);
			json_object_set_new(sJ, "rest", json_real(s.rest));
			json_object_set_new(sJ, "legato", json_real(s.legato));
			json_object_set_new(sJ, "rate", json_real(s.rate));
			json_object_set_new(sJ, "entropy", json_real(s.entropy));
			json_object_set_new(sJ, "harmony", json_real(s.harmony));
			json_object_set_new(sJ, "chaos", json_real(s.chaos));
			json_object_set_new(sJ, "octaves", json_real(s.octaves));
			return sJ;
		};
		json_object_set_new(rootJ, "sceneA", sceneToJson(sceneA));
		json_object_set_new(rootJ, "sceneB", sceneToJson(sceneB));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* latchJ = json_object_get(rootJ, "latchOn");
		if (latchJ) latchOnState = json_boolean_value(latchJ);
		json_t* arpSeqJ = json_object_get(rootJ, "arpSeqOn");
		if (arpSeqJ) arpSeqOnState = json_boolean_value(arpSeqJ);
		json_t* strumJ = json_object_get(rootJ, "strumOn");
		if (strumJ) strumOnState = json_boolean_value(strumJ);
		json_t* freezeJ = json_object_get(rootJ, "freezeOn");
		if (freezeJ) freezeOnState = json_boolean_value(freezeJ);
		json_t* routingJ = json_object_get(rootJ, "routingState");
		if (routingJ) routingState = clamp((int)json_integer_value(routingJ), 0, 1);
		json_t* focusBJ = json_object_get(rootJ, "focusB");
		if (focusBJ) focusB = json_boolean_value(focusBJ);
		auto sceneFromJson = [](json_t* sJ, SceneState& s) {
			if (!sJ) return;
			json_t* fJ = json_object_get(sJ, "faders");
			if (fJ) for (int i = 0; i < 8 && i < (int)json_array_size(fJ); i++)
				s.faders[i] = (float)json_real_value(json_array_get(fJ, i));
			json_t* v;
			if ((v = json_object_get(sJ, "rest"))) s.rest = (float)json_real_value(v);
			if ((v = json_object_get(sJ, "legato"))) s.legato = (float)json_real_value(v);
			if ((v = json_object_get(sJ, "rate"))) s.rate = (float)json_real_value(v);
			if ((v = json_object_get(sJ, "entropy"))) s.entropy = (float)json_real_value(v);
			if ((v = json_object_get(sJ, "harmony"))) s.harmony = (float)json_real_value(v);
			if ((v = json_object_get(sJ, "chaos"))) s.chaos = (float)json_real_value(v);
			if ((v = json_object_get(sJ, "octaves"))) s.octaves = (float)json_real_value(v);
		};
		sceneFromJson(json_object_get(rootJ, "sceneA"), sceneA);
		sceneFromJson(json_object_get(rootJ, "sceneB"), sceneB);
	}

	void process(const ProcessArgs& args) override {
		if (sceneATrig.process(params[SCENE_A_PARAM].getValue())) { focusB = false; loadSceneIntoParams(sceneA); }
		if (sceneBTrig.process(params[SCENE_B_PARAM].getValue())) { focusB = true; loadSceneIntoParams(sceneB); }
		lights[SCENE_A_LIGHT].setBrightness(!focusB);
		lights[SCENE_B_LIGHT].setBrightness(focusB);

		// Mode toggle LEDs: navy when on, unlit when off
		if (latchTrig.process(params[LATCH_PARAM].getValue())) latchOnState = !latchOnState;
		if (arpSeqTrig.process(params[ARPSEQ_PARAM].getValue())) arpSeqOnState = !arpSeqOnState;
		if (strumTrig.process(params[STRUM_PARAM].getValue())) strumOnState = !strumOnState;
		if (octBiasDownTrig.process(params[OCT_BIAS_DOWN_PARAM].getValue())) applyOctaveBias(-1);
		if (octBiasUpTrig.process(params[OCT_BIAS_UP_PARAM].getValue())) applyOctaveBias(1);
		if (freezeTrig.process(params[FREEZE_PARAM].getValue())) freezeOnState = !freezeOnState;
		if (routingTrig.process(params[ROUTING_PARAM].getValue())) routingState = (routingState + 1) % 2;
		lights[LATCH_LIGHT].setBrightness(latchOnState ? 1.f : 0.f);
		lights[ARPSEQ_LIGHT].setBrightness(arpSeqOnState ? 1.f : 0.f);
		lights[STRUM_LIGHT].setBrightness(strumOnState ? 1.f : 0.f);
		lights[FREEZE_LIGHT].setBrightness(freezeOnState ? 1.f : 0.f);
		lights[ROUTING_LIGHT].setBrightness(routingState ? 1.f : 0.f);

		if (meloTrig.process(params[MELO_PARAM].getValue())) randomizeMelo();
		if (diceArtiTrig.process(params[DICE_ARTI].getValue())) randomizeArti();
		if (diceTimeTrig.process(params[DICE_TIME].getValue())) randomizeTime();
		if (diceNavyTrig.process(params[DICE_NAVY].getValue())) randomizeNavy();

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
		heldVelocities.clear();
		bool velPatched = inputs[VELOCITY_INPUT].isConnected();
		int channels = std::max(inputs[VOCT_INPUT].getChannels(), 1);
		for (int c = 0; c < channels; c++) {
			if (inputs[GATE_INPUT].getVoltage(c) >= 1.f) {
				int pitch = 60 + (int)std::round(inputs[VOCT_INPUT].getVoltage(c) * 12.f);
				heldNotes.push_back(pitch);
				// Captured in lockstep with V/OCT+GATE, same channel index
				// -- a real poly MIDI-CV module presents velocity the same
				// way. 0-10V normalized to 0-1.
				float vel = velPatched ? clamp(inputs[VELOCITY_INPUT].getVoltage(c) / 10.f, 0.f, 1.f) : 0.f;
				heldVelocities.push_back(vel);
			}
		}
		bool latchOn = latchOnState;
		bool freezeOn = freezeOnState;
		if (!heldNotes.empty() && latchOn) { latchedNotes = heldNotes; latchedVelocities = heldVelocities; }

		// FREEZE: capture a full snapshot the instant it's engaged (rising
		// edge only), matching the original exactly -- now whole scenes +
		// MORPH position, see the field comment above.
		if (freezeOn && !freezeWasOn) {
			frozenSceneA = sceneA; frozenSceneB = sceneB; frozenMorph = morph;
			frozenHeldNotes = heldNotes; frozenLatchedNotes = latchedNotes;
			frozenHeldVelocities = heldVelocities; frozenLatchedVelocities = latchedVelocities;
		}
		freezeWasOn = freezeOn;

		// Effective scenes/morph: the frozen snapshot while FREEZE is
		// engaged, live otherwise. Everything below the note-generation
		// point reads these, never sceneA/sceneB/morph directly.
		const SceneState& effA = freezeOn ? frozenSceneA : sceneA;
		const SceneState& effB = freezeOn ? frozenSceneB : sceneB;
		float morphEff = freezeOn ? frozenMorph : morph;
		float entropyEff = crossfade(effA.entropy, effB.entropy, morphEff);  // RATE/ENTROPY: always shared, one engine
		float rate01Eff = crossfade(effA.rate, effB.rate, morphEff);
		std::vector<int>& notesToPlay = freezeOn
			? (latchOn ? frozenLatchedNotes : frozenHeldNotes)
			: (latchOn ? latchedNotes : heldNotes);
		std::vector<float>& velocitiesToPlay = freezeOn
			? (latchOn ? frozenLatchedVelocities : frozenHeldVelocities)
			: (latchOn ? latchedVelocities : heldVelocities);

		// RESET: rising edge jumps the sequencer back to step 1 immediately
		// -- repositions the pointer and updates the step lights right away,
		// but doesn't itself fire a note; playback of step 1 happens
		// normally on the next clock/free-run step trigger, same as any
		// other step. Standard eurorack convention -- scoped to position
		// only, doesn't touch clock phase/timing or note-hold state.
		if (resetTrig.process(inputs[RESET_INPUT].getVoltage())) {
			currentStep = 0;
			goingForward = true;
			for (int i = 0; i < 8; i++)
				lights[STEP_LIGHTS + i].setBrightness(i == 0 ? 1.f : 0.f);
		}

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
			int rateIdx = clamp((int)std::round(rate01Eff * 3.f), 0, 3);
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
			// Deliberate departure from source here: the original always
			// gates on a held note (or FREEZE), because it's a MIDI
			// arpeggiator built to be played from a keyboard. Command is a
			// standalone eurorack sequencer now -- ARP mode still needs a
			// held/latched note or FREEZE (it has nothing to arpeggiate
			// without one), but SEQ mode never reads notesToPlay for pitch
			// at all, so gating it on a held note was pure inherited VST
			// behavior with no modular justification. SEQ mode now runs on
			// CLOCK/free-run alone, unconditionally.
			bool playing = arpSeqOnState ? (!notesToPlay.empty() || freezeOn) : true;
			double bpm = 40.0 + rate01Eff * 200.0;  // matches original: 40-240 BPM free-run
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

		// STRUM: while a gate is open and its stream has more than one
		// chord tone queued, roll through them evenly across the gate
		// window (one continuous gate, pitch stepping underneath -- this
		// module has no polyphonic wires, see design notes). Recomputed
		// every sample so the pitch output updates smoothly as the gate
		// progresses.
		if (aGateCountdown > 0 && aStrumPitches.size() > 1) {
			float progress = 1.f - (float)aGateCountdown / (float)std::max(1, aGateTotalSamples);
			int idx = clamp((int)(progress * aStrumPitches.size()), 0, (int)aStrumPitches.size() - 1);
			lastAPitchVolt = (aStrumPitches[idx] - 60) / 12.f;
		}
		if (bGateCountdown > 0 && bStrumPitches.size() > 1) {
			float progress = 1.f - (float)bGateCountdown / (float)std::max(1, bGateTotalSamples);
			int idx = clamp((int)(progress * bStrumPitches.size()), 0, (int)bStrumPitches.size() - 1);
			lastBPitchVolt = (bStrumPitches[idx] - 60) / 12.f;
		}
		// Count down each stream's gate-high duration; when it reaches zero
		// the gate output drops low (see the end of process() below).
		if (aGateCountdown > 0) { aGateCountdown--; }
		if (bGateCountdown > 0) { bGateCountdown--; }

		if (stepTriggered) {
			int playDirection = 0;
			if (entropyEff >= -0.1f && entropyEff <= 0.1f) playDirection = 0;
			else if (entropyEff > 0.1f && entropyEff <= 0.5f) playDirection = 1;
			else if (entropyEff > 0.5f) playDirection = 2;
			else if (entropyEff < -0.1f && entropyEff >= -0.5f) playDirection = 3;
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

			// ARP mode has nothing to arpeggiate without a held/latched note
			// (or FREEZE) -- rest rather than falling back to a SEQ-style
			// pitch. SEQ mode never needed notesToPlay for pitch, so it's
			// never gated here (see the free-run "playing" gate above,
			// which already made SEQ mode note-independent; this closes
			// the same gap for the CLOCK-patched path, where steps always
			// fire regardless of held notes).
			bool arpNeedsNotes = arpSeqOnState && notesToPlay.empty() && !freezeOn;
			float density = params[DENSITY_PARAM].getValue();

			// External velocity (MIDI-CV, via VELOCITY_INPUT): ARP mode
			// matches the exact note index that's about to play; SEQ mode
			// has no principled note-to-velocity pairing (pitch ignores
			// which keys are held), so it averages everything currently
			// held. One shared value -- one physical performer -- applied
			// identically to both streams below.
			float externalVelocity = 0.f;
			if (velPatched && !velocitiesToPlay.empty()) {
				if (arpSeqOnState && !notesToPlay.empty())
					externalVelocity = velocitiesToPlay[localStep % velocitiesToPlay.size()];
				else {
					float sum = 0.f;
					for (float v : velocitiesToPlay) sum += v;
					externalVelocity = sum / velocitiesToPlay.size();
				}
			}

			static const std::vector<std::vector<int>> scales = {
				{0,2,4,5,7,9,11,12}, {0,2,3,5,7,8,10,12}, {0,3,5,7,10,12,15,17}, {0,2,4,7,9,12,14,16},
				{0,2,3,5,7,9,10,12}, {0,1,3,5,7,8,10,12}, {0,2,4,6,7,9,11,12}, {0,2,4,5,7,9,10,12},
				{0,2,3,5,7,8,11,12}, {0,2,3,5,7,9,11,12}
			};
			int rootKeyIdx = (int)std::round(params[ROOT_KEY_PARAM].getValue());
			int scaleIdx = (int)std::round(params[SCALE_TYPE_PARAM].getValue());
			const std::vector<int>& scaleOffsets = scales[clamp(scaleIdx, 0, 9)];

			// One stream's worth of note generation. weight is this
			// stream's crossfader presence (always 1.0 in Blend mode --
			// Split mode folds the equal-power weight straight into
			// DENSITY's existing reshape rather than a separate roll, so
			// a stream fades to true silence only at its own crossfader
			// extreme, gradually, same mechanism REST/DENSITY already use).
			struct StreamOut { bool fired = false; std::vector<int> pitches; float velocity = 0.f; };
			auto generateStream = [&](float restIn, float legatoIn, float harmonyIn, float chaosIn,
			                          float octavesIn, float faderStep, float weight) -> StreamOut {
				StreamOut out;
				float restAdj = restIn;
				if (legatoIn >= 0.8f) restAdj = restIn * clamp((1.f - legatoIn) / 0.2f, 0.f, 1.f);

				float effDensity = density * weight;
				float faderProb = faderStep;
				if (effDensity < 0.5f) faderProb = faderStep * (effDensity / 0.5f);
				else if (effDensity > 0.5f) faderProb = faderStep + (1.f - faderStep) * ((effDensity - 0.5f) / 0.5f);

				if (arpNeedsNotes || !(random::uniform() <= faderProb) || (random::uniform() <= restAdj))
					return out;

				int rawPitch, octaveBase;
				if (arpSeqOnState && !notesToPlay.empty()) {
					rawPitch = notesToPlay[localStep % notesToPlay.size()];
					octaveBase = ((rawPitch - rootKeyIdx) / 12) * 12 + rootKeyIdx;
				} else {
					rawPitch = 48 + rootKeyIdx + scaleOffsets[localStep % (int)scaleOffsets.size()];
					octaveBase = ((rawPitch - rootKeyIdx) / 12) * 12 + rootKeyIdx;
				}
				int octaveShiftLocal = (int)std::round(octavesIn);
				auto applyChaosAndClamp = [&](int p) {
					if (chaosIn > 0.2f && random::uniform() <= chaosIn)
						p += (random::uniform() < 0.5f) ? 12 : -12;
					return clamp(p + 12 * octaveShiftLocal, 0, 127);
				};

				out.pitches.push_back(applyChaosAndClamp(rawPitch));
				// HARMONY still decides chord size (2/3/4 notes for
				// 0.25-0.5/0.5-0.75/0.75+); STRUM decides whether those
				// extra tones get rolled or dropped -- see design notes
				// on why this replaced POLY (no polyphonic wires here).
				int maxAllowedNotes = (harmonyIn > 0.25f && harmonyIn < 0.5f) ? 2
				                     : (harmonyIn >= 0.5f && harmonyIn < 0.75f) ? 3
				                     : (harmonyIn >= 0.75f) ? 4 : 1;
				if (strumOnState && maxAllowedNotes > 1) {
					for (int n = 1; n < maxAllowedNotes; ++n) {
						int tone = octaveBase + scaleOffsets[(localStep + n * 2) % (int)scaleOffsets.size()];
						out.pitches.push_back(applyChaosAndClamp(tone));
					}
				}

				// 3-tier velocity (soft/medium/accent), leaned by DENSITY
				// toward soft at low density and accent at high density.
				// Independent of the fire-probability roll above -- FADER
				// only ever affects whether a step fires, nothing else.
				static const float kTiers[3] = {0.25f, 0.60f, 1.00f};
				float pSoft = 0.8f + density * (0.05f - 0.8f);
				float pAccent = 0.05f + density * (0.8f - 0.05f);
				float pMedium = clamp(1.f - pSoft - pAccent, 0.f, 1.f);
				float total = std::max(0.0001f, pSoft + pMedium + pAccent);
				pSoft /= total; pMedium /= total;
				float r = random::uniform();
				float tierVel = (r < pSoft) ? kTiers[0] : (r < pSoft + pMedium) ? kTiers[1] : kTiers[2];
				out.velocity = velPatched ? externalVelocity : tierVel;
				out.fired = true;
				return out;
			};

			bool splitMode = (routingState == 1);
			float wA = splitMode ? std::cos(morphEff * float(M_PI) / 2.f) : 1.f;
			float wB = splitMode ? std::sin(morphEff * float(M_PI) / 2.f) : 1.f;

			StreamOut rA, rB;
			if (splitMode) {
				rA = generateStream(effA.rest, effA.legato, effA.harmony, effA.chaos, effA.octaves, effA.faders[localStep], wA);
				rB = generateStream(effB.rest, effB.legato, effB.harmony, effB.chaos, effB.octaves, effB.faders[localStep], wB);
			} else {
				// Blend: one shared stream, computed once and mirrored to
				// both outputs identically -- A and B are only ever two
				// wires carrying the same signal here, never independently
				// rolled (that would make Blend behave like a hidden second
				// Split, which it explicitly is not).
				float restBlend = crossfade(effA.rest, effB.rest, morphEff);
				float legatoBlend = crossfade(effA.legato, effB.legato, morphEff);
				float harmonyBlend = crossfade(effA.harmony, effB.harmony, morphEff);
				float chaosBlend = crossfade(effA.chaos, effB.chaos, morphEff);
				float octavesBlend = crossfade(effA.octaves, effB.octaves, morphEff);
				float faderBlend = crossfade(effA.faders[localStep], effB.faders[localStep], morphEff);
				rA = generateStream(restBlend, legatoBlend, harmonyBlend, chaosBlend, octavesBlend, faderBlend, 1.f);
				rB = rA;
			}

			if (rA.fired) {
				lastAPitchVolt = (rA.pitches[0] - 60) / 12.f;
				aStrumPitches = rA.pitches;
				aGateTotalSamples = std::max(1, (int)std::round(lastStepIntervalSamples * kGateLenFraction));
				aGateCountdown = aGateTotalSamples;
				lastAVelVolt = rA.velocity * 10.f;  // 0-10V
			}
			if (rB.fired) {
				lastBPitchVolt = (rB.pitches[0] - 60) / 12.f;
				bStrumPitches = rB.pitches;
				bGateTotalSamples = std::max(1, (int)std::round(lastStepIntervalSamples * kGateLenFraction));
				bGateCountdown = bGateTotalSamples;
				lastBVelVolt = rB.velocity * 10.f;
			}
			for (int i = 0; i < 8; i++)
				lights[STEP_LIGHTS + i].setBrightness(i == localStep ? 1.f : 0.f);
		}

		// CV Pitch/Gate/Velocity outputs, one triplet per stream. In Blend
		// mode A and B always carry the identical signal; in Split mode
		// each stream is genuinely independent (see generateStream above).
		// Velocity holds its last value between fires, same as pitch --
		// it's sample-and-held on trigger, never reset or drooped between
		// notes, and a REST'd/silenced step simply leaves it untouched.
		outputs[A_PITCH_OUTPUT].setVoltage(lastAPitchVolt);
		outputs[B_PITCH_OUTPUT].setVoltage(lastBPitchVolt);
		outputs[A_GATE_OUTPUT].setVoltage(aGateCountdown > 0 ? 10.f : 0.f);
		outputs[B_GATE_OUTPUT].setVoltage(bGateCountdown > 0 ? 10.f : 0.f);
		outputs[A_VEL_OUTPUT].setVoltage(lastAVelVolt);
		outputs[B_VEL_OUTPUT].setVoltage(lastBVelVolt);
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

// Half-width variant of SquareButton for the split OCT bias button (one
// column-slot in the DICE/RANDOM cluster, rendered as two adjacent
// halves rather than a single square) -- same drawing/letter logic,
// just narrower.
struct OctBiasButton : SquareButton {
	OctBiasButton() {
		box.size = mm2px(Vec(5.5, 6.5));
	}
};

struct VFaderHandle : ParamWidget {
	float trackY0Px = 0.f, trackY1Px = 0.f;  // Y at value=1 (top), value=0 (bottom)
	float centerX = 0.f;
	float* displayValuePtr = nullptr;  // live Scene A/B blend, computed every frame by the module
	bool dragging = false;  // whether THIS fader is currently being dragged
	Module* mod = nullptr;
	int lightId = -1;  // step-position LED, embedded in the cap itself (see draw())

	VFaderHandle() {
		box.size = mm2px(Vec(8.0, 3.5));  // width trimmed toward a slimmer F8R-style nub (was 9.5mm) -- height unchanged, already halved per earlier request. Fader pitch itself is tightened separately in the layout script, not by this width change (see design notes: the two are independent).
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
		// Body: subtle top-to-bottom gradient for some dimensionality
		// instead of flat black, plus a thin grip-ridge highlight -- a
		// small aesthetic pass alongside embedding the LED below.
		NVGpaint bodyGrad = nvgLinearGradient(args.vg, 0, 0, 0, box.size.y,
			nvgRGBA(0x2C, 0x2C, 0x2E, 255), nvgRGBA(0x0A, 0x0A, 0x0C, 255));
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 1.2f);
		nvgFillPaint(args.vg, bodyGrad);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x2A, 0x2A, 0x2A));
		nvgStrokeWidth(args.vg, 0.9f);
		nvgStroke(args.vg);

		float cx = box.size.x / 2.f;
		float ledY = box.size.y * 0.32f;

		// Grip-ridge: thin highlight line, purely cosmetic.
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 1.2f, box.size.y * 0.62f);
		nvgLineTo(args.vg, box.size.x - 1.2f, box.size.y * 0.62f);
		nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 22));
		nvgStrokeWidth(args.vg, 0.6f);
		nvgStroke(args.vg);

		// Step-position LED, embedded directly in the cap -- replaces the
		// old separate row of red lights above the faders. Lit red when
		// this fader is the currently-playing step, a dim unlit dot
		// otherwise (so the LED window is always visible, on or off).
		// Dot/glow scaled down in step with the cap's own width trim
		// (9.5mm->8mm, factor ~0.84) so it doesn't clip against the
		// now-narrower cap edges -- a fixed-size glow was fine at the old
		// width but would look slightly cut off at this one.
		float brightness = (mod && lightId >= 0) ? mod->lights[lightId].getBrightness() : 0.f;
		NVGcolor ledColor = nvgRGBA(
			(unsigned char)(0x40 + (0xFF - 0x40) * brightness),
			(unsigned char)(0x12 + (0x28 - 0x12) * brightness),
			(unsigned char)(0x12 + (0x28 - 0x12) * brightness),
			255);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, ledY, 0.97f);
		nvgFillColor(args.vg, ledColor);
		nvgFill(args.vg);
		if (brightness > 0.05f) {
			// Clip the glow to the cap's own rounded-rect body -- the cap is
			// only 3.5mm tall (and now 8mm wide), so an unclipped glow would
			// bleed past its edges onto the track behind it.
			nvgSave(args.vg);
			nvgIntersectScissor(args.vg, 0.f, 0.f, box.size.x, box.size.y);
			NVGpaint glow = nvgRadialGradient(args.vg, cx, ledY, 0.25f, 1.68f,
				nvgRGBA(0xFF, 0x30, 0x30, (unsigned char)(160 * brightness)), nvgRGBA(0xFF, 0x30, 0x30, 0));
			nvgBeginPath(args.vg);
			nvgRect(args.vg, cx - 2.53f, ledY - 2.53f, 5.05f, 5.05f);
			nvgFillPaint(args.vg, glow);
			nvgFill(args.vg);
			nvgRestore(args.vg);
		}
	}
};

struct SpacesCommandWidget : ModuleWidget {
	SpacesCommandWidget(SpacesCommand* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SpacesCommand.svg")));

// I/O
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.4, 19.2)), module, SpacesCommand::CLOCK_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(35.53, 19.2)), module, SpacesCommand::RESET_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(51.65, 19.2)), module, SpacesCommand::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(67.77, 19.2)), module, SpacesCommand::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(83.9, 19.2)), module, SpacesCommand::VELOCITY_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(100.02, 19.2)), module, SpacesCommand::A_PITCH_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(116.14, 19.2)), module, SpacesCommand::A_GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(132.27, 19.2)), module, SpacesCommand::A_VEL_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(148.39, 19.2)), module, SpacesCommand::B_PITCH_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(164.51, 19.2)), module, SpacesCommand::B_GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(180.64, 19.2)), module, SpacesCommand::B_VEL_OUTPUT));

		// FEEL: macro knobs -- custom MorphKnob for live scene-blend display, matching original VST
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(21.0, 45.02)), module, SpacesCommand::REST_PARAM);
			if (module) k->displayValuePtr = &module->displayRest;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(35.27, 45.02)), module, SpacesCommand::LEGATO_PARAM);
			if (module) k->displayValuePtr = &module->displayLegato;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(49.54, 45.02)), module, SpacesCommand::RATE_PARAM);
			if (module) k->displayValuePtr = &module->displayRate;
			k->quantizeSteps = 201;  // 1 BPM per detent across the real 40-240 BPM range
			k->numTicks = 9;  // decorative reference marks (too many BPM steps to tick individually)
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(63.81, 45.02)), module, SpacesCommand::ENTROPY_PARAM);
			if (module) k->displayValuePtr = &module->displayEntropy;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(78.08, 45.02)), module, SpacesCommand::HARMONY_PARAM);
			if (module) k->displayValuePtr = &module->displayHarmony;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(92.35, 45.02)), module, SpacesCommand::CHAOS_PARAM);
			if (module) k->displayValuePtr = &module->displayChaos;
			addParam(k);
		}
		{
			auto* k = createParamCentered<MorphKnob>(mm2px(Vec(106.62, 45.02)), module, SpacesCommand::OCTAVES_PARAM);
			if (module) k->displayValuePtr = &module->displayOctaves;
			k->numTicks = 7;  // one tick per whole-octave position (-3..+3); snapping itself is engine-level (snapEnabled)
			addParam(k);
		}

		// CONTROLS: square toggle buttons, navy when engaged (real toggle now, not momentary-read)
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(138.12, 45.02)), module, SpacesCommand::LATCH_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::LATCH_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(148.6, 45.02)), module, SpacesCommand::ARPSEQ_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::ARPSEQ_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(159.08, 45.02)), module, SpacesCommand::STRUM_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::STRUM_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(169.56, 45.02)), module, SpacesCommand::FREEZE_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::FREEZE_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}
		{
			// ROUTING: BLEND/SPLIT toggle, now genuinely read in process()
			// (Split mode's per-stream A/B generation). Two states, one
			// bit of light brightness -- the old 3-state threshold bug
			// (brightness>0.5 only ever distinguished 2 of 3 states) is
			// moot now, there's nothing left to threshold.
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(180.04, 45.02)), module, SpacesCommand::ROUTING_PARAM);
			btn->mod = module;
			btn->lightId = SpacesCommand::ROUTING_LIGHT;
			btn->litColor = nvgRGB(0x2E, 0x4A, 0x6E);
			addParam(btn);
		}

		// SCENE: A/B focus (square, letter baked in, red glow when focused) + crossfader fader cap
		{
			auto* btnA = createParamCentered<SquareButton>(mm2px(Vec(20.0, 70.54)), module, SpacesCommand::SCENE_A_PARAM);
			btnA->mod = module;
			btnA->lightId = SpacesCommand::SCENE_A_LIGHT;
			btnA->litColor = nvgRGB(0xE0, 0x40, 0x40);
			btnA->letter = "A";
			addParam(btnA);
		}
		{
			auto* btnB = createParamCentered<SquareButton>(mm2px(Vec(111.24, 70.54)), module, SpacesCommand::SCENE_B_PARAM);
			btnB->mod = module;
			btnB->lightId = SpacesCommand::SCENE_B_LIGHT;
			btnB->litColor = nvgRGB(0xE0, 0x40, 0x40);
			btnB->letter = "B";
			addParam(btnB);
		}
		{
			auto* xfHandle = createParamCentered<HCrossfaderHandle>(mm2px(Vec((30.75+100.49)/2.f, 70.54)), module, SpacesCommand::MORPH_PARAM);
			// Inset by half the cap's own width (9.0mm / 2 = 4.5mm) at each
			// end, same fix as the vertical faders -- otherwise the cap
			// overshoots the drawn track by half its own size at the
			// extremes, since its position is centered on these bounds.
			xfHandle->trackX0Px = mm2px(Vec(35.25, 0)).x;
			xfHandle->trackX1Px = mm2px(Vec(95.99, 0)).x;
			xfHandle->centerY = mm2px(Vec(0, 70.54)).y;
			addParam(xfHandle);
		}

		// KEY: genuinely smaller stock component (Trimpot)
		addParam(createParamCentered<Trimpot>(mm2px(Vec(140.74, 69.04)), module, SpacesCommand::ROOT_KEY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(154.18, 69.04)), module, SpacesCommand::SCALE_TYPE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(167.61, 69.04)), module, SpacesCommand::DENSITY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(181.04, 69.04)), module, SpacesCommand::SWING_PARAM));

		// PATTERN: custom vertical faders (live scene-morph display), each
		// with its step-position LED embedded directly in the cap (see
		// VFaderHandle::draw) instead of a separate row of lights above.
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(23.0, 102.24)), module, SpacesCommand::FADER_PARAM + 0);
			fader->trackY0Px = mm2px(Vec(0, 90.49)).y;  // inset by half the cap's own height (1.75mm) so the cap stays fully inside the drawn track at max value
			fader->trackY1Px = mm2px(Vec(0, 113.99)).y;  // same inset at the bottom, min value
			fader->centerX = mm2px(Vec(23.0, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[0];
			fader->mod = module;
			fader->lightId = SpacesCommand::STEP_LIGHTS + 0;
			addParam(fader);
		}
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(36.72, 102.24)), module, SpacesCommand::FADER_PARAM + 1);
			fader->trackY0Px = mm2px(Vec(0, 90.49)).y;  // inset by half the cap's own height (1.75mm) so the cap stays fully inside the drawn track at max value
			fader->trackY1Px = mm2px(Vec(0, 113.99)).y;  // same inset at the bottom, min value
			fader->centerX = mm2px(Vec(36.72, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[1];
			fader->mod = module;
			fader->lightId = SpacesCommand::STEP_LIGHTS + 1;
			addParam(fader);
		}
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(50.44, 102.24)), module, SpacesCommand::FADER_PARAM + 2);
			fader->trackY0Px = mm2px(Vec(0, 90.49)).y;  // inset by half the cap's own height (1.75mm) so the cap stays fully inside the drawn track at max value
			fader->trackY1Px = mm2px(Vec(0, 113.99)).y;  // same inset at the bottom, min value
			fader->centerX = mm2px(Vec(50.44, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[2];
			fader->mod = module;
			fader->lightId = SpacesCommand::STEP_LIGHTS + 2;
			addParam(fader);
		}
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(64.15, 102.24)), module, SpacesCommand::FADER_PARAM + 3);
			fader->trackY0Px = mm2px(Vec(0, 90.49)).y;  // inset by half the cap's own height (1.75mm) so the cap stays fully inside the drawn track at max value
			fader->trackY1Px = mm2px(Vec(0, 113.99)).y;  // same inset at the bottom, min value
			fader->centerX = mm2px(Vec(64.15, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[3];
			fader->mod = module;
			fader->lightId = SpacesCommand::STEP_LIGHTS + 3;
			addParam(fader);
		}
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(77.87, 102.24)), module, SpacesCommand::FADER_PARAM + 4);
			fader->trackY0Px = mm2px(Vec(0, 90.49)).y;  // inset by half the cap's own height (1.75mm) so the cap stays fully inside the drawn track at max value
			fader->trackY1Px = mm2px(Vec(0, 113.99)).y;  // same inset at the bottom, min value
			fader->centerX = mm2px(Vec(77.87, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[4];
			fader->mod = module;
			fader->lightId = SpacesCommand::STEP_LIGHTS + 4;
			addParam(fader);
		}
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(91.59, 102.24)), module, SpacesCommand::FADER_PARAM + 5);
			fader->trackY0Px = mm2px(Vec(0, 90.49)).y;  // inset by half the cap's own height (1.75mm) so the cap stays fully inside the drawn track at max value
			fader->trackY1Px = mm2px(Vec(0, 113.99)).y;  // same inset at the bottom, min value
			fader->centerX = mm2px(Vec(91.59, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[5];
			fader->mod = module;
			fader->lightId = SpacesCommand::STEP_LIGHTS + 5;
			addParam(fader);
		}
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(105.31, 102.24)), module, SpacesCommand::FADER_PARAM + 6);
			fader->trackY0Px = mm2px(Vec(0, 90.49)).y;  // inset by half the cap's own height (1.75mm) so the cap stays fully inside the drawn track at max value
			fader->trackY1Px = mm2px(Vec(0, 113.99)).y;  // same inset at the bottom, min value
			fader->centerX = mm2px(Vec(105.31, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[6];
			fader->mod = module;
			fader->lightId = SpacesCommand::STEP_LIGHTS + 6;
			addParam(fader);
		}
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(119.03, 102.24)), module, SpacesCommand::FADER_PARAM + 7);
			fader->trackY0Px = mm2px(Vec(0, 90.49)).y;  // inset by half the cap's own height (1.75mm) so the cap stays fully inside the drawn track at max value
			fader->trackY1Px = mm2px(Vec(0, 113.99)).y;  // same inset at the bottom, min value
			fader->centerX = mm2px(Vec(119.03, 0)).x;
			if (module) fader->displayValuePtr = &module->displayFaderValue[7];
			fader->mod = module;
			fader->lightId = SpacesCommand::STEP_LIGHTS + 7;
			addParam(fader);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(126.02, 103.84)), module, SpacesCommand::MELO_PARAM);
			btn->unlitColor = nvgRGB(0x6A, 0x42, 0x08);  // amber family -- matches the 8 pattern faders it randomizes
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(139.02, 103.84)), module, SpacesCommand::DICE_ARTI);
			btn->unlitColor = nvgRGB(0x5A, 0x1E, 0x1E);  // maroon family -- matches REST+LEGATO knob rings
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(152.02, 103.84)), module, SpacesCommand::DICE_TIME);
			btn->unlitColor = nvgRGB(0x5A, 0x40, 0x18);  // brass/ochre family -- matches RATE+OCTAVES knob rings
			addParam(btn);
		}
		{
			auto* btn = createParamCentered<SquareButton>(mm2px(Vec(165.02, 103.84)), module, SpacesCommand::DICE_NAVY);
			btn->unlitColor = nvgRGB(0x1E, 0x30, 0x48);  // navy family -- matches ENTROPY+HARMONY+CHAOS knob rings
			addParam(btn);
		}
		{
			// OCT: one column-slot, split left/right into two half-width
			// buttons -- matches keyboard octave-switch convention (down
			// on the left, up on the right), not a circular dice bezel.
			// Sets the UNFOCUSED scene's octave relative to the focused
			// one (magnitude a surprise 0-3 octaves) -- see OCT_BIAS_DOWN/
			// UP_PARAM and applyOctaveBias() in the module.
			auto* down = createParamCentered<OctBiasButton>(mm2px(Vec(174.77, 103.84)), module, SpacesCommand::OCT_BIAS_DOWN_PARAM);
			down->unlitColor = nvgRGB(0x5A, 0x40, 0x18);  // brass family -- same as TIME, since it shares OCTAVES' identity
			down->letter = "-";
			addParam(down);
			auto* up = createParamCentered<OctBiasButton>(mm2px(Vec(181.27, 103.84)), module, SpacesCommand::OCT_BIAS_UP_PARAM);
			up->unlitColor = nvgRGB(0x5A, 0x40, 0x18);
			up->letter = "+";
			addParam(up);
		}

	}
};

Model* modelSpacesCommand = createModel<SpacesCommand, SpacesCommandWidget>("SpacesCommand");
