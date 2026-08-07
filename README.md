# Spaces — Navy Arp 2 for VCV Rack

VCV Rack port of [Navy Arp 2](https://github.com/Grandsummoner/navy-arp2), a dual-scene probabilistic
arpeggiator/step sequencer with two built-in synth voices.

## Status: functional core scaffold, not yet built/tested

Ported so far (src/NavyArp2.cpp):
- `SynthVoice`: both oscillator engines (Analog/FM/Supersaw/Pulse) + ADSR + resonant one-pole LPF,
  ported near-verbatim from `PluginProcessor.h`.
- 8-step sequencer: probability faders, rest, density, entropy-driven play direction
  (forward / ping-pong / random-forward / reverse / random-skip), root/scale quantizer,
  free-run rate (40-240 BPM).
- Scene A/B: focus-select buttons capture live knob edits into `sceneA`/`sceneB`; a morph
  knob crossfades both scenes' values for playback (matches the source plugin's model).
- Latch and Freeze toggles.
- V/Oct + Gate poly inputs feed "held notes" (replaces the VST's MIDI note on/off).
- Outputs: Pitch (1V/oct), Gate, Audio L/R (voice 1 + voice 2 panned/summed).

**Not yet ported** (flagged for follow-up passes):
- LFO modulation matrix (8 LFOs to any knob)
- Per-voice delay/reverb sends
- Preset save/load, MIDI CC learn (not applicable to Rack in the same form)
- Full custom UI: motorized knobs, LED rings, animated OLED (3D wireframe globe + probability
  towers). Current panel is a functional placeholder only.
- Swing timing (param exists, not yet wired into the step clock)
- Harmony/Chaos knobs (placeholders in the original plugin too, no real DSP there yet)

## Building

Standard Rack plugin build, see the Rack plugin development guide.
Clone into your Rack `plugins/` folder (or symlink) and `make`.
