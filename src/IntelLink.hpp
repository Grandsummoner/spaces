#pragma once
// Shared message layout for the invisible Intel <-> Command expander link.
// This exact struct definition is duplicated verbatim in both the Intel and
// Spaces (Command) repos -- they're separate plugin binaries, so there's no
// shared header to include from; keeping the text identical in both copies
// is what keeps the memory layout compatible across the expander boundary.
// If you ever add/reorder fields here, make the identical edit in the other
// repo's copy or the two sides will silently misread each other.
struct IntelModMessage {
	// Continuous modulation offsets, already depth-scaled by Intel, roughly
	// in a -1..1 range (Command decides how to apply/scale/floor each one
	// against its own parameter's real range).
	float rateOffset = 0.f;
	float densOffset = 0.f;
	float swingOffset = 0.f;
	float entropyOffset = 0.f;
	// True whenever Intel is actually present and sending -- lets Command
	// tell "adjacent but message not yet flipped this frame" apart from
	// "no Intel here", though in practice the adjacency check on Command's
	// side (model slug match) already covers most of that distinction.
	bool present = false;
};
