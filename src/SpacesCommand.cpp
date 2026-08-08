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
		MELO_PARAM,
		SCENE_A_PARAM, SCENE_B_PARAM, MORPH_PARAM,
		LATCH_PARAM, ARPSEQ_PARAM, POLY_PARAM, FREEZE_PARAM, ROUTING_PARAM,
		REST_PARAM, DICE_ARTI, LEGATO_PARAM, RATE_PARAM, DICE_TIME,
		ENTROPY_PARAM, HARMONY_PARAM, CHAOS_PARAM, DICE_NAVY, OCTAVES_PARAM,
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
		configParam(LEGATO_PARAM, 0.f, 1.f, 0.5f, "Legato", "%", 0, 100);
		configParam(RATE_PARAM, 0.f, 1.f, 0.5f, "Rate", " BPM", 0, 200, 40);
		configButton(DICE_TIME, "Randomize Rate+Octaves (TIME)");
		configParam(ENTROPY_PARAM, -1.f, 1.f, 0.f, "Entropy (play direction)");
		configParam(HARMONY_PARAM, 0.f, 1.f, 0.f, "Harmony");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.f, "Chaos");
		configButton(DICE_NAVY, "Randomize Entropy+Harmony+Chaos (NAVY)");
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

	HCrossfaderHandle() {
		box.size = mm2px(Vec(5.4, 8.4));
	}

	void draw(const DrawArgs& args) override {
		float v = 0.f;
		ParamQuantity* pq = getParamQuantity();
		if (pq) v = (float)pq->getScaledValue();
		NVGcolor fillA = nvgRGB(0xB8, 0x72, 0x0A);
		NVGcolor fillB = nvgRGB(0x0E, 0x7A, 0x8C);
		NVGcolor fill = nvgLerpRGBA(fillA, fillB, v);
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, 2.f);
		nvgFillColor(args.vg, fill);
		nvgFill(args.vg);
		nvgStrokeColor(args.vg, nvgRGB(0x2A, 0x26, 0x20));
		nvgStrokeWidth(args.vg, 1.1f);
		nvgStroke(args.vg);
		// center notch so the cap reads as a fader handle, not a plain block
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, box.size.x/2, box.size.y*0.25f);
		nvgLineTo(args.vg, box.size.x/2, box.size.y*0.75f);
		nvgStrokeColor(args.vg, nvgRGBA(0xFF, 0xFF, 0xFF, 160));
		nvgStrokeWidth(args.vg, 0.8f);
		nvgStroke(args.vg);
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
};

struct SpacesCommandWidget : ModuleWidget {
	SpacesCommandWidget(SpacesCommand* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SpacesCommand.svg")));

// PATTERN: 8 vertical faders + step lights
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(21.0, 14.7)), module, SpacesCommand::STEP_LIGHTS + 0));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(21.0, 29.5)), module, SpacesCommand::FADER_PARAM + 0));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(52.05, 14.7)), module, SpacesCommand::STEP_LIGHTS + 1));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(52.05, 29.5)), module, SpacesCommand::FADER_PARAM + 1));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(83.09, 14.7)), module, SpacesCommand::STEP_LIGHTS + 2));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(83.09, 29.5)), module, SpacesCommand::FADER_PARAM + 2));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(114.14, 14.7)), module, SpacesCommand::STEP_LIGHTS + 3));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(114.14, 29.5)), module, SpacesCommand::FADER_PARAM + 3));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(145.18, 14.7)), module, SpacesCommand::STEP_LIGHTS + 4));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(145.18, 29.5)), module, SpacesCommand::FADER_PARAM + 4));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(176.23, 14.7)), module, SpacesCommand::STEP_LIGHTS + 5));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(176.23, 29.5)), module, SpacesCommand::FADER_PARAM + 5));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(207.27, 14.7)), module, SpacesCommand::STEP_LIGHTS + 6));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(207.27, 29.5)), module, SpacesCommand::FADER_PARAM + 6));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(238.32, 14.7)), module, SpacesCommand::STEP_LIGHTS + 7));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(238.32, 29.5)), module, SpacesCommand::FADER_PARAM + 7));

		// PATTERN: grouped randomize buttons (MELO/ARTI/TIME/NAVY), square LEDBezel
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(256.08, 23.5)), module, SpacesCommand::MELO_PARAM));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(262.56, 23.5)), module, SpacesCommand::DICE_ARTI));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(256.08, 35.5)), module, SpacesCommand::DICE_TIME));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(262.56, 35.5)), module, SpacesCommand::DICE_NAVY));

		// SCENE: A/B focus (square, whole-face red when focused) + custom crossfader fader cap + mode toggles (square, green on/off)
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(19.0, 54.0)), module, SpacesCommand::SCENE_A_PARAM));
		addChild(createLightCentered<LEDBezelLight<RedLight>>(mm2px(Vec(19.0, 54.0)), module, SpacesCommand::SCENE_A_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(160.19, 54.0)), module, SpacesCommand::SCENE_B_PARAM));
		addChild(createLightCentered<LEDBezelLight<RedLight>>(mm2px(Vec(160.19, 54.0)), module, SpacesCommand::SCENE_B_LIGHT));
		{
			auto* xfHandle = createParamCentered<HCrossfaderHandle>(mm2px(Vec((25.6+153.59)/2.f, 54.0)), module, SpacesCommand::MORPH_PARAM);
			xfHandle->trackX0Px = mm2px(Vec(25.6, 0)).x;
			xfHandle->trackX1Px = mm2px(Vec(153.59, 0)).x;
			addParam(xfHandle);
		}
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(186.55, 54.0)), module, SpacesCommand::LATCH_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(186.55, 54.0)), module, SpacesCommand::LATCH_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(202.9, 54.0)), module, SpacesCommand::ARPSEQ_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(202.9, 54.0)), module, SpacesCommand::ARPSEQ_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(219.26, 54.0)), module, SpacesCommand::POLY_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(219.26, 54.0)), module, SpacesCommand::POLY_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(235.61, 54.0)), module, SpacesCommand::FREEZE_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(235.61, 54.0)), module, SpacesCommand::FREEZE_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(251.97, 54.0)), module, SpacesCommand::ROUTING_PARAM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(251.97, 54.0)), module, SpacesCommand::ROUTING_LIGHT));

		// FEEL: macro knobs
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(21.0, 70.58)), module, SpacesCommand::REST_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(45.15, 70.58)), module, SpacesCommand::LEGATO_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(69.29, 70.58)), module, SpacesCommand::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(93.44, 70.58)), module, SpacesCommand::ENTROPY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(117.59, 70.58)), module, SpacesCommand::HARMONY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(141.74, 70.58)), module, SpacesCommand::CHAOS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(165.88, 70.58)), module, SpacesCommand::OCTAVES_PARAM));

		// KEY: same knob size as FEEL/DENS/SWING throughout
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(192.38, 70.58)), module, SpacesCommand::ROOT_KEY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(215.03, 70.58)), module, SpacesCommand::SCALE_TYPE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(237.67, 70.58)), module, SpacesCommand::DENSITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(260.32, 70.58)), module, SpacesCommand::SWING_PARAM));

		// VOICE1: square LEDBezel wave buttons (persisted toggle, green on/off) + knobs
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(19.58, 89.64)), module, SpacesCommand::VOICE1_WAVE_AN));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(19.58, 89.64)), module, SpacesCommand::VOICE1_WAVE_AN_LIGHT + 0));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(33.24, 89.64)), module, SpacesCommand::VOICE1_WAVE_FM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(33.24, 89.64)), module, SpacesCommand::VOICE1_WAVE_AN_LIGHT + 1));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(46.89, 89.64)), module, SpacesCommand::VOICE1_WAVE_SS));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(46.89, 89.64)), module, SpacesCommand::VOICE1_WAVE_AN_LIGHT + 2));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(60.55, 89.64)), module, SpacesCommand::VOICE1_WAVE_PL));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(60.55, 89.64)), module, SpacesCommand::VOICE1_WAVE_AN_LIGHT + 3));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(74.2, 89.64)), module, SpacesCommand::VOICE1_ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(87.86, 89.64)), module, SpacesCommand::VOICE1_DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(101.52, 89.64)), module, SpacesCommand::VOICE1_SUSTAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(115.17, 89.64)), module, SpacesCommand::VOICE1_RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(128.83, 89.64)), module, SpacesCommand::VOICE1_TIMBRE_PARAM));

		// VOICE2: square LEDBezel wave buttons (persisted toggle, green on/off) + knobs
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(152.49, 89.64)), module, SpacesCommand::VOICE2_WAVE_AN));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(152.49, 89.64)), module, SpacesCommand::VOICE2_WAVE_AN_LIGHT + 0));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(166.15, 89.64)), module, SpacesCommand::VOICE2_WAVE_FM));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(166.15, 89.64)), module, SpacesCommand::VOICE2_WAVE_AN_LIGHT + 1));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(179.8, 89.64)), module, SpacesCommand::VOICE2_WAVE_SS));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(179.8, 89.64)), module, SpacesCommand::VOICE2_WAVE_AN_LIGHT + 2));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(193.46, 89.64)), module, SpacesCommand::VOICE2_WAVE_PL));
		addChild(createLightCentered<LEDBezelLight<GreenLight>>(mm2px(Vec(193.46, 89.64)), module, SpacesCommand::VOICE2_WAVE_AN_LIGHT + 3));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(207.12, 89.64)), module, SpacesCommand::VOICE2_ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(220.77, 89.64)), module, SpacesCommand::VOICE2_DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(234.43, 89.64)), module, SpacesCommand::VOICE2_SUSTAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(248.08, 89.64)), module, SpacesCommand::VOICE2_RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(261.74, 89.64)), module, SpacesCommand::VOICE2_TIMBRE_PARAM));

		// I/O
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(19.4, 108.02)), module, SpacesCommand::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(54.05, 108.02)), module, SpacesCommand::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(88.69, 108.02)), module, SpacesCommand::VELOCITY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(123.34, 108.02)), module, SpacesCommand::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(157.98, 108.02)), module, SpacesCommand::VOICE1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(192.63, 108.02)), module, SpacesCommand::VOICE2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(227.27, 108.02)), module, SpacesCommand::MASTER_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(261.92, 108.02)), module, SpacesCommand::MASTER_R_OUTPUT));
	}
};

Model* modelSpacesCommand = createModel<SpacesCommand, SpacesCommandWidget>("SpacesCommand");
