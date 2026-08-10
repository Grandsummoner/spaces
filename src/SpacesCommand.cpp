#include "plugin.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

// =====================================================================
// SYNTH VOICE — ported from the original Navy Arp 2 (JUCE) SynthVoice.
// Waveform layers (Analog/FM/Supersaw/Pulse) are non-exclusive: any
// combination can be active at once. Per explicit decision, active
// layers are PLAIN-AVERAGED (sum / count), not loudness-compensated
// (sum / sqrt(count)) -- stacking layers keeps total loudness roughly
// constant rather than getting louder as more are added.
// =====================================================================
struct SynthVoice {
	float sampleRate = 44100.f;
	float phase = 0.f;
	float phaseIncrement = 0.f;

	float fmModPhase = 0.f;
	float sawPhases[7] = {};
	float sawPhaseIncrements[7] = {};

	float s1 = 0.f, sSupersaw = 0.f;

	float attack = 0.01f, decay = 0.35f, sustain = 0.70f, release = 0.25f;

	enum class EnvState { Idle, Attack, Decay, Sustain, Release };
	EnvState envState = EnvState::Idle;
	float envVal = 0.f;
	float releaseLevel = 0.f;
	double stateTime = 0.0;
	int activeNoteCount = 0;

	void triggerNote(float pitchVolt) {
		float freq = dsp::FREQ_C4 * std::pow(2.f, pitchVolt);
		phaseIncrement = freq / sampleRate;
		float detunes[7] = { -0.06f, -0.04f, -0.015f, 0.f, 0.015f, 0.04f, 0.06f };
		for (int i = 0; i < 7; i++)
			sawPhaseIncrements[i] = freq * std::pow(2.f, detunes[i] / 12.f) / sampleRate;
		if (activeNoteCount == 0) { envState = EnvState::Attack; stateTime = 0.0; envVal = 0.f; }
		activeNoteCount++;
	}

	void releaseNote() {
		if (activeNoteCount > 0) activeNoteCount--;
		if (activeNoteCount == 0 && envState != EnvState::Idle) {
			envState = EnvState::Release; releaseLevel = envVal; stateTime = 0.0;
		}
	}

	float process(bool an, bool fm, bool ss, bool pl, float timbre) {
		double dt = 1.0 / sampleRate;
		stateTime += dt;
		switch (envState) {
			case EnvState::Idle: envVal = 0.f; break;
			case EnvState::Attack: {
				float dur = std::max(0.001f, attack);
				envVal = (float)(stateTime / dur);
				if (envVal >= 1.f) { envVal = 1.f; envState = EnvState::Decay; stateTime = 0.0; }
				break;
			}
			case EnvState::Decay: {
				float dur = std::max(0.001f, decay);
				float p = (float)(stateTime / dur);
				if (p >= 1.f) { envVal = sustain; envState = EnvState::Sustain; stateTime = 0.0; }
				else envVal = 1.f - (1.f - sustain) * p;
				break;
			}
			case EnvState::Sustain: envVal = sustain; break;
			case EnvState::Release: {
				float dur = std::max(0.001f, release);
				float p = (float)(stateTime / dur);
				if (p >= 1.f) { envVal = 0.f; envState = EnvState::Idle; }
				else envVal = releaseLevel * (1.f - p);
				break;
			}
		}
		if (envVal <= 0.0001f) { envVal = 0.f; return 0.f; }

		float total = 0.f;
		int count = 0;
		bool advancePhase = an || fm || pl;
		float cutoffHz = 60.f + (timbre * 7500.f) * envVal;
		float wd = 2.f * (float)M_PI * cutoffHz / sampleRate;
		float g = std::tan(wd * 0.5f);
		float h = g / (1.f + g);

		if (an) {
			float wave = 2.f * phase - 1.f;
			float out = (wave - s1 * 0.45f) * h + s1;
			s1 = out; total += out; count++;
		}
		if (fm) {
			fmModPhase += phaseIncrement * 3.5f;
			if (fmModPhase >= 1.f) fmModPhase -= 1.f;
			float mod = std::sin(fmModPhase * 2.f * (float)M_PI);
			float carrierPhase = phase + mod * (timbre * 6.5f) * phaseIncrement;
			float carrier = std::sin(carrierPhase * 2.f * (float)M_PI);
			float out = (carrier - s1 * 0.35f) * h + s1;
			s1 = out; total += out; count++;
		}
		if (ss) {
			float sum = 0.f;
			for (int i = 0; i < 7; i++) {
				sum += 2.f * sawPhases[i] - 1.f;
				sawPhases[i] += sawPhaseIncrements[i];
				if (sawPhases[i] >= 1.f) sawPhases[i] -= 1.f;
			}
			float raw = sum * 0.35f;
			float out = (raw - sSupersaw * 0.38f) * h + sSupersaw;
			sSupersaw = out; total += out; count++;
		}
		if (pl) {
			float width = 0.15f + timbre * 0.7f;
			float wave = (phase < width) ? 0.4f : -0.4f;
			float out = (wave - s1 * 0.40f) * h + s1;
			s1 = out; total += out * 0.6f; count++;
		}
		if (advancePhase) { phase += phaseIncrement; if (phase >= 1.f) phase -= 1.f; }

		// plain average across active layers (not sqrt-normalized) -- per explicit decision
		if (count > 1) total /= (float)count;
		return total * envVal;
	}
};

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
		VOICE1_WAVE_AN, VOICE1_WAVE_FM, VOICE1_WAVE_SS, VOICE1_WAVE_PL,
		VOICE1_ATTACK_PARAM, VOICE1_DECAY_PARAM, VOICE1_SUSTAIN_PARAM, VOICE1_RELEASE_PARAM, VOICE1_TIMBRE_PARAM,
		VOICE2_WAVE_AN, VOICE2_WAVE_FM, VOICE2_WAVE_SS, VOICE2_WAVE_PL,
		VOICE2_ATTACK_PARAM, VOICE2_DECAY_PARAM, VOICE2_SUSTAIN_PARAM, VOICE2_RELEASE_PARAM, VOICE2_TIMBRE_PARAM,
		PARAMS_LEN
	};
	enum InputId { VOCT_INPUT, GATE_INPUT, VELOCITY_INPUT, CLOCK_INPUT, INPUTS_LEN };
	enum OutputId { VOICE1_OUTPUT, VOICE2_OUTPUT, MASTER_L_OUTPUT, MASTER_R_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		ENUMS(STEP_LIGHTS, 8), SCENE_A_LIGHT, SCENE_B_LIGHT,
		ENUMS(VOICE1_WAVE_AN_LIGHT, 4), ENUMS(VOICE2_WAVE_AN_LIGHT, 4),
		LATCH_LIGHT, ARPSEQ_LIGHT, POLY_LIGHT, FREEZE_LIGHT, ROUTING_LIGHT,
		LIGHTS_LEN
	};

	SceneState sceneA, sceneB;
	bool focusB = false;
	int currentStep = 0;
	bool goingForward = true;
	double phaseAccumSamples = 0.0;

	std::vector<int> heldNotes, latchedNotes;
	SynthVoice voice1, voice2;
	dsp::PulseGenerator gatePulse;
	dsp::SchmittTrigger sceneATrig, sceneBTrig, clockTrig;
	dsp::SchmittTrigger diceArtiTrig, diceTimeTrig, diceNavyTrig, meloTrig;
	dsp::SchmittTrigger meloNudgeDownTrig, meloNudgeUpTrig, artiNudgeDownTrig, artiNudgeUpTrig;
	dsp::SchmittTrigger timeNudgeDownTrig, timeNudgeUpTrig, navyNudgeDownTrig, navyNudgeUpTrig;

	// Waveform buttons are momentary presses (TL1105) that TOGGLE persisted
	// state, enforced to always have >=1 active per voice -- fixes the bug
	// where sound only played while the button was physically held.
	bool v1WaveOn[4] = {true, false, false, false};  // AN, FM, SS, PL
	bool v2WaveOn[4] = {false, false, true, false};
	dsp::SchmittTrigger v1WaveTrig[4], v2WaveTrig[4];

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
		configSwitch(ROUTING_PARAM, 0.f, 1.f, 0.f, "Voice routing", {"Together", "Split A\u00b7B"});
		configParam(REST_PARAM, 0.f, 1.f, 0.1f, "Rest probability", "%", 0, 100);
		configButton(DICE_ARTI, "Randomize Rest+Legato (ARTI)");
		configButton(ARTI_NUDGE_DOWN, "Nudge Rest+Legato down");
		configButton(ARTI_NUDGE_UP, "Nudge Rest+Legato up");
		configParam(LEGATO_PARAM, 0.f, 1.f, 0.5f, "Legato", "%", 0, 100);
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate", " BPM", 0, 200, 40);
		configButton(DICE_TIME, "Randomize Rate+Octaves (TIME)");
		configButton(TIME_NUDGE_DOWN, "Nudge Rate+Octaves down");
		configButton(TIME_NUDGE_UP, "Nudge Rate+Octaves up");
		configParam(ENTROPY_PARAM, -1.f, 1.f, 0.f, "Entropy (play direction)");
		configParam(HARMONY_PARAM, 0.f, 1.f, 0.f, "Harmony");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.f, "Chaos");
		configButton(DICE_NAVY, "Randomize Entropy+Harmony+Chaos (NAVY)");
		configButton(NAVY_NUDGE_DOWN, "Nudge Entropy+Harmony+Chaos down");
		configButton(NAVY_NUDGE_UP, "Nudge Entropy+Harmony+Chaos up");
		configParam(OCTAVES_PARAM, -3.f, 3.f, 0.f, "Octave shift");
		configParam(ROOT_KEY_PARAM, 0.f, 11.f, 0.f, "Root key");
		getParamQuantity(ROOT_KEY_PARAM)->snapEnabled = true;
		configParam(SCALE_TYPE_PARAM, 0.f, 9.f, 0.f, "Scale");
		getParamQuantity(SCALE_TYPE_PARAM)->snapEnabled = true;
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.5f, "Density", "%", 0, 100);
		configParam(SWING_PARAM, 0.f, 1.f, 0.f, "Swing", "%", 0, 100);

		configButton(VOICE1_WAVE_AN, "Voice 1: Analog");
		configButton(VOICE1_WAVE_FM, "Voice 1: FM");
		configButton(VOICE1_WAVE_SS, "Voice 1: Supersaw");
		configButton(VOICE1_WAVE_PL, "Voice 1: Pulse");
		configParam(VOICE1_ATTACK_PARAM, 0.001f, 2.f, 0.01f, "Voice 1 attack", " s");
		configParam(VOICE1_DECAY_PARAM, 0.001f, 2.f, 0.35f, "Voice 1 decay", " s");
		configParam(VOICE1_SUSTAIN_PARAM, 0.f, 1.f, 0.70f, "Voice 1 sustain", "%", 0, 100);
		configParam(VOICE1_RELEASE_PARAM, 0.001f, 4.f, 0.25f, "Voice 1 release", " s");
		configParam(VOICE1_TIMBRE_PARAM, 0.f, 1.f, 0.5f, "Voice 1 timbre");

		configButton(VOICE2_WAVE_AN, "Voice 2: Analog");
		configButton(VOICE2_WAVE_FM, "Voice 2: FM");
		configButton(VOICE2_WAVE_SS, "Voice 2: Supersaw");
		configButton(VOICE2_WAVE_PL, "Voice 2: Pulse");
		configParam(VOICE2_ATTACK_PARAM, 0.001f, 2.f, 0.01f, "Voice 2 attack", " s");
		configParam(VOICE2_DECAY_PARAM, 0.001f, 2.f, 0.35f, "Voice 2 decay", " s");
		configParam(VOICE2_SUSTAIN_PARAM, 0.f, 1.f, 0.70f, "Voice 2 sustain", "%", 0, 100);
		configParam(VOICE2_RELEASE_PARAM, 0.001f, 4.f, 0.25f, "Voice 2 release", " s");
		configParam(VOICE2_TIMBRE_PARAM, 0.f, 1.f, 0.5f, "Voice 2 timbre");

		configInput(VOCT_INPUT, "1V/oct pitch (poly, held notes)");
		configInput(GATE_INPUT, "Gate (poly, held notes)");
		configInput(VELOCITY_INPUT, "Velocity (poly)");
		configInput(CLOCK_INPUT, "Clock (patched = external run/stop; unpatched = free-run on RATE)");
		configOutput(VOICE1_OUTPUT, "Voice 1");
		configOutput(VOICE2_OUTPUT, "Voice 2");
		configOutput(MASTER_L_OUTPUT, "Master L");
		configOutput(MASTER_R_OUTPUT, "Master R");
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
		json_t* v1J = json_array();
		json_t* v2J = json_array();
		for (int i = 0; i < 4; i++) {
			json_array_append_new(v1J, json_boolean(v1WaveOn[i]));
			json_array_append_new(v2J, json_boolean(v2WaveOn[i]));
		}
		json_object_set_new(rootJ, "v1WaveOn", v1J);
		json_object_set_new(rootJ, "v2WaveOn", v2J);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* v1J = json_object_get(rootJ, "v1WaveOn");
		json_t* v2J = json_object_get(rootJ, "v2WaveOn");
		if (v1J) for (int i = 0; i < 4 && i < (int)json_array_size(v1J); i++)
			v1WaveOn[i] = json_boolean_value(json_array_get(v1J, i));
		if (v2J) for (int i = 0; i < 4 && i < (int)json_array_size(v2J); i++)
			v2WaveOn[i] = json_boolean_value(json_array_get(v2J, i));
	}

	void process(const ProcessArgs& args) override {
		voice1.sampleRate = args.sampleRate;
		voice2.sampleRate = args.sampleRate;

		if (sceneATrig.process(params[SCENE_A_PARAM].getValue())) focusB = false;
		if (sceneBTrig.process(params[SCENE_B_PARAM].getValue())) focusB = true;
		lights[SCENE_A_LIGHT].setBrightness(!focusB);
		lights[SCENE_B_LIGHT].setBrightness(focusB);

		// Mode toggle LEDs: green when on, unlit when off
		lights[LATCH_LIGHT].setBrightness(params[LATCH_PARAM].getValue() > 0.5f ? 1.f : 0.f);
		lights[ARPSEQ_LIGHT].setBrightness(params[ARPSEQ_PARAM].getValue() > 0.5f ? 1.f : 0.f);
		lights[POLY_LIGHT].setBrightness(params[POLY_PARAM].getValue() > 0.5f ? 1.f : 0.f);
		lights[FREEZE_LIGHT].setBrightness(params[FREEZE_PARAM].getValue() > 0.5f ? 1.f : 0.f);
		lights[ROUTING_LIGHT].setBrightness(params[ROUTING_PARAM].getValue() > 0.5f ? 1.f : 0.f);

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
		float rest = crossfade(sceneA.rest, sceneB.rest, morph);
		float rate01 = crossfade(sceneA.rate, sceneB.rate, morph);
		float entropy = crossfade(sceneA.entropy, sceneB.entropy, morph);
		float octavesF = crossfade(sceneA.octaves, sceneB.octaves, morph);
		int octaveShift = (int)std::round(octavesF);

		heldNotes.clear();
		int channels = std::max(inputs[VOCT_INPUT].getChannels(), 1);
		for (int c = 0; c < channels; c++) {
			if (inputs[GATE_INPUT].getVoltage(c) >= 1.f) {
				int pitch = 60 + (int)std::round(inputs[VOCT_INPUT].getVoltage(c) * 12.f);
				heldNotes.push_back(pitch);
			}
		}
		bool latchOn = params[LATCH_PARAM].getValue() > 0.5f;
		bool freezeOn = params[FREEZE_PARAM].getValue() > 0.5f;
		if (!heldNotes.empty() && latchOn) latchedNotes = heldNotes;
		std::vector<int>& notesToPlay = latchOn ? latchedNotes : heldNotes;

		// Clock-presence run logic: patched CLOCK drives steps on its rising
		// edges (patching/unpatching IS start/stop); unpatched free-runs off
		// RATE, gated by held notes / FREEZE, same as before.
		bool clockPatched = inputs[CLOCK_INPUT].isConnected();
		bool stepTriggered = false;

		if (clockPatched) {
			stepTriggered = clockTrig.process(inputs[CLOCK_INPUT].getVoltage());
		} else {
			bool playing = !notesToPlay.empty() || freezeOn;
			double bpm = 40.0 + rate01 * 200.0;
			double stepSamples = args.sampleRate * (60.0 / std::max(1.0, bpm)) * 0.25;
			if (playing) {
				phaseAccumSamples += 1.0;
				if (phaseAccumSamples >= stepSamples) { phaseAccumSamples = 0.0; stepTriggered = true; }
			}
		}

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
				if (!notesToPlay.empty())
					pitch = notesToPlay[localStep % notesToPlay.size()] + 12 * octaveShift;
				else
					pitch = 48 + rootKeyIdx + scaleOffsets[localStep % (int)scaleOffsets.size()] + 12 * octaveShift;

				float pitchVolt = (pitch - 60) / 12.f;
				gatePulse.trigger(1e-3f);

				voice1.attack = params[VOICE1_ATTACK_PARAM].getValue();
				voice1.decay = params[VOICE1_DECAY_PARAM].getValue();
				voice1.sustain = params[VOICE1_SUSTAIN_PARAM].getValue();
				voice1.release = params[VOICE1_RELEASE_PARAM].getValue();
				voice2.attack = params[VOICE2_ATTACK_PARAM].getValue();
				voice2.decay = params[VOICE2_DECAY_PARAM].getValue();
				voice2.sustain = params[VOICE2_SUSTAIN_PARAM].getValue();
				voice2.release = params[VOICE2_RELEASE_PARAM].getValue();

				bool split = params[ROUTING_PARAM].getValue() > 0.5f;
				if (!split) {
					// TOGETHER: both voices always fire on every note
					voice1.triggerNote(pitchVolt);
					voice2.triggerNote(pitchVolt);
				} else {
					// SPLIT A.B: Voice 1 plays Scene-A-weighted steps, Voice 2
					// plays Scene-B-weighted steps, chosen by which scene the
					// morph currently favors -- lets the two voices diverge
					// into distinct instruments as you move the crossfader.
					if (morph < 0.5f) voice1.triggerNote(pitchVolt);
					else voice2.triggerNote(pitchVolt);
				}
			}
			for (int i = 0; i < 8; i++)
				lights[STEP_LIGHTS + i].setBrightness(i == localStep ? 1.f : 0.f);
		}

		// Waveform buttons: momentary press toggles persisted on/off state,
		// but never allow the last active layer to be turned off (voice
		// must always have >=1 waveform selected -- mix-and-match is fine,
		// silence is not).
		auto handleWaveToggle = [&](bool wasOn[4], dsp::SchmittTrigger trig[4], int paramBase, int lightBase) {
			for (int i = 0; i < 4; i++) {
				if (trig[i].process(params[paramBase + i].getValue())) {
					int activeCount = 0;
					for (int j = 0; j < 4; j++) if (wasOn[j]) activeCount++;
					if (wasOn[i] && activeCount <= 1) {
						// would turn off the last active layer -- ignore
					} else {
						wasOn[i] = !wasOn[i];
					}
				}
				lights[lightBase + i].setBrightness(wasOn[i] ? 1.f : 0.f);
			}
		};
		handleWaveToggle(v1WaveOn, v1WaveTrig, VOICE1_WAVE_AN, VOICE1_WAVE_AN_LIGHT);
		handleWaveToggle(v2WaveOn, v2WaveTrig, VOICE2_WAVE_AN, VOICE2_WAVE_AN_LIGHT);

		bool v1an = v1WaveOn[0], v1fm = v1WaveOn[1], v1ss = v1WaveOn[2], v1pl = v1WaveOn[3];
		bool v2an = v2WaveOn[0], v2fm = v2WaveOn[1], v2ss = v2WaveOn[2], v2pl = v2WaveOn[3];

		float v1out = voice1.process(v1an, v1fm, v1ss, v1pl, params[VOICE1_TIMBRE_PARAM].getValue());
		float v2out = voice2.process(v2an, v2fm, v2ss, v2pl, params[VOICE2_TIMBRE_PARAM].getValue());

		outputs[VOICE1_OUTPUT].setVoltage(v1out * 5.f);
		outputs[VOICE2_OUTPUT].setVoltage(v2out * 5.f);
		outputs[MASTER_L_OUTPUT].setVoltage((v1out * 0.7f + v2out * 0.3f) * 5.f);
		outputs[MASTER_R_OUTPUT].setVoltage((v1out * 0.3f + v2out * 0.7f) * 5.f);
	}
};

// =====================================================================
// Custom horizontal crossfader handle -- a real fader CAP that slides
// along the SCENE track, per explicit request that it must not be a
// knob. Drag horizontally; position maps directly to MORPH_PARAM.
// Fill color itself blends amber(A)->cyan(B) with position, so the
// handle visually shows how far toward each scene it's leaning.
// =====================================================================
struct HCrossfaderHandle : ParamWidget {
	float trackX0Px = 0.f, trackX1Px = 0.f;
	float centerY = 0.f;

	HCrossfaderHandle() {
		box.size = mm2px(Vec(9.0, 8.4));  // wider cap per explicit request
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
		// blue/black cap per explicit request (was amber/cyan gradient)
		NVGcolor body = nvgRGB(0x16, 0x18, 0x1E);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, body);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x2A, 0x4A, 0x7A));
		nvgStrokeWidth(args.vg, 1.2f);
		nvgStroke(args.vg);
		// grip notches
		for (int i = -1; i <= 1; i++) {
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, box.size.x/2 + i*2.2f, box.size.y*0.22f);
			nvgLineTo(args.vg, box.size.x/2 + i*2.2f, box.size.y*0.78f);
			nvgStrokeColor(args.vg, nvgRGBA(0x4A, 0x6A, 0x9A, 180));
			nvgStrokeWidth(args.vg, 0.6f);
			nvgStroke(args.vg);
		}
		// embedded LED, glowing blue
		float cx = box.size.x/2, cy = box.size.y*0.5f;
		NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, 0.5f, 3.5f, nvgRGBA(0x4A, 0x9A, 0xFF, 220), nvgRGBA(0x4A, 0x9A, 0xFF, 0));
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

struct VFaderHandle : ParamWidget {
	float trackY0Px = 0.f, trackY1Px = 0.f;  // Y at value=1 (top), value=0 (bottom)
	float centerX = 0.f;

	VFaderHandle() {
		box.size = mm2px(Vec(9.5, 7.0));
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
		float trackLenPx = std::max(1.f, trackY1Px - trackY0Px);
		// dragging UP (negative mouseDelta.y) should INCREASE the value
		float delta = -e.mouseDelta.y / zoom / trackLenPx;
		pq->setScaledValue(clamp((float)pq->getScaledValue() + delta, 0.f, 1.f));
	}

	void step() override {
		ParamWidget::step();
		ParamQuantity* pq = getParamQuantity();
		float v = pq ? (float)pq->getScaledValue() : 0.f;
		float centerYPx = trackY1Px + (trackY0Px - trackY1Px) * v;
		box.pos.x = centerX - box.size.x / 2.f;
		box.pos.y = centerYPx - box.size.y / 2.f;
	}

	void draw(const DrawArgs& args) override {
		NVGcolor body = nvgRGB(0x16, 0x18, 0x1E);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 1.8f);
		nvgFillColor(args.vg, body);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x3A, 0x6A, 0x3A));
		nvgStrokeWidth(args.vg, 1.1f);
		nvgStroke(args.vg);
		for (int i = -1; i <= 1; i++) {
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, box.size.x*0.2f, box.size.y/2 + i*1.8f);
			nvgLineTo(args.vg, box.size.x*0.8f, box.size.y/2 + i*1.8f);
			nvgStrokeColor(args.vg, nvgRGBA(0x4A, 0x9A, 0x4A, 160));
			nvgStrokeWidth(args.vg, 0.6f);
			nvgStroke(args.vg);
		}
		float cx = box.size.x/2, cy = box.size.y/2;
		NVGpaint glow = nvgRadialGradient(args.vg, cx, cy, 0.5f, 3.5f, nvgRGBA(0x4A, 0xFF, 0x6A, 200), nvgRGBA(0x4A, 0xFF, 0x6A, 0));
		nvgBeginPath(args.vg);
		nvgRect(args.vg, cx-4, cy-4, 8, 8);
		nvgFillPaint(args.vg, glow);
		nvgFill(args.vg);
		nvgBeginPath(args.vg);
		nvgCircle(args.vg, cx, cy, 1.1f);
		nvgFillColor(args.vg, nvgRGB(0xC0, 0xFF, 0xC8));
		nvgFill(args.vg);
	}
};

struct SpacesCommandWidget : ModuleWidget {
	SpacesCommandWidget(SpacesCommand* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SpacesCommand.svg")));

// I/O
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.4, 19.2)), module, SpacesCommand::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(54.05, 19.2)), module, SpacesCommand::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(88.69, 19.2)), module, SpacesCommand::VELOCITY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(123.34, 19.2)), module, SpacesCommand::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(157.98, 19.2)), module, SpacesCommand::VOICE1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(192.63, 19.2)), module, SpacesCommand::VOICE2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(227.27, 19.2)), module, SpacesCommand::MASTER_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(261.92, 19.2)), module, SpacesCommand::MASTER_R_OUTPUT));

		// FEEL: macro knobs, 85%-scaled
		addParam(createParamCentered<SmallKnob85>(mm2px(Vec(21.0, 38.02)), module, SpacesCommand::REST_PARAM));
		addParam(createParamCentered<SmallKnob85>(mm2px(Vec(43.4, 38.02)), module, SpacesCommand::LEGATO_PARAM));
		addParam(createParamCentered<SmallKnob85>(mm2px(Vec(65.8, 38.02)), module, SpacesCommand::RATE_PARAM));
		addParam(createParamCentered<SmallKnob85>(mm2px(Vec(88.2, 38.02)), module, SpacesCommand::ENTROPY_PARAM));
		addParam(createParamCentered<SmallKnob85>(mm2px(Vec(110.59, 38.02)), module, SpacesCommand::HARMONY_PARAM));
		addParam(createParamCentered<SmallKnob85>(mm2px(Vec(132.99, 38.02)), module, SpacesCommand::CHAOS_PARAM));
		addParam(createParamCentered<SmallKnob85>(mm2px(Vec(155.39, 38.02)), module, SpacesCommand::OCTAVES_PARAM));

		// CONTROLS: mode toggles, square, green on/off
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(179.89, 38.02)), module, SpacesCommand::LATCH_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(179.89, 38.02)), module, SpacesCommand::LATCH_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(200.25, 38.02)), module, SpacesCommand::ARPSEQ_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(200.25, 38.02)), module, SpacesCommand::ARPSEQ_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(220.61, 38.02)), module, SpacesCommand::POLY_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(220.61, 38.02)), module, SpacesCommand::POLY_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(240.96, 38.02)), module, SpacesCommand::FREEZE_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(240.96, 38.02)), module, SpacesCommand::FREEZE_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(261.32, 38.02)), module, SpacesCommand::ROUTING_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(261.32, 38.02)), module, SpacesCommand::ROUTING_LIGHT));

		// VOICE1: square wave buttons + 85%-scaled knobs
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(18.82, 56.76)), module, SpacesCommand::VOICE1_WAVE_AN));
		addChild(createLightCentered<LEDBezelLight<RedLight>>(mm2px(Vec(18.82, 56.76)), module, SpacesCommand::VOICE1_WAVE_AN_LIGHT + 0));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(32.73, 56.76)), module, SpacesCommand::VOICE1_WAVE_FM));
		addChild(createLightCentered<LEDBezelLight<YellowLight>>(mm2px(Vec(32.73, 56.76)), module, SpacesCommand::VOICE1_WAVE_AN_LIGHT + 1));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(46.64, 56.76)), module, SpacesCommand::VOICE1_WAVE_SS));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(46.64, 56.76)), module, SpacesCommand::VOICE1_WAVE_AN_LIGHT + 2));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(60.55, 56.76)), module, SpacesCommand::VOICE1_WAVE_PL));
		addChild(createLightCentered<LEDBezelLight<BlueLight>>(mm2px(Vec(60.55, 56.76)), module, SpacesCommand::VOICE1_WAVE_AN_LIGHT + 3));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(74.45, 56.76)), module, SpacesCommand::VOICE1_ATTACK_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(88.36, 56.76)), module, SpacesCommand::VOICE1_DECAY_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(102.27, 56.76)), module, SpacesCommand::VOICE1_SUSTAIN_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(116.18, 56.76)), module, SpacesCommand::VOICE1_RELEASE_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(130.09, 56.76)), module, SpacesCommand::VOICE1_TIMBRE_PARAM));

		// VOICE2: square wave buttons + 85%-scaled knobs
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(151.23, 56.76)), module, SpacesCommand::VOICE2_WAVE_AN));
		addChild(createLightCentered<LEDBezelLight<RedLight>>(mm2px(Vec(151.23, 56.76)), module, SpacesCommand::VOICE2_WAVE_AN_LIGHT + 0));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(165.14, 56.76)), module, SpacesCommand::VOICE2_WAVE_FM));
		addChild(createLightCentered<LEDBezelLight<YellowLight>>(mm2px(Vec(165.14, 56.76)), module, SpacesCommand::VOICE2_WAVE_AN_LIGHT + 1));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(179.05, 56.76)), module, SpacesCommand::VOICE2_WAVE_SS));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(179.05, 56.76)), module, SpacesCommand::VOICE2_WAVE_AN_LIGHT + 2));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(192.96, 56.76)), module, SpacesCommand::VOICE2_WAVE_PL));
		addChild(createLightCentered<LEDBezelLight<BlueLight>>(mm2px(Vec(192.96, 56.76)), module, SpacesCommand::VOICE2_WAVE_AN_LIGHT + 3));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(206.87, 56.76)), module, SpacesCommand::VOICE2_ATTACK_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(220.77, 56.76)), module, SpacesCommand::VOICE2_DECAY_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(234.68, 56.76)), module, SpacesCommand::VOICE2_SUSTAIN_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(248.59, 56.76)), module, SpacesCommand::VOICE2_RELEASE_PARAM));
		addParam(createParamCentered<SmallKnobVoice>(mm2px(Vec(262.5, 56.76)), module, SpacesCommand::VOICE2_TIMBRE_PARAM));

		// SCENE: A/B focus (letter inside via NanoVG overlay) + crossfader fader cap, centered, no label
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(20.0, 75.28)), module, SpacesCommand::SCENE_A_PARAM));
		addChild(createLightCentered<LEDBezelLight<RedLight>>(mm2px(Vec(20.0, 75.28)), module, SpacesCommand::SCENE_A_LIGHT));
		{
			auto* letterA = new LetterOverlay("A", nvgRGB(0xE8, 0xE8, 0xE8));
			letterA->box.size = mm2px(Vec(5.2, 5.2));
			letterA->box.pos = mm2px(Vec(20.0, 75.28)).minus(letterA->box.size.div(2));
			addChild(letterA);
		}
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(161.64, 75.28)), module, SpacesCommand::SCENE_B_PARAM));
		addChild(createLightCentered<LEDBezelLight<RedLight>>(mm2px(Vec(161.64, 75.28)), module, SpacesCommand::SCENE_B_LIGHT));
		{
			auto* letterB = new LetterOverlay("B", nvgRGB(0xE8, 0xE8, 0xE8));
			letterB->box.size = mm2px(Vec(5.2, 5.2));
			letterB->box.pos = mm2px(Vec(161.64, 75.28)).minus(letterB->box.size.div(2));
			addChild(letterB);
		}
		{
			auto* xfHandle = createParamCentered<HCrossfaderHandle>(mm2px(Vec((27.6+154.04)/2.f, 75.28)), module, SpacesCommand::MORPH_PARAM);
			xfHandle->trackX0Px = mm2px(Vec(27.6, 0)).x;
			xfHandle->trackX1Px = mm2px(Vec(154.04, 0)).x;
			xfHandle->centerY = mm2px(Vec(0, 75.28)).y;
			addParam(xfHandle);
		}

		// KEY: genuinely smaller stock component (Trimpot), since it needs to be smaller than standard
		addParam(createParamCentered<Trimpot>(mm2px(Vec(184.14, 73.78)), module, SpacesCommand::ROOT_KEY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(210.2, 73.78)), module, SpacesCommand::SCALE_TYPE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(236.26, 73.78)), module, SpacesCommand::DENSITY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(262.32, 73.78)), module, SpacesCommand::SWING_PARAM));

		// PATTERN: custom vertical faders + step lights + DICE box (randomize + nudge)
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(23.0, 87.48)), module, SpacesCommand::STEP_LIGHTS + 0));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(23.0, 101.28)), module, SpacesCommand::FADER_PARAM + 0);
			fader->trackY0Px = mm2px(Vec(0, 89.28)).y;
			fader->trackY1Px = mm2px(Vec(0, 113.28)).y;
			fader->centerX = mm2px(Vec(23.0, 0)).x;
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(44.46, 87.48)), module, SpacesCommand::STEP_LIGHTS + 1));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(44.46, 101.28)), module, SpacesCommand::FADER_PARAM + 1);
			fader->trackY0Px = mm2px(Vec(0, 89.28)).y;
			fader->trackY1Px = mm2px(Vec(0, 113.28)).y;
			fader->centerX = mm2px(Vec(44.46, 0)).x;
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(65.92, 87.48)), module, SpacesCommand::STEP_LIGHTS + 2));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(65.92, 101.28)), module, SpacesCommand::FADER_PARAM + 2);
			fader->trackY0Px = mm2px(Vec(0, 89.28)).y;
			fader->trackY1Px = mm2px(Vec(0, 113.28)).y;
			fader->centerX = mm2px(Vec(65.92, 0)).x;
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(87.38, 87.48)), module, SpacesCommand::STEP_LIGHTS + 3));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(87.38, 101.28)), module, SpacesCommand::FADER_PARAM + 3);
			fader->trackY0Px = mm2px(Vec(0, 89.28)).y;
			fader->trackY1Px = mm2px(Vec(0, 113.28)).y;
			fader->centerX = mm2px(Vec(87.38, 0)).x;
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(108.84, 87.48)), module, SpacesCommand::STEP_LIGHTS + 4));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(108.84, 101.28)), module, SpacesCommand::FADER_PARAM + 4);
			fader->trackY0Px = mm2px(Vec(0, 89.28)).y;
			fader->trackY1Px = mm2px(Vec(0, 113.28)).y;
			fader->centerX = mm2px(Vec(108.84, 0)).x;
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(130.3, 87.48)), module, SpacesCommand::STEP_LIGHTS + 5));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(130.3, 101.28)), module, SpacesCommand::FADER_PARAM + 5);
			fader->trackY0Px = mm2px(Vec(0, 89.28)).y;
			fader->trackY1Px = mm2px(Vec(0, 113.28)).y;
			fader->centerX = mm2px(Vec(130.3, 0)).x;
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(151.75, 87.48)), module, SpacesCommand::STEP_LIGHTS + 6));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(151.75, 101.28)), module, SpacesCommand::FADER_PARAM + 6);
			fader->trackY0Px = mm2px(Vec(0, 89.28)).y;
			fader->trackY1Px = mm2px(Vec(0, 113.28)).y;
			fader->centerX = mm2px(Vec(151.75, 0)).x;
			addParam(fader);
		}
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(173.21, 87.48)), module, SpacesCommand::STEP_LIGHTS + 7));
		{
			auto* fader = createParamCentered<VFaderHandle>(mm2px(Vec(173.21, 101.28)), module, SpacesCommand::FADER_PARAM + 7);
			fader->trackY0Px = mm2px(Vec(0, 89.28)).y;
			fader->trackY1Px = mm2px(Vec(0, 113.28)).y;
			fader->centerX = mm2px(Vec(173.21, 0)).x;
			addParam(fader);
		}
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(202.27, 97.18)), module, SpacesCommand::MELO_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(198.17, 106.28)), module, SpacesCommand::MELO_NUDGE_DOWN));
		{
			auto* minusLabel = new LetterOverlay("-", nvgRGB(0x2A, 0x26, 0x20));
			minusLabel->box.size = mm2px(Vec(3.6, 3.6));
			minusLabel->box.pos = mm2px(Vec(198.17, 106.28)).minus(minusLabel->box.size.div(2));
			addChild(minusLabel);
		}
		addParam(createParamCentered<TL1105>(mm2px(Vec(206.37, 106.28)), module, SpacesCommand::MELO_NUDGE_UP));
		{
			auto* plusLabel = new LetterOverlay("+", nvgRGB(0x2A, 0x26, 0x20));
			plusLabel->box.size = mm2px(Vec(3.6, 3.6));
			plusLabel->box.pos = mm2px(Vec(206.37, 106.28)).minus(plusLabel->box.size.div(2));
			addChild(plusLabel);
		}
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(219.27, 97.18)), module, SpacesCommand::DICE_ARTI));
		addParam(createParamCentered<TL1105>(mm2px(Vec(215.17, 106.28)), module, SpacesCommand::ARTI_NUDGE_DOWN));
		{
			auto* minusLabel = new LetterOverlay("-", nvgRGB(0x2A, 0x26, 0x20));
			minusLabel->box.size = mm2px(Vec(3.6, 3.6));
			minusLabel->box.pos = mm2px(Vec(215.17, 106.28)).minus(minusLabel->box.size.div(2));
			addChild(minusLabel);
		}
		addParam(createParamCentered<TL1105>(mm2px(Vec(223.37, 106.28)), module, SpacesCommand::ARTI_NUDGE_UP));
		{
			auto* plusLabel = new LetterOverlay("+", nvgRGB(0x2A, 0x26, 0x20));
			plusLabel->box.size = mm2px(Vec(3.6, 3.6));
			plusLabel->box.pos = mm2px(Vec(223.37, 106.28)).minus(plusLabel->box.size.div(2));
			addChild(plusLabel);
		}
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(236.27, 97.18)), module, SpacesCommand::DICE_TIME));
		addParam(createParamCentered<TL1105>(mm2px(Vec(232.17, 106.28)), module, SpacesCommand::TIME_NUDGE_DOWN));
		{
			auto* minusLabel = new LetterOverlay("-", nvgRGB(0x2A, 0x26, 0x20));
			minusLabel->box.size = mm2px(Vec(3.6, 3.6));
			minusLabel->box.pos = mm2px(Vec(232.17, 106.28)).minus(minusLabel->box.size.div(2));
			addChild(minusLabel);
		}
		addParam(createParamCentered<TL1105>(mm2px(Vec(240.37, 106.28)), module, SpacesCommand::TIME_NUDGE_UP));
		{
			auto* plusLabel = new LetterOverlay("+", nvgRGB(0x2A, 0x26, 0x20));
			plusLabel->box.size = mm2px(Vec(3.6, 3.6));
			plusLabel->box.pos = mm2px(Vec(240.37, 106.28)).minus(plusLabel->box.size.div(2));
			addChild(plusLabel);
		}
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(253.27, 97.18)), module, SpacesCommand::DICE_NAVY));
		addParam(createParamCentered<TL1105>(mm2px(Vec(249.17, 106.28)), module, SpacesCommand::NAVY_NUDGE_DOWN));
		{
			auto* minusLabel = new LetterOverlay("-", nvgRGB(0x2A, 0x26, 0x20));
			minusLabel->box.size = mm2px(Vec(3.6, 3.6));
			minusLabel->box.pos = mm2px(Vec(249.17, 106.28)).minus(minusLabel->box.size.div(2));
			addChild(minusLabel);
		}
		addParam(createParamCentered<TL1105>(mm2px(Vec(257.37, 106.28)), module, SpacesCommand::NAVY_NUDGE_UP));
		{
			auto* plusLabel = new LetterOverlay("+", nvgRGB(0x2A, 0x26, 0x20));
			plusLabel->box.size = mm2px(Vec(3.6, 3.6));
			plusLabel->box.pos = mm2px(Vec(257.37, 106.28)).minus(plusLabel->box.size.div(2));
			addChild(plusLabel);
		}
	}
};

Model* modelSpacesCommand = createModel<SpacesCommand, SpacesCommandWidget>("SpacesCommand");
