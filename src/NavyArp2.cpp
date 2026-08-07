#include "plugin.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

// =====================================================================
// SYNTH VOICE — ported near-verbatim from source/PluginProcessor.h SynthVoice
// =====================================================================
struct SynthVoice {
	float sampleRate = 44100.f;
	float phase = 0.f;
	float phaseIncrement = 0.f;

	float fmModPhase = 0.f;
	float fmModFreq = 0.f;

	float sawPhases[7] = {};
	float sawPhaseIncrements[7] = {};

	float s1 = 0.f, s2 = 0.f;
	float sSupersaw = 0.f;

	float attack = 0.01f;
	float decay = 0.35f;
	float sustain = 0.70f;
	float release = 0.25f;

	enum class EnvState { Idle, Attack, Decay, Sustain, Release };
	EnvState envState = EnvState::Idle;
	float envVal = 0.f;
	float releaseLevel = 0.f;
	double stateTime = 0.0;

	int activeNoteCount = 0;

	void triggerNote(float pitchVolt) {
		// pitchVolt is Rack 1V/oct where 0V = C4 (MIDI 60)
		float freq = dsp::FREQ_C4 * std::pow(2.f, pitchVolt);
		phaseIncrement = freq / sampleRate;

		float detunes[7] = { -0.06f, -0.04f, -0.015f, 0.f, 0.015f, 0.04f, 0.06f };
		for (int i = 0; i < 7; i++) {
			float detunedFreq = freq * std::pow(2.f, detunes[i] / 12.f);
			sawPhaseIncrements[i] = detunedFreq / sampleRate;
		}

		if (activeNoteCount == 0) {
			envState = EnvState::Attack;
			stateTime = 0.0;
			envVal = 0.f;
		}
		activeNoteCount++;
	}

	void releaseNote() {
		if (activeNoteCount > 0)
			activeNoteCount--;

		if (activeNoteCount == 0) {
			if (envState != EnvState::Idle && envState != EnvState::Release) {
				envState = EnvState::Release;
				releaseLevel = envVal;
				stateTime = 0.0;
			}
		}
	}

	float process(bool analogActive, bool fmActive, bool supersawActive, bool pulseActive, float cutoffKnob) {
		double dt = 1.0 / sampleRate;
		stateTime += dt;

		switch (envState) {
			case EnvState::Idle:
				envVal = 0.f;
				break;
			case EnvState::Attack: {
				float dur = std::max(0.001f, attack);
				envVal = (float)(stateTime / dur);
				if (envVal >= 1.f) { envVal = 1.f; envState = EnvState::Decay; stateTime = 0.0; }
				break;
			}
			case EnvState::Decay: {
				float dur = std::max(0.001f, decay);
				float progress = (float)(stateTime / dur);
				if (progress >= 1.f) { envVal = sustain; envState = EnvState::Sustain; stateTime = 0.0; }
				else { envVal = 1.f - (1.f - sustain) * progress; }
				break;
			}
			case EnvState::Sustain:
				envVal = sustain;
				break;
			case EnvState::Release: {
				float dur = std::max(0.001f, release);
				float progress = (float)(stateTime / dur);
				if (progress >= 1.f) { envVal = 0.f; envState = EnvState::Idle; }
				else { envVal = releaseLevel * (1.f - progress); }
				break;
			}
		}

		if (envVal <= 0.0001f) { envVal = 0.f; return 0.f; }

		float totalOutput = 0.f;
		int activeCount = 0;

		bool needsPhaseAdvance = analogActive || fmActive || pulseActive;

		float filterCutoffHz = 60.f + (cutoffKnob * 7500.f) * envVal;
		float wd = 2.f * (float)M_PI * filterCutoffHz / sampleRate;
		float g = std::tan(wd * 0.5f);
		float h = g / (1.f + g);

		if (analogActive) {
			float wave = 2.f * phase - 1.f;
			float resonance = 0.45f;
			float filterOut = (wave - s1 * resonance) * h + s1;
			s1 = filterOut;
			totalOutput += filterOut;
			activeCount++;
		}

		if (fmActive) {
			float modMultiplier = 3.5f;
			float fmModPhaseIncrement = phaseIncrement * modMultiplier;
			fmModPhase += fmModPhaseIncrement;
			if (fmModPhase >= 1.f) fmModPhase -= 1.f;

			float modOut = std::sin(fmModPhase * 2.f * (float)M_PI);
			float modIndex = cutoffKnob * 6.5f;

			float carrierPhase = phase + modOut * modIndex * phaseIncrement;
			float carrierWave = std::sin(carrierPhase * 2.f * (float)M_PI);

			float resonance = 0.35f;
			float filterOut = (carrierWave - s1 * resonance) * h + s1;
			s1 = filterOut;
			totalOutput += filterOut;
			activeCount++;
		}

		if (supersawActive) {
			float supersawSum = 0.f;
			for (int i = 0; i < 7; i++) {
				float sawWave = 2.f * sawPhases[i] - 1.f;
				supersawSum += sawWave;
				sawPhases[i] += sawPhaseIncrements[i];
				if (sawPhases[i] >= 1.f) sawPhases[i] -= 1.f;
			}
			float rawSupersaw = supersawSum * 0.35f;
			float resonance = 0.38f;
			float filterOut = (rawSupersaw - sSupersaw * resonance) * h + sSupersaw;
			sSupersaw = filterOut;
			totalOutput += filterOut;
			activeCount++;
		}

		if (pulseActive) {
			float width = 0.15f + cutoffKnob * 0.7f;
			float wave = (phase < width) ? 0.4f : -0.4f;
			float resonance = 0.40f;
			float filterOut = (wave - s1 * resonance) * h + s1;
			s1 = filterOut;
			totalOutput += filterOut * 0.6f;
			activeCount++;
		}

		if (needsPhaseAdvance) {
			phase += phaseIncrement;
			if (phase >= 1.f) phase -= 1.f;
		}

		if (activeCount > 1)
			totalOutput /= std::sqrt((float)activeCount);

		return totalOutput * envVal;
	}
};

// =====================================================================
// SCENE STATE — ported from PluginProcessor.h SceneState
// =====================================================================
struct SceneState {
	float faders[8] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
	float rest = 0.1f;
	float legato = 0.5f;
	float rate = 2.f / 3.f;
	float entropy = 0.f;
	float harmony = 0.f;
	float chaos = 0.f;
	float octaves = 0.f;
};

// =====================================================================
// MODULE
// =====================================================================
struct NavyArp2 : Module {
	enum ParamId {
		ENUMS(FADER_PARAM, 8),
		REST_PARAM,
		LEGATO_PARAM,
		RATE_PARAM,
		ENTROPY_PARAM,
		HARMONY_PARAM,
		CHAOS_PARAM,
		OCTAVES_PARAM,
		MORPH_PARAM,
		SCENE_A_PARAM,
		SCENE_B_PARAM,
		LATCH_PARAM,
		FREEZE_PARAM,
		ROOT_KEY_PARAM,
		SCALE_TYPE_PARAM,
		DENSITY_PARAM,
		SWING_PARAM,
		VOICE1_WAVE_PARAM,
		VOICE2_WAVE_PARAM,
		VOICE1_ATTACK_PARAM, VOICE1_DECAY_PARAM, VOICE1_SUSTAIN_PARAM, VOICE1_RELEASE_PARAM, VOICE1_TIMBRE_PARAM,
		VOICE2_ATTACK_PARAM, VOICE2_DECAY_PARAM, VOICE2_SUSTAIN_PARAM, VOICE2_RELEASE_PARAM, VOICE2_TIMBRE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		VOCT_INPUT,
		GATE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		PITCH_OUTPUT,
		GATE_OUTPUT,
		AUDIO_L_OUTPUT,
		AUDIO_R_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		ENUMS(STEP_LIGHTS, 8),
		SCENE_A_LIGHT,
		SCENE_B_LIGHT,
		LIGHTS_LEN
	};

	// Oscillator waveform enum matches VST panel buttons: 0=Analog,1=FM,2=Supersaw,3=Pulse
	SceneState sceneA;
	SceneState sceneB;
	bool focusB = false;

	int currentStep = 0;
	bool goingForward = true;
	double phaseAccumSamples = 0.0;
	int lastStep = -1;

	std::vector<int> heldNotes;
	std::vector<int> latchedNotes;
	int lastNotePlayedVoice1 = -1;

	SynthVoice voice1;
	SynthVoice voice2;

	dsp::PulseGenerator gatePulse;
	dsp::SchmittTrigger sceneATrig, sceneBTrig, gateInTrig[16];

	NavyArp2() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		for (int i = 0; i < 8; i++)
			configParam(FADER_PARAM + i, 0.f, 1.f, 1.f, string::f("Step %d probability", i + 1), "%", 0, 100);

		configParam(REST_PARAM, 0.f, 1.f, 0.1f, "Rest probability", "%", 0, 100);
		configParam(LEGATO_PARAM, 0.f, 1.f, 0.5f, "Legato", "%", 0, 100);
		configParam(RATE_PARAM, 0.f, 1.f, 2.f / 3.f, "Rate", " BPM", 0, 200, 40);
		configParam(ENTROPY_PARAM, -1.f, 1.f, 0.f, "Entropy (play direction)");
		configParam(HARMONY_PARAM, 0.f, 1.f, 0.f, "Harmony");
		configParam(CHAOS_PARAM, 0.f, 1.f, 0.f, "Chaos");
		configParam(OCTAVES_PARAM, -3.f, 3.f, 0.f, "Octave shift");
		configParam(MORPH_PARAM, 0.f, 1.f, 0.f, "Scene morph", "%", 0, 100);
		configButton(SCENE_A_PARAM, "Focus Scene A");
		configButton(SCENE_B_PARAM, "Focus Scene B");
		configSwitch(LATCH_PARAM, 0.f, 1.f, 0.f, "Latch", {"Off", "On"});
		configSwitch(FREEZE_PARAM, 0.f, 1.f, 0.f, "Freeze", {"Off", "On"});
		configParam(ROOT_KEY_PARAM, 0.f, 11.f, 0.f, "Root key");
		getParamQuantity(ROOT_KEY_PARAM)->snapEnabled = true;
		configParam(SCALE_TYPE_PARAM, 0.f, 9.f, 0.f, "Scale");
		getParamQuantity(SCALE_TYPE_PARAM)->snapEnabled = true;
		configParam(DENSITY_PARAM, 0.f, 1.f, 0.5f, "Density", "%", 0, 100);
		configParam(SWING_PARAM, 0.f, 1.f, 0.f, "Swing", "%", 0, 100);

		configSwitch(VOICE1_WAVE_PARAM, 0.f, 3.f, 0.f, "Voice 1 waveform", {"Analog", "FM", "Supersaw", "Pulse"});
		getParamQuantity(VOICE1_WAVE_PARAM)->snapEnabled = true;
		configSwitch(VOICE2_WAVE_PARAM, 0.f, 3.f, 2.f, "Voice 2 waveform", {"Analog", "FM", "Supersaw", "Pulse"});
		getParamQuantity(VOICE2_WAVE_PARAM)->snapEnabled = true;

		configParam(VOICE1_ATTACK_PARAM, 0.001f, 2.f, 0.01f, "Voice 1 attack", " s");
		configParam(VOICE1_DECAY_PARAM, 0.001f, 2.f, 0.35f, "Voice 1 decay", " s");
		configParam(VOICE1_SUSTAIN_PARAM, 0.f, 1.f, 0.70f, "Voice 1 sustain", "%", 0, 100);
		configParam(VOICE1_RELEASE_PARAM, 0.001f, 4.f, 0.25f, "Voice 1 release", " s");
		configParam(VOICE1_TIMBRE_PARAM, 0.f, 1.f, 0.5f, "Voice 1 timbre");

		configParam(VOICE2_ATTACK_PARAM, 0.001f, 2.f, 0.01f, "Voice 2 attack", " s");
		configParam(VOICE2_DECAY_PARAM, 0.001f, 2.f, 0.35f, "Voice 2 decay", " s");
		configParam(VOICE2_SUSTAIN_PARAM, 0.f, 1.f, 0.70f, "Voice 2 sustain", "%", 0, 100);
		configParam(VOICE2_RELEASE_PARAM, 0.001f, 4.f, 0.25f, "Voice 2 release", " s");
		configParam(VOICE2_TIMBRE_PARAM, 0.f, 1.f, 0.5f, "Voice 2 timbre");

		configInput(VOCT_INPUT, "1V/oct pitch (poly, held notes)");
		configInput(GATE_INPUT, "Gate (poly, held notes)");
		configOutput(PITCH_OUTPUT, "1V/oct pitch");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(AUDIO_L_OUTPUT, "Audio L");
		configOutput(AUDIO_R_OUTPUT, "Audio R");
	}

	// capture currently-touched knob values into the focused scene each frame
	void captureFocusedScene() {
		SceneState& s = focusB ? sceneB : sceneA;
		for (int i = 0; i < 8; i++)
			s.faders[i] = params[FADER_PARAM + i].getValue();
		s.rest = params[REST_PARAM].getValue();
		s.legato = params[LEGATO_PARAM].getValue();
		s.rate = params[RATE_PARAM].getValue();
		s.entropy = params[ENTROPY_PARAM].getValue();
		s.harmony = params[HARMONY_PARAM].getValue();
		s.chaos = params[CHAOS_PARAM].getValue();
		s.octaves = params[OCTAVES_PARAM].getValue();
	}

	void process(const ProcessArgs& args) override {
		voice1.sampleRate = args.sampleRate;
		voice2.sampleRate = args.sampleRate;

		if (sceneATrig.process(params[SCENE_A_PARAM].getValue()))
			focusB = false;
		if (sceneBTrig.process(params[SCENE_B_PARAM].getValue()))
			focusB = true;
		lights[SCENE_A_LIGHT].setBrightness(!focusB);
		lights[SCENE_B_LIGHT].setBrightness(focusB);

		captureFocusedScene();

		float morph = params[MORPH_PARAM].getValue();
		float rest = crossfade(sceneA.rest, sceneB.rest, morph);
		float rate01 = crossfade(sceneA.rate, sceneB.rate, morph);
		float entropy = crossfade(sceneA.entropy, sceneB.entropy, morph);
		float octavesF = crossfade(sceneA.octaves, sceneB.octaves, morph);
		int octaveShift = (int)std::round(octavesF);

		// Gather held notes from poly V/Oct + Gate inputs
		heldNotes.clear();
		int channels = std::max(inputs[VOCT_INPUT].getChannels(), 1);
		for (int c = 0; c < channels; c++) {
			bool gateHigh = inputs[GATE_INPUT].getVoltage(c) >= 1.f;
			if (gateHigh) {
				float volt = inputs[VOCT_INPUT].getVoltage(c);
				int pitch = 60 + (int)std::round(volt * 12.f);
				heldNotes.push_back(pitch);
			}
		}
		bool latchOn = params[LATCH_PARAM].getValue() > 0.5f;
		bool freezeOn = params[FREEZE_PARAM].getValue() > 0.5f;
		if (!heldNotes.empty() && latchOn)
			latchedNotes = heldNotes;
		std::vector<int>& notesToPlay = latchOn ? latchedNotes : heldNotes;

		bool playing = !notesToPlay.empty() || freezeOn;

		double bpm = 40.0 + rate01 * 200.0;
		double samplesPerBeat = args.sampleRate * (60.0 / std::max(1.0, bpm));
		double stepSamples = samplesPerBeat * 0.25; // 1/16 note steps, free-run

		bool stepTriggered = false;
		if (playing) {
			phaseAccumSamples += 1.0;
			if (phaseAccumSamples >= stepSamples) {
				phaseAccumSamples = 0.0;
				stepTriggered = true;
			}
		}

		if (stepTriggered) {
			// play-direction from entropy, ported from PluginProcessor::processBlock
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
			}
			else if (playDirection == 2) {
				float r = random::uniform();
				if (r < 0.7f) localStep = (localStep + 1) % 8;
				else if (r < 0.9f) localStep = (localStep - 1 + 8) % 8;
			}
			else if (playDirection == 3) {
				localStep = (localStep - 1 + 8) % 8;
			}
			else if (playDirection == 4) {
				localStep = (random::uniform() < 0.2f) ? (localStep + 2) % 8 : (localStep + 1) % 8;
			}
			else {
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
				outputs[PITCH_OUTPUT].setVoltage(pitchVolt);
				gatePulse.trigger(1e-3f);

				voice1.attack = params[VOICE1_ATTACK_PARAM].getValue();
				voice1.decay = params[VOICE1_DECAY_PARAM].getValue();
				voice1.sustain = params[VOICE1_SUSTAIN_PARAM].getValue();
				voice1.release = params[VOICE1_RELEASE_PARAM].getValue();
				voice2.attack = params[VOICE2_ATTACK_PARAM].getValue();
				voice2.decay = params[VOICE2_DECAY_PARAM].getValue();
				voice2.sustain = params[VOICE2_SUSTAIN_PARAM].getValue();
				voice2.release = params[VOICE2_RELEASE_PARAM].getValue();

				voice1.triggerNote(pitchVolt);
				voice2.triggerNote(pitchVolt);
			}

			for (int i = 0; i < 8; i++)
				lights[STEP_LIGHTS + i].setBrightness(i == localStep ? 1.f : 0.f);
		}

		bool gateOut = gatePulse.process(args.sampleTime);
		outputs[GATE_OUTPUT].setVoltage(gateOut ? 10.f : 0.f);

		int wave1 = (int)std::round(params[VOICE1_WAVE_PARAM].getValue());
		int wave2 = (int)std::round(params[VOICE2_WAVE_PARAM].getValue());
		float v1 = voice1.process(wave1 == 0, wave1 == 1, wave1 == 2, wave1 == 3, params[VOICE1_TIMBRE_PARAM].getValue());
		float v2 = voice2.process(wave2 == 0, wave2 == 1, wave2 == 2, wave2 == 3, params[VOICE2_TIMBRE_PARAM].getValue());

		float outL = (v1 * 0.8f + v2 * 0.2f) * 5.f;
		float outR = (v1 * 0.2f + v2 * 0.8f) * 5.f;
		outputs[AUDIO_L_OUTPUT].setVoltage(outL);
		outputs[AUDIO_R_OUTPUT].setVoltage(outR);
	}
};

// =====================================================================
// WIDGET — grid-aligned to the section layout in build_panel.py /
// panel_sections.json. Motorized-knob / LED-ring / OLED-globe widgets
// still to come; using stock VCV components for now so the layout and
// grouping are correct first.
// =====================================================================
struct NavyArp2Widget : ModuleWidget {
	NavyArp2Widget(NavyArp2* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/NavyArp2.svg")));

// STEPS: probability faders + step lights
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(10.0, 19.1)), module, NavyArp2::STEP_LIGHTS + 0));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(10.0, 27.0)), module, NavyArp2::FADER_PARAM + 0));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(30.37, 19.1)), module, NavyArp2::STEP_LIGHTS + 1));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(30.37, 27.0)), module, NavyArp2::FADER_PARAM + 1));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(50.73, 19.1)), module, NavyArp2::STEP_LIGHTS + 2));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(50.73, 27.0)), module, NavyArp2::FADER_PARAM + 2));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(71.1, 19.1)), module, NavyArp2::STEP_LIGHTS + 3));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(71.1, 27.0)), module, NavyArp2::FADER_PARAM + 3));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(91.46, 19.1)), module, NavyArp2::STEP_LIGHTS + 4));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(91.46, 27.0)), module, NavyArp2::FADER_PARAM + 4));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(111.83, 19.1)), module, NavyArp2::STEP_LIGHTS + 5));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(111.83, 27.0)), module, NavyArp2::FADER_PARAM + 5));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(132.19, 19.1)), module, NavyArp2::STEP_LIGHTS + 6));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(132.19, 27.0)), module, NavyArp2::FADER_PARAM + 6));
		addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(152.56, 19.1)), module, NavyArp2::STEP_LIGHTS + 7));
		addParam(createParamCentered<RoundBigBlackKnob>(mm2px(Vec(152.56, 27.0)), module, NavyArp2::FADER_PARAM + 7));

		// MACRO
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(10.0, 49.77)), module, NavyArp2::REST_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(23.22, 49.77)), module, NavyArp2::LEGATO_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.44, 49.77)), module, NavyArp2::RATE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(49.66, 49.77)), module, NavyArp2::ENTROPY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(62.88, 49.77)), module, NavyArp2::HARMONY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(76.1, 49.77)), module, NavyArp2::CHAOS_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(89.32, 49.77)), module, NavyArp2::OCTAVES_PARAM));

		// SCENE / MODE
		addParam(createParamCentered<CKD6>(mm2px(Vec(98.82, 49.77)), module, NavyArp2::SCENE_A_PARAM));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(94.61999999999999, 45.57)), module, NavyArp2::SCENE_A_LIGHT));
		addParam(createParamCentered<CKD6>(mm2px(Vec(112.26, 49.77)), module, NavyArp2::SCENE_B_PARAM));
		addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(108.06, 45.57)), module, NavyArp2::SCENE_B_LIGHT));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(125.69, 49.77)), module, NavyArp2::MORPH_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(139.13, 49.77)), module, NavyArp2::LATCH_PARAM));
		addParam(createParamCentered<CKSS>(mm2px(Vec(152.56, 49.77)), module, NavyArp2::FREEZE_PARAM));

		// KEY / DENSITY
		addParam(createParamCentered<Trimpot>(mm2px(Vec(10.0, 71.53)), module, NavyArp2::ROOT_KEY_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(25.4, 71.53)), module, NavyArp2::SCALE_TYPE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(40.8, 71.53)), module, NavyArp2::DENSITY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(56.2, 71.53)), module, NavyArp2::SWING_PARAM));

		// VOICE1
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(10.0, 91.61)), module, NavyArp2::VOICE1_WAVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(23.31, 91.61)), module, NavyArp2::VOICE1_ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(36.61, 91.61)), module, NavyArp2::VOICE1_DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(49.92, 91.61)), module, NavyArp2::VOICE1_SUSTAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(63.22, 91.61)), module, NavyArp2::VOICE1_RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(76.53, 91.61)), module, NavyArp2::VOICE1_TIMBRE_PARAM));

		// VOICE2
		addParam(createParamCentered<CKSSThree>(mm2px(Vec(86.03, 91.61)), module, NavyArp2::VOICE2_WAVE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(99.34, 91.61)), module, NavyArp2::VOICE2_ATTACK_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(112.64, 91.61)), module, NavyArp2::VOICE2_DECAY_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(125.95, 91.61)), module, NavyArp2::VOICE2_SUSTAIN_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(139.25, 91.61)), module, NavyArp2::VOICE2_RELEASE_PARAM));
		addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(152.56, 91.61)), module, NavyArp2::VOICE2_TIMBRE_PARAM));

		// I/O
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(14.0, 111.01)), module, NavyArp2::VOCT_INPUT));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(40.91, 111.01)), module, NavyArp2::GATE_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(67.82, 111.01)), module, NavyArp2::PITCH_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(94.74, 111.01)), module, NavyArp2::GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(121.65, 111.01)), module, NavyArp2::AUDIO_L_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(148.56, 111.01)), module, NavyArp2::AUDIO_R_OUTPUT));
	}
};

Model* modelNavyArp2 = createModel<NavyArp2, NavyArp2Widget>("NavyArp2");
