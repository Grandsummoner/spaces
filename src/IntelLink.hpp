#pragma once
#include <rack.hpp>
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
	// Static DEPTH-setting level per channel, 0 (no LFO) to 1 (max LFO) --
	// i.e. depthState/3, NOT the live oscillating value above. Command's
	// optional depth-gauge display reads these; actual modulation
	// application still uses the *Offset fields.
	float rateDepth = 0.f;
	float densDepth = 0.f;
	float swingDepth = 0.f;
	float entropyDepth = 0.f;
	// True whenever Intel is actually present and sending -- lets Command
	// tell "adjacent but message not yet flipped this frame" apart from
	// "no Intel here", though in practice the adjacency check on Command's
	// side (model slug match) already covers most of that distinction.
	bool present = false;
};

// Finds the nearest module with the given plugin/model slug by walking
// outward from `self` in one direction (right = true, else left) through
// the contiguous row of adjacent modules -- so the link works with other
// modules (Stellar, etc.) sitting in between, not just immediate
// neighbors. Stops at the first gap in the row. Also identical in both
// repos' copies of this file.
inline rack::engine::Module* intelLinkFind(rack::engine::Module* self, bool right, const char* slug) {
	rack::engine::Module* m = right ? self->rightExpander.module : self->leftExpander.module;
	for (int hops = 0; m && hops < 32; hops++) {
		if (m->model && m->model->plugin && m->model->plugin->slug == slug && m->model->slug == slug)
			return m;
		m = right ? m->rightExpander.module : m->leftExpander.module;
	}
	return nullptr;
}
