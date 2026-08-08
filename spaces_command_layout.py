"""
Navy-Command panel layout. Computed from real component sizes, asserted
to fit the fixed 128.5mm Rack panel height before anything is written.
Rows that would otherwise blow the height budget are merged side-by-side
(Voice1+Voice2 in one row, Macro+Key in one row) rather than cramped.
"""
from gen_panel import text_to_path
import json

HP = 5.08
PANEL_H = 128.5

R = {
    "knob":      30 / 2 / (75/25.4),   # RoundBlackKnob, used everywhere for consistency
    "trimpot":   17 / 2 / (75/25.4),
    "ckd6":      22 / 2 / (75/25.4),
    "port":      26 / 2 / (75/25.4),
    "light":     1.0,
    "btn_sm":    2.2,    # dice / wave-select buttons -- smaller than a knob on purpose
    "toggle":    2.3,    # mode toggle buttons (LATCH/ARP-SEQ/POLY/FREEZE)
    "fader_half": 11.0,  # vertical step-fader half-travel (22mm total travel)
    "xfader_half": 3.0,  # crossfader body half-height
}

BG, BG2 = "#F4F1EA", "#E9E4D8"       # warm off-white gradient
BORDER = "#8A7F6A"                    # muted brown-grey border
TEXT_DIM = "#6B6255"
TEXT_BRIGHT = "#2A2620"               # near-black for titles
MICRO = "#5C5347"
SLOT = "#DDD6C6"                      # section box fill, slightly darker than bg
AMBER = "#B8720A"                     # darker amber, readable on light bg
CYAN = "#0E7A8C"                      # darker teal-cyan, readable on light bg

TITLE_PAD = 3.0
TITLE_GAP = 1.2
LABEL_GAP = 1.2
LABEL_H = 1.8
BOTTOM_PAD = 1.2
GAP = 1.5

def row_height(r, extra=0.0):
    return TITLE_PAD + TITLE_GAP + extra + r*2 + LABEL_GAP + LABEL_H + BOTTOM_PAD

# ---- compute required width first (drives HP choice) ----
# Steps: 8 faders, generous per-fader footprint
STEP_PITCH = 15.0
steps_w_needed = STEP_PITCH * 8 + 16  # + side margins + room for MELO button
# Voice: 4 wave buttons + 5 knobs = 9 controls, generous pitch
VOICE_PITCH = 13.0
voice_w_needed = VOICE_PITCH * 9 + 10
two_voice_w = voice_w_needed * 2 + 4
# Macro+Key: 7+3+4 = 14 controls total across the row
macrokey_w_needed = 13.0 * 14 + 12

content_w_needed = max(steps_w_needed, two_voice_w, macrokey_w_needed)
x0 = 6.0
PANEL_W = content_w_needed + 2*x0
WIDTH_HP = int(-(-PANEL_W // HP))  # ceil to whole HP
PANEL_W = WIDTH_HP * HP
full_w = PANEL_W - 2*x0

svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PANEL_W} {PANEL_H}" width="{PANEL_W}mm" height="{PANEL_H}mm">']
svg.append(f'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="{BG}"/><stop offset="100%" stop-color="{BG2}"/></linearGradient></defs>')
svg.append(f'<rect x="0" y="0" width="{PANEL_W}" height="{PANEL_H}" fill="url(#bg)"/>')
svg.append(f'<rect x="0.5" y="0.5" width="{PANEL_W-1}" height="{PANEL_H-1}" fill="none" stroke="{BORDER}" stroke-width="0.4" opacity="0.6"/>')
# no corner screws (decided: not needed for a software module)

def txt(text, x, y, size, color, anchor="start", tracking=1.05):
    p, w = text_to_path(text, x, y, size, tracking=tracking, anchor=anchor)
    return f'<g fill="{color}">{p}</g>', w

def add(s): svg.append(s)

p, _ = txt("SPACES COMMAND", 8, 7.2, 4.4, TEXT_BRIGHT)
add(p)
p, _ = txt("arpeggiator + dual voice", 8, 10.4, 1.9, TEXT_DIM)
add(p)
TITLE_H = 12.5

sections = {}
def section(name, x, y, w, h, title, color=TEXT_DIM):
    sections[name] = (x, y, w, h, title)
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="1.2" fill="{SLOT}" opacity="0.9" stroke="{BORDER}" stroke-width="0.3" stroke-opacity="0.7"/>')
    p, _ = txt(title, x + 2.0, y + TITLE_PAD, 2.6, color)
    add(p)

y = TITLE_H

steps_h = row_height(R["fader_half"], extra=R["light"]*2 + 0.8)
section("steps", x0, y, full_w, steps_h, "PATTERN  (probability / degree)")
y += steps_h + GAP

xfader_h = row_height(R["ckd6"])
section("crossfader", x0, y, full_w, xfader_h, "SCENE")
y += xfader_h + GAP

row_mk_h = row_height(R["knob"])
macro_w = full_w * 0.68
key_w = full_w - macro_w - GAP
section("macro", x0, y, macro_w, row_mk_h, "FEEL")
section("key", x0 + macro_w + GAP, y, key_w, row_mk_h, "KEY / DENSITY")
y += row_mk_h + GAP

row_v_h = row_height(R["knob"])
voice_w = (full_w - GAP) / 2
section("voice1", x0, y, voice_w, row_v_h, "VOICE 1", color=AMBER)
section("voice2", x0 + voice_w + GAP, y, voice_w, row_v_h, "VOICE 2", color=CYAN)
y += row_v_h + GAP

io_h = row_height(R["port"])
section("io", x0, y, full_w, io_h, "I/O")
y += io_h

margin = PANEL_H - y
print(f"PANEL {PANEL_W:.1f}mm ({WIDTH_HP}HP) x {PANEL_H}mm -- content ends {y:.1f}mm, margin {margin:.1f}mm")
assert margin >= 3, f"OVERFLOW by {-margin:.1f}mm"

open("res/SpacesCommand.svg.tmp", "w").write("\n".join(svg) + "\n</svg>")
json.dump({"PANEL_W": PANEL_W, "WIDTH_HP": WIDTH_HP, "sections": sections, "y_end": y}, open("command_sections.json","w"), indent=2, default=str)

# =====================================================================
# CONTROL PLACEMENT
# =====================================================================
def micro(text, x, y, color=MICRO, size=1.7):
    p, _ = txt(text, x, y, size, color, anchor="middle")
    add(p)

layout = {"steps": [], "crossfader": {}, "macro": [], "mode": [], "key": [],
          "voice1": [], "voice2": [], "io": []}

def grid_x(name, n, i, pad=6.0):
    x, sy, w, h, _ = sections[name]
    usable = w - 2*pad
    step = usable / max(1, n-1) if n > 1 else 0
    return x + pad + step*i if n > 1 else x + w/2

# ---- STEPS: 8 vertical faders + lights + MELO dice button ----
sx, sy, sw, sh, _ = sections["steps"]
ctrl_top = sy + TITLE_PAD + TITLE_GAP
light_y = ctrl_top + R["light"]
fader_y = light_y + R["light"] + 0.8 + R["fader_half"]
label_y = fader_y + R["fader_half"] + LABEL_GAP + LABEL_H

MELO_ZONE_W = 20.0  # reserved on the right; faders are laid out in the remaining width only
faders_w = sw - MELO_ZONE_W
fader_pad = 10.0
for i in range(8):
    step_x = fader_pad + (faders_w - 2*fader_pad) / 7 * i
    cx = sx + step_x
    micro(str(i+1), cx, label_y)
    layout["steps"].append({"x": round(cx,2), "light_y": round(light_y,2), "fader_y": round(fader_y,2)})

# MELO dice button: own reserved zone, anchored to the SAME vertical level
# as the faders (not the section's empty mid-height), with a divider so it
# reads as its own zone rather than a stray dot floating in empty space.
divider_x = sx + faders_w + 4.0
add(f'<line x1="{divider_x}" y1="{sy+6}" x2="{divider_x}" y2="{sy+sh-6}" stroke="{BORDER}" stroke-width="0.3" stroke-opacity="0.5"/>')
melo_x = sx + faders_w + (MELO_ZONE_W - 4.0)/2 + 4.0
melo_y = fader_y  # same y as the fader knobs' vertical center, not row mid-height
add(f'<circle cx="{melo_x}" cy="{melo_y}" r="{R["btn_sm"]+0.8}" fill="none" stroke="{AMBER}" stroke-width="0.35" opacity="0.7"/>')
micro("MELO", melo_x, label_y)
micro("randomize", melo_x, sy + 6.5, color=TEXT_DIM, size=1.3)
layout["macro_dice_melo"] = {"x": round(melo_x,2), "y": round(melo_y,2)}

# ---- CROSSFADER row: A btn -- horizontal fader track -- B btn, then 4 mode toggles ----
cx0, cy0, cw0, ch0, _ = sections["crossfader"]
ctrl_top = cy0 + TITLE_PAD + TITLE_GAP
ctrl_y = ctrl_top + R["ckd6"]
label_y_xf = ctrl_y + R["ckd6"] + LABEL_GAP + LABEL_H

xfader_zone_w = cw0 * 0.62
a_x = cx0 + 6
b_x = cx0 + xfader_zone_w - 6
track_x0 = a_x + R["ckd6"] + 3
track_x1 = b_x - R["ckd6"] - 3
add(f'<circle cx="{a_x}" cy="{ctrl_y}" r="{R["ckd6"]}" fill="none" stroke="{AMBER}" stroke-width="0.3"/>')
micro("A", a_x, label_y_xf, color=AMBER)
add(f'<circle cx="{b_x}" cy="{ctrl_y}" r="{R["ckd6"]}" fill="none" stroke="{CYAN}" stroke-width="0.3"/>')
micro("B", b_x, label_y_xf, color=CYAN)
add(f'<rect x="{track_x0}" y="{ctrl_y-1.2}" width="{track_x1-track_x0}" height="2.4" rx="1.2" fill="none" stroke="{TEXT_DIM}" stroke-width="0.25"/>')
micro("MORPH", (track_x0+track_x1)/2, label_y_xf)
layout["crossfader"] = {"a_x": round(a_x,2), "b_x": round(b_x,2), "track_x0": round(track_x0,2),
                          "track_x1": round(track_x1,2), "y": round(ctrl_y,2)}

mode_names = ["LATCH", "ARP-SEQ", "POLY", "FREEZE", "ROUTING"]
mode_zone_x0 = cx0 + xfader_zone_w + 4
mode_zone_w = cw0 - xfader_zone_w - 4
for i, nm in enumerate(mode_names):
    mx = mode_zone_x0 + (mode_zone_w / (len(mode_names)+1)) * (i+1)
    add(f'<rect x="{mx-R["toggle"]}" y="{ctrl_y-R["toggle"]}" width="{R["toggle"]*2}" height="{R["toggle"]*2}" rx="0.5" fill="none" stroke="{TEXT_DIM}" stroke-width="0.3"/>')
    micro(nm, mx, label_y_xf, size=1.5)
    layout["mode"].append({"x": round(mx,2), "y": round(ctrl_y,2), "name": nm})

# ---- MACRO: 7 knobs + ARTI/TIME/NAVY dice inline ----
mx0, my0, mw0, mh0, _ = sections["macro"]
ctrl_top = my0 + TITLE_PAD + TITLE_GAP
knob_y = ctrl_top + R["knob"]
label_y_m = knob_y + R["knob"] + LABEL_GAP + LABEL_H
# interleaved: dice buttons sit next to the knob cluster they randomize (same row, same y)
macro_items = [
    ("REST", "knob", "REST_PARAM"), ("ARTI", "dice", "DICE_ARTI"), ("LEGATO", "knob", "LEGATO_PARAM"),
    ("RATE", "knob", "RATE_PARAM"), ("TIME", "dice", "DICE_TIME"),
    ("ENTROPY", "knob", "ENTROPY_PARAM"), ("HARMONY", "knob", "HARMONY_PARAM"), ("CHAOS", "knob", "CHAOS_PARAM"),
    ("NAVY", "dice", "DICE_NAVY"), ("OCTAVES", "knob", "OCTAVES_PARAM"),
]
n = len(macro_items)
for i, (nm, kind, pnm) in enumerate(macro_items):
    cx = grid_x("macro", n, i, pad=6.0)
    micro(nm, cx, label_y_m, size=1.5 if kind == "dice" else 1.7)
    layout["macro"].append({"x": round(cx,2), "y": round(knob_y,2), "param": pnm, "kind": kind})

# ---- KEY / DENSITY ----
kx0, ky0, kw0, kh0, _ = sections["key"]
ctrl_top = ky0 + TITLE_PAD + TITLE_GAP
knob_y_k = ctrl_top + R["knob"]
label_y_k = knob_y_k + R["knob"] + LABEL_GAP + LABEL_H
key_names = ["ROOT","SCALE","DENS","SWING"]
key_params = ["ROOT_KEY_PARAM","SCALE_TYPE_PARAM","DENSITY_PARAM","SWING_PARAM"]
for i, (nm, pnm) in enumerate(zip(key_names, key_params)):
    cx = grid_x("key", 4, i, pad=7.0)
    micro(nm, cx, label_y_k)
    layout["key"].append({"x": round(cx,2), "y": round(knob_y_k,2), "param": pnm})

# ---- VOICE 1 / VOICE 2: 4 wave buttons + ADSR + TIMBRE, one equally-spaced grid ----
def voice_layout(name, prefix, accent):
    vx, vy, vw, vh, _ = sections[name]
    ctrl_top = vy + TITLE_PAD + TITLE_GAP
    knob_y_v = ctrl_top + R["knob"]
    label_y_v = knob_y_v + R["knob"] + LABEL_GAP + LABEL_H
    out = []
    items = [("AN","wave"), ("FM","wave"), ("SS","wave"), ("PL","wave"),
             ("ATK","knob"), ("DEC","knob"), ("SUS","knob"), ("REL","knob"), ("TIMBRE","knob")]
    n = len(items)
    pad = R["knob"] + 1.5  # keep edge items clear of the section border
    usable = vw - 2*pad
    for i, (nm, kind) in enumerate(items):
        cx = vx + pad + (usable / (n-1)) * i
        if kind == "wave":
            add(f'<circle cx="{cx}" cy="{knob_y_v}" r="{R["btn_sm"]}" fill="none" stroke="{accent}" stroke-width="0.3"/>')
            pnm = f"{prefix}_WAVE_{nm}"
        else:
            pnm = f"{prefix}_{'ATTACK' if nm=='ATK' else 'DECAY' if nm=='DEC' else 'SUSTAIN' if nm=='SUS' else 'RELEASE' if nm=='REL' else 'TIMBRE'}_PARAM"
        micro(nm, cx, label_y_v, size=1.4 if kind == "wave" else 1.7)
        out.append({"x": round(cx,2), "y": round(knob_y_v,2), "param": pnm})
    return out

layout["voice1"] = voice_layout("voice1", "VOICE1", AMBER)
layout["voice2"] = voice_layout("voice2", "VOICE2", CYAN)

# ---- I/O: 4 in + 4 out, one uniform equally-spaced grid of 8 across the full row ----
iox, ioy, iow, ioh, _ = sections["io"]
ctrl_top = ioy + TITLE_PAD + TITLE_GAP
port_y = ctrl_top + R["port"]
label_y_io = port_y + R["port"] + LABEL_GAP + LABEL_H
io_names = ["V/OCT","GATE","VEL","CLOCK","VOICE 1","VOICE 2","MASTER L","MASTER R"]
io_params = ["VOCT_INPUT","GATE_INPUT","VELOCITY_INPUT","CLOCK_INPUT",
             "VOICE1_OUTPUT","VOICE2_OUTPUT","MASTER_L_OUTPUT","MASTER_R_OUTPUT"]
io_dirs = ["in","in","in","in","out","out","out","out"]
n_io = len(io_names)
pad_io = R["port"] + 2.0
usable_io = iow - 2*pad_io
for i, (nm, pnm, dr) in enumerate(zip(io_names, io_params, io_dirs)):
    cx = iox + pad_io + (usable_io / (n_io-1)) * i
    micro(nm, cx, label_y_io, size=1.5)
    layout["io"].append({"x": round(cx,2), "y": round(port_y,2), "param": pnm, "dir": dr})

svg.append('</svg>')
open("res/SpacesCommand.svg", "w").write("\n".join(svg))
json.dump(layout, open("command_layout.json","w"), indent=2)
print("controls placed")
