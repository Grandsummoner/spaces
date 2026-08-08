# Spaces

VCV Rack plugin family, ported from [Navy Arp 2](https://github.com/Grandsummoner/navy-arp2) (JUCE VST3).

## Spaces Command (implemented)

Self-sufficient core instrument: 8-step probabilistic pattern sequencer with dual-scene
(A/B) morphing and two built-in synth voices (Analog/FM/Supersaw/Pulse, non-exclusive
layering, plain-averaged amplitude).

**Panel sections:**
- PATTERN: 8 vertical probability faders + step lights + MELO randomize button
- SCENE: Octatrack-style A/B crossfader + LATCH/ARP-SEQ/POLY/FREEZE/ROUTING toggles
- FEEL: Rest/Legato/Rate/Entropy/Harmony/Chaos/Octaves knobs + ARTI/TIME/NAVY randomize
  buttons (bundled exactly as the original VST's dice buttons)
- KEY/DENSITY: Root/Scale/Density/Swing
- VOICE 1 / VOICE 2: 4 non-exclusive waveform buttons + ADSR + Timbre each
- I/O: V/Oct, Gate, Velocity, Clock in \u00b7 Voice 1, Voice 2, Master L, Master R out

**Run/stop:** clock-presence based. Patch a clock into CLOCK IN and patching/unpatching
it starts/stops the sequencer (standard Eurorack convention). Unpatched, it free-runs
off the RATE knob, gated by held notes or FREEZE.

**Routing modes:** TOGETHER (both voices fire on every note, original VST behavior) or
SPLIT A\u00b7B (Voice 1 leans toward Scene A, Voice 2 toward Scene B, new mode not in the
original -- lets the two voices diverge into distinct instruments as you move the
crossfader).

Note input is external, same as any Rack instrument -- patch VCV core's MIDI-CV module
(reads your keyboard/controller) into V/Oct + Gate + Velocity.

## Spaces Intel (planned, expander)
VU meter, animated OLED/globe display, per-voice Reverb/Delay send knobs, 8-LFO
modulation matrix. Connects to Command via VCV's Expander system (place side-by-side,
no cables).

## Spaces Supply (planned, expander)
8 preset-slot buttons (hold = save with flash confirmation + auto-unlatch, tap = recall,
stays lit until a different slot is tapped) + small functional value readout
(Root/Scale/BPM/active slot). Connects via Expander.

## Building

Panel art and control layout are generated from `spaces_command_layout.py` (single
source of truth -- computes every control position from real VCV component sizes,
asserts the layout fits the fixed 128.5mm panel height, and writes both
`res/SpacesCommand.svg` and `command_layout.json` before any C++ is touched). Re-run it
with `python3 spaces_command_layout.py` if you want to adjust the layout; it requires
`fonttools` (`pip install fonttools --break-system-packages`).

Standard Rack plugin build from there: `RACK_DIR=<path to Rack SDK> make`, or via the
GitHub Actions workflow in `.github/workflows/build.yml` (Windows build, runs on push).
