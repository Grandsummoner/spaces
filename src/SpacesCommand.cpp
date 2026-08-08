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
	enum LightId { ENUMS(STEP_LIGHTS, 8), SCENE_A_LIGHT, SCENE_B_LIGHT, LIGHTS_LEN };

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

	void process(const ProcessArgs& args) override {
		voice1.sampleRate = args.sampleRate;
		voice2.sampleRate = args.sampleRate;

		if (sceneATrig.process(params[SCENE_A_PARAM].getValue())) focusB = false;
		if (sceneBTrig.process(params[SCENE_B_PARAM].getValue())) focusB = true;
		lights[SCENE_A_LIGHT].setBrightness(!focusB);
		lights[SCENE_B_LIGHT].setBrightness(focusB);

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

		int wave1 = 0;
		bool v1an = params[VOICE1_WAVE_AN].getValue() > 0.5f;
		bool v1fm = params[VOICE1_WAVE_FM].getValue() > 0.5f;
		bool v1ss = params[VOICE1_WAVE_SS].getValue() > 0.5f;
		bool v1pl = params[VOICE1_WAVE_PL].getValue() > 0.5f;
		bool v2an = params[VOICE2_WAVE_AN].getValue() > 0.5f;
		bool v2fm = params[VOICE2_WAVE_FM].getValue() > 0.5f;
		bool v2ss = params[VOICE2_WAVE_SS].getValue() > 0.5f;
		bool v2pl = params[VOICE2_WAVE_PL].getValue() > 0.5f;
		(void)wave1;

		float v1out = voice1.process(v1an, v1fm, v1ss, v1pl, params[VOICE1_TIMBRE_PARAM].getValue());
		float v2out = voice2.process(v2an, v2fm, v2ss, v2pl, params[VOICE2_TIMBRE_PARAM].getValue());

		outputs[VOICE1_OUTPUT].setVoltage(v1out * 5.f);
		outputs[VOICE2_OUTPUT].setVoltage(v2out * 5.f);
		outputs[MASTER_L_OUTPUT].setVoltage((v1out * 0.7f + v2out * 0.3f) * 5.f);
		outputs[MASTER_R_OUTPUT].setVoltage((v1out * 0.3f + v2out * 0.7f) * 5.f);
	}
};

struct SpacesCommandWidget : ModuleWidget {
	SpacesCommandWidget(SpacesCommand* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/SpacesCommand.svg")));

// PATTERN: 8 vertical faders + step lights
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(16.0, 17.7)), module, SpacesCommand::STEP_LIGHTS + 0));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(16.0, 30.5)), module, SpacesCommand::FADER_PARAM + 0));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(47.76, 17.7)), module, SpacesCommand::STEP_LIGHTS + 1));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(47.76, 30.5)), module, SpacesCommand::FADER_PARAM + 1));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(79.52, 17.7)), module, SpacesCommand::STEP_LIGHTS + 2));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(79.52, 30.5)), module, SpacesCommand::FADER_PARAM + 2));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(111.28, 17.7)), module, SpacesCommand::STEP_LIGHTS + 3));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(111.28, 30.5)), module, SpacesCommand::FADER_PARAM + 3));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(143.04, 17.7)), module, SpacesCommand::STEP_LIGHTS + 4));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(143.04, 30.5)), module, SpacesCommand::FADER_PARAM + 4));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(174.8, 17.7)), module, SpacesCommand::STEP_LIGHTS + 5));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(174.8, 30.5)), module, SpacesCommand::FADER_PARAM + 5));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(206.56, 17.7)), module, SpacesCommand::STEP_LIGHTS + 6));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(206.56, 30.5)), module, SpacesCommand::FADER_PARAM + 6));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(238.32, 17.7)), module, SpacesCommand::STEP_LIGHTS + 7));
		addParam(createParamCentered<LEDSliderGreen>(mm2px(Vec(238.32, 30.5)), module, SpacesCommand::FADER_PARAM + 7));
		addParam(createParamCentered<TL1105>(mm2px(Vec(260.32, 30.5)), module, SpacesCommand::MELO_PARAM));

		// SCENE: crossfader + A/B focus + mode toggles
		addParam(createParamCentered<CKD6>(mm2px(Vec(12.0, 55.13)), module, SpacesCommand::SCENE_A_PARAM));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(12.0, 50.13)), module, SpacesCommand::SCENE_A_LIGHT));
		addParam(createParamCentered<CKD6>(mm2px(Vec(162.64, 55.13)), module, SpacesCommand::SCENE_B_PARAM));
		addChild(createLightCentered<SmallLight<BlueLight>>(mm2px(Vec(162.64, 50.13)), module, SpacesCommand::SCENE_B_LIGHT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(87.32, 55.13)), module, SpacesCommand::MORPH_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(188.59, 55.13)), module, SpacesCommand::LATCH_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(204.53, 55.13)), module, SpacesCommand::ARPSEQ_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(220.48, 55.13)), module, SpacesCommand::POLY_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(236.43, 55.13)), module, SpacesCommand::FREEZE_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(252.37, 55.13)), module, SpacesCommand::ROUTING_PARAM));

		// FEEL: macro knobs + dice buttons
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(12.0, 73.83)), module, SpacesCommand::REST_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(30.49, 73.83)), module, SpacesCommand::DICE_ARTI));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(48.97, 73.83)), module, SpacesCommand::LEGATO_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(67.46, 73.83)), module, SpacesCommand::RATE_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(85.95, 73.83)), module, SpacesCommand::DICE_TIME));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(104.43, 73.83)), module, SpacesCommand::ENTROPY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(122.92, 73.83)), module, SpacesCommand::HARMONY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(141.4, 73.83)), module, SpacesCommand::CHAOS_PARAM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(159.89, 73.83)), module, SpacesCommand::DICE_NAVY));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(178.38, 73.83)), module, SpacesCommand::OCTAVES_PARAM));

		// KEY / DENSITY
		addParam(createParamCentered<Trimpot>(mm2px(Vec(192.88, 73.83)), module, SpacesCommand::ROOT_KEY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(215.69, 73.83)), module, SpacesCommand::SCALE_TYPE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(238.51, 73.83)), module, SpacesCommand::DENSITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(261.32, 73.83)), module, SpacesCommand::SWING_PARAM));

		// VOICE1
		addParam(createParamCentered<TL1105>(mm2px(Vec(12.58, 93.89)), module, SpacesCommand::VOICE1_WAVE_AN));
		addParam(createParamCentered<TL1105>(mm2px(Vec(27.24, 93.89)), module, SpacesCommand::VOICE1_WAVE_FM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(41.89, 93.89)), module, SpacesCommand::VOICE1_WAVE_SS));
		addParam(createParamCentered<TL1105>(mm2px(Vec(56.55, 93.89)), module, SpacesCommand::VOICE1_WAVE_PL));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(71.2, 93.89)), module, SpacesCommand::VOICE1_ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(85.86, 93.89)), module, SpacesCommand::VOICE1_DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(100.52, 93.89)), module, SpacesCommand::VOICE1_SUSTAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(115.17, 93.89)), module, SpacesCommand::VOICE1_RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(129.83, 93.89)), module, SpacesCommand::VOICE1_TIMBRE_PARAM));

		// VOICE2
		addParam(createParamCentered<TL1105>(mm2px(Vec(144.49, 93.89)), module, SpacesCommand::VOICE2_WAVE_AN));
		addParam(createParamCentered<TL1105>(mm2px(Vec(159.15, 93.89)), module, SpacesCommand::VOICE2_WAVE_FM));
		addParam(createParamCentered<TL1105>(mm2px(Vec(173.8, 93.89)), module, SpacesCommand::VOICE2_WAVE_SS));
		addParam(createParamCentered<TL1105>(mm2px(Vec(188.46, 93.89)), module, SpacesCommand::VOICE2_WAVE_PL));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(203.12, 93.89)), module, SpacesCommand::VOICE2_ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(217.77, 93.89)), module, SpacesCommand::VOICE2_DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(232.43, 93.89)), module, SpacesCommand::VOICE2_SUSTAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(247.08, 93.89)), module, SpacesCommand::VOICE2_RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(261.74, 93.89)), module, SpacesCommand::VOICE2_TIMBRE_PARAM));

		// I/O
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.4, 113.27)), module, SpacesCommand::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(48.05, 113.27)), module, SpacesCommand::GATE_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(83.69, 113.27)), module, SpacesCommand::VELOCITY_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(119.34, 113.27)), module, SpacesCommand::CLOCK_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(154.98, 113.27)), module, SpacesCommand::VOICE1_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(190.63, 113.27)), module, SpacesCommand::VOICE2_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(226.27, 113.27)), module, SpacesCommand::MASTER_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(261.92, 113.27)), module, SpacesCommand::MASTER_R_OUTPUT));
	}
};

Model* modelSpacesCommand = createModel<SpacesCommand, SpacesCommandWidget>("SpacesCommand");
