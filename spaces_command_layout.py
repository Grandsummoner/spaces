"""
Spaces Command panel layout v4. Computed from real component sizes,
asserted to fit the fixed 128.5mm Rack panel height before anything is
written. Changes from v3 (per explicit feedback):
  - Row order (top to bottom): I/O, FEEL+CONTROLS, VOICE1+VOICE2,
    SCENE+KEY, PATTERN -- inverted from v3's PATTERN-first order.
  - LATCH/ARP-SEQ/POLY/FREEZE/ROUTING split out of SCENE into their own
    "CONTROLS" group, paired alongside FEEL.
  - SCENE now holds only A/B focus + crossfader (mode toggles moved out),
    paired alongside KEY.
  - Crossfader track is 50% thinner.
  - Every row now guarantees a real minimum top/bottom margin (not just
    symmetric small padding) so nothing touches the box border.
  - Voice wave buttons get 4 distinct colors (Red/Yellow/Green/Blue),
    same 4 repeated for both voices.
  - Scene A/B buttons get their letter rendered INSIDE the square via a
    custom NanoVG text overlay in the module C++ (panel SVG text can't
    render on top of a runtime component correctly, so this is done at
    the widget level, not baked into the panel art).
"""
from gen_panel import text_to_path
import json

HP = 5.08
PANEL_H = 128.5

R = {
    "knob":       (30 / 2 / (75/25.4)) * 0.85,   # 85% of stock size per explicit request
    "knob_key":   (30 / 2 / (75/25.4)) * 0.85 * 0.72,  # KEY row needs smaller-than-standard knobs to fit
    "knob_voice": (30 / 2 / (75/25.4)) * 0.85 * 0.85,  # matches SmallKnobVoice's actual render scale
    "bezel":      2.6,
    "port":       26 / 2 / (75/25.4),
    "light":      1.0,
    "fader_half": 12.0,
    "track_half": 1.05,     # another 30% thinner per explicit request (was 1.5)
}

BG, BG2 = "#F4F1EA", "#E9E4D8"
BORDER = "#8A7F6A"
TEXT_DIM = "#6B6255"
TEXT_BRIGHT = "#2A2620"
MICRO = "#5C5347"
SLOT = "#DDD6C6"
AMBER = "#B8720A"
CYAN = "#0E7A8C"
ACCENT_RING_COLORS = [AMBER, CYAN, "#6B7A3A", None, None, "#8A5A9A", None]

LABEL_GAP = 1.2
LABEL_H = 1.8
GAP = 2.5
SIDE_TITLE_W = 7.0
MIN_MARGIN = 2.3   # guaranteed clearance above/below content in every box

def row_height(r, extra=0.0):
    """Box height = guaranteed top margin + content block + guaranteed
    bottom margin. Content block = [control (2r, plus any extra like a
    light above it) + gap + label]."""
    block_h = extra + 2*r + LABEL_GAP + LABEL_H
    return block_h + 2*MIN_MARGIN

def centered_y(box_y, box_h, r, extra=0.0):
    block_h = extra + 2*r + LABEL_GAP + LABEL_H
    block_top = box_y + (box_h - block_h) / 2
    return block_top + extra + r

STEP_PITCH = 13.5
steps_w_needed = STEP_PITCH * 8 + 30
VOICE_PITCH = 13.0
voice_w_needed = VOICE_PITCH * 9 + 10
two_voice_w = voice_w_needed * 2 + 4
feelctrl_w_needed = 15.0 * 12  # 7 feel knobs + 5 controls buttons, generous

content_w_needed = max(steps_w_needed, two_voice_w, feelctrl_w_needed)
x0 = 6.0
PANEL_W = content_w_needed + 2*x0
WIDTH_HP = int(-(-PANEL_W // HP))
PANEL_W = WIDTH_HP * HP
full_w = PANEL_W - 2*x0

svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PANEL_W} {PANEL_H}" width="{PANEL_W}mm" height="{PANEL_H}mm">']
svg.append(f'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="{BG}"/><stop offset="100%" stop-color="{BG2}"/></linearGradient></defs>')
svg.append(f'<rect x="0" y="0" width="{PANEL_W}" height="{PANEL_H}" fill="url(#bg)"/>')
svg.append(f'<rect x="0.5" y="0.5" width="{PANEL_W-1}" height="{PANEL_H-1}" fill="none" stroke="{BORDER}" stroke-width="0.4" opacity="0.6"/>')

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
    sections[name] = (x + SIDE_TITLE_W, y, w - SIDE_TITLE_W, h, title)
    add(f'<rect x="{x+SIDE_TITLE_W}" y="{y}" width="{w-SIDE_TITLE_W}" height="{h}" rx="1.2" fill="{SLOT}" opacity="0.9" stroke="{BORDER}" stroke-width="0.3" stroke-opacity="0.7"/>')
    cx, cy = x + SIDE_TITLE_W/2 + 1.0, y + h/2
    # The rotated title's rendered WIDTH becomes its VERTICAL extent once
    # rotated -90deg. A fixed font size let long words like "CONTROLS" or
    # "VOICE 2" render taller than short rows, poking past the row's own
    # top/bottom border into neighboring rows. Scale the font down so the
    # rotated text always fits within (row height - margin).
    max_size = 2.6
    available = h - 3.0  # keep clear of the row's own top/bottom edge
    _, measured_w = text_to_path(title, 0, 0, max_size, tracking=1.1, anchor="middle")
    size = max_size if measured_w <= available else max(1.5, max_size * available / measured_w)
    p, w = text_to_path(title, 0, 0, size, tracking=1.1, anchor="middle")
    add(f'<g fill="{color}" transform="translate({cx},{cy}) rotate(-90)">{p}</g>')

y = TITLE_H

# ---- Row order: I/O, FEEL+CONTROLS, VOICE1+VOICE2, SCENE+KEY, PATTERN ----
io_h = row_height(R["port"])
section("io", x0, y, full_w, io_h, "I/O")
y += io_h + GAP

row_fc_h = row_height(R["knob"])
feel_w = full_w * 0.60
ctrl_w = full_w - feel_w - GAP
section("macro", x0, y, feel_w, row_fc_h, "FEEL")
section("controls", x0 + feel_w + GAP, y, ctrl_w, row_fc_h, "CONTROLS")
y += row_fc_h + GAP

row_v_h = row_height(R["knob"])
voice_w = (full_w - GAP) / 2
section("voice1", x0, y, voice_w, row_v_h, "VOICE 1", color=AMBER)
section("voice2", x0 + voice_w + GAP, y, voice_w, row_v_h, "VOICE 2", color=CYAN)
y += row_v_h + GAP

row_sk_h = row_height(R["bezel"])
scene_w = full_w * 0.62
key_w = full_w - scene_w - GAP
section("crossfader", x0, y, scene_w, row_sk_h, "SCENE")
section("key", x0 + scene_w + GAP, y, key_w, row_sk_h, "KEY")
y += row_sk_h + GAP

steps_h = row_height(R["fader_half"], extra=R["light"]*2 + 0.8)
section("steps", x0, y, full_w, steps_h, "PATTERN")
y += steps_h

margin = PANEL_H - y
print(f"PANEL {PANEL_W:.1f}mm ({WIDTH_HP}HP) x {PANEL_H}mm -- content ends {y:.1f}mm, margin {margin:.1f}mm")
assert margin >= 3, f"OVERFLOW by {-margin:.1f}mm"

def micro(text, x, y, color=MICRO, size=1.7):
    p, _ = txt(text, x, y, size, color, anchor="middle")
    add(p)

layout = {"steps": [], "crossfader": {}, "controls": [], "key": [], "macro": [],
          "voice1": [], "voice2": [], "io": [], "randomize": []}

def grid_x(name, n, i, pad=6.0):
    x, sy, w, h, _ = sections[name]
    usable = w - 2*pad
    step = usable / max(1, n-1) if n > 1 else 0
    return x + pad + step*i if n > 1 else x + w/2

# ---- I/O ----
iox, ioy, iow, ioh, _ = sections["io"]
port_y = centered_y(ioy, ioh, R["port"])
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

# ---- FEEL ----
mx0, my0, mw0, mh0, _ = sections["macro"]
knob_y = centered_y(my0, mh0, R["knob"])
label_y_m = knob_y + R["knob"] + LABEL_GAP + LABEL_H
macro_names = ["REST","LEGATO","RATE","ENTROPY","HARMONY","CHAOS","OCTAVES"]
macro_params = ["REST_PARAM","LEGATO_PARAM","RATE_PARAM","ENTROPY_PARAM","HARMONY_PARAM","CHAOS_PARAM","OCTAVES_PARAM"]
n = 7
for i, (nm, pnm) in enumerate(zip(macro_names, macro_params)):
    cx = grid_x("macro", n, i, pad=8.0)
    ring_color = ACCENT_RING_COLORS[i]
    # every knob gets a neutral ring so all 7 read as consistently present;
    # accented ones additionally get a colored ring instead of the neutral
    # one, as a highlight, rather than being the ONLY knobs with any ring
    if ring_color:
        add(f'<circle cx="{cx}" cy="{knob_y}" r="{R["knob"]+1.0}" fill="none" stroke="{ring_color}" stroke-width="0.5" opacity="0.55"/>')
    else:
        add(f'<circle cx="{cx}" cy="{knob_y}" r="{R["knob"]+1.0}" fill="none" stroke="{BORDER}" stroke-width="0.3" opacity="0.35"/>')
    micro(nm, cx, label_y_m)
    layout["macro"].append({"x": round(cx,2), "y": round(knob_y,2), "param": pnm})

# ---- CONTROLS (mode toggles, split out of SCENE) ----
ctx0, cty0, ctw0, cth0, _ = sections["controls"]
ctrl_y = centered_y(cty0, cth0, R["bezel"])
label_y_ctrl = ctrl_y + R["bezel"] + LABEL_GAP + LABEL_H
mode_names = ["LATCH", "ARP-SEQ", "POLY", "FREEZE", "ROUTING"]
mode_param_map = {"LATCH":"LATCH_PARAM","ARP-SEQ":"ARPSEQ_PARAM","POLY":"POLY_PARAM","FREEZE":"FREEZE_PARAM","ROUTING":"ROUTING_PARAM"}
mode_light_map = {"LATCH":"LATCH_LIGHT","ARP-SEQ":"ARPSEQ_LIGHT","POLY":"POLY_LIGHT","FREEZE":"FREEZE_LIGHT","ROUTING":"ROUTING_LIGHT"}
for i, nm in enumerate(mode_names):
    cx = grid_x("controls", 5, i, pad=7.0)
    micro(nm, cx, label_y_ctrl, size=1.4)
    layout["controls"].append({"x": round(cx,2), "y": round(ctrl_y,2), "param": mode_param_map[nm], "light": mode_light_map[nm], "name": nm})

# ---- VOICE 1 / VOICE 2 ----
def voice_layout(name, prefix, accent):
    vx, vy, vw, vh, _ = sections[name]
    knob_y_v = centered_y(vy, vh, R["knob"])
    label_y_v = knob_y_v + R["knob"] + LABEL_GAP + LABEL_H
    out = []
    items = [("AN","wave"), ("FM","wave"), ("SS","wave"), ("PL","wave"),
             ("ATK","knob"), ("DEC","knob"), ("SUS","knob"), ("REL","knob"), ("TIMBRE","knob")]
    n = len(items)
    pad = R["knob"] + 1.5
    usable = vw - 2*pad
    for i, (nm, kind) in enumerate(items):
        cx = vx + pad + (usable / (n-1)) * i
        pnm = f"{prefix}_WAVE_{nm}" if kind == "wave" else \
              f"{prefix}_{'ATTACK' if nm=='ATK' else 'DECAY' if nm=='DEC' else 'SUSTAIN' if nm=='SUS' else 'RELEASE' if nm=='REL' else 'TIMBRE'}_PARAM"
        if kind == "knob":
            add(f'<circle cx="{cx}" cy="{knob_y_v}" r="{R["knob_voice"]+1.0}" fill="none" stroke="{BORDER}" stroke-width="0.3" opacity="0.35"/>')
        micro(nm, cx, label_y_v, size=1.4 if kind == "wave" else 1.7)
        out.append({"x": round(cx,2), "y": round(knob_y_v,2), "param": pnm, "kind": kind})
    return out

layout["voice1"] = voice_layout("voice1", "VOICE1", AMBER)
layout["voice2"] = voice_layout("voice2", "VOICE2", CYAN)

# ---- SCENE: A/B focus + crossfader only (mode toggles moved to CONTROLS) ----
cx0, cy0, cw0, ch0, _ = sections["crossfader"]
ctrl_y2 = cy0 + ch0 / 2   # no label reserved in this row anymore -- just center in the full box
a_x = cx0 + 7
b_x = cx0 + cw0 - 7
track_x0 = a_x + R["bezel"] + 5
track_x1 = b_x - R["bezel"] - 5
grad_id = "xfgrad"
svg.insert(3, f'<linearGradient id="{grad_id}" x1="0" y1="0" x2="1" y2="0"><stop offset="0%" stop-color="{AMBER}" stop-opacity="0.30"/><stop offset="100%" stop-color="{CYAN}" stop-opacity="0.30"/></linearGradient>')
add(f'<rect x="{track_x0}" y="{ctrl_y2-R["track_half"]}" width="{track_x1-track_x0}" height="{R["track_half"]*2}" rx="{R["track_half"]}" fill="url(#{grad_id})"/>')
add(f'<rect x="{track_x0}" y="{ctrl_y2-R["track_half"]}" width="{track_x1-track_x0}" height="{R["track_half"]*2}" rx="{R["track_half"]}" fill="none" stroke="{TEXT_DIM}" stroke-width="0.3"/>')
layout["crossfader"] = {"a_x": round(a_x,2), "b_x": round(b_x,2), "track_x0": round(track_x0,2),
                          "track_x1": round(track_x1,2), "y": round(ctrl_y2,2)}

# ---- KEY ----
kx0, ky0, kw0, kh0, _ = sections["key"]
knob_y_k = centered_y(ky0, kh0, R["knob_key"])
label_y_k = knob_y_k + R["knob_key"] + LABEL_GAP + LABEL_H
key_names = ["ROOT","SCALE","DENS","SWING"]
key_params = ["ROOT_KEY_PARAM","SCALE_TYPE_PARAM","DENSITY_PARAM","SWING_PARAM"]
for i, (nm, pnm) in enumerate(zip(key_names, key_params)):
    cx = grid_x("key", 4, i, pad=6.0)
    add(f'<circle cx="{cx}" cy="{knob_y_k}" r="{R["knob_key"]+1.0}" fill="none" stroke="{BORDER}" stroke-width="0.3" opacity="0.35"/>')
    micro(nm, cx, label_y_k)
    layout["key"].append({"x": round(cx,2), "y": round(knob_y_k,2), "param": pnm, "size": "small"})

# ---- PATTERN: 8 faders (2/3 width) + randomize cluster (1/3 width) ----
sx, sy, sw, sh, _ = sections["steps"]
light_extra = R["light"]*2 + 0.8
fader_y = centered_y(sy, sh, R["fader_half"], extra=light_extra)
light_y = fader_y - R["fader_half"] - 0.8 - R["light"]
label_y = fader_y + R["fader_half"] + LABEL_GAP + LABEL_H

faders_w = sw * (2.0/3.0)
fader_pad = 10.0
for i in range(8):
    step_x = fader_pad + (faders_w - 2*fader_pad) / 7 * i
    cx = sx + step_x
    add(f'<rect x="{cx-1.3}" y="{fader_y-R["fader_half"]}" width="2.6" height="{R["fader_half"]*2}" rx="1.3" fill="{BG2}" stroke="{BORDER}" stroke-width="0.25" stroke-opacity="0.6"/>')
    micro(str(i+1), cx, label_y)
    layout["steps"].append({"x": round(cx,2), "light_y": round(light_y,2), "fader_y": round(fader_y,2),
                              "track_y0": round(fader_y-R["fader_half"],2), "track_y1": round(fader_y+R["fader_half"],2)})

divider_x = sx + faders_w + 4.0
add(f'<line x1="{divider_x}" y1="{sy+5}" x2="{divider_x}" y2="{sy+sh-5}" stroke="{BORDER}" stroke-width="0.3" stroke-opacity="0.5"/>')
cluster_x0 = sx + faders_w + 4.0
cluster_w = sw - faders_w - 4.0

R["bezel_big"] = 5.5   # dice buttons
R["nudge"] = 1.8       # small -/+ nudge buttons

rand_names = ["MELO", "ARTI", "TIME", "NAVY"]
rand_params = ["MELO_PARAM", "DICE_ARTI", "DICE_TIME", "DICE_NAVY"]
nudge_params = [("MELO_NUDGE_DOWN","MELO_NUDGE_UP"), ("ARTI_NUDGE_DOWN","ARTI_NUDGE_UP"),
                 ("TIME_NUDGE_DOWN","TIME_NUDGE_UP"), ("NAVY_NUDGE_DOWN","NAVY_NUDGE_UP")]

# ---- vertical stack, computed top-down, asserted to fit within sh ----
box_pad = 3.0
title_h = 4.5
gap1 = 1.5   # title -> dice button
gap2 = 1.8   # dice button -> nudge row
gap3 = 1.2   # nudge row -> label
label_h_dice = 1.8

dice_d = R["bezel_big"] * 2
nudge_row_h = R["nudge"] * 2
stack_h = title_h + gap1 + dice_d + gap2 + nudge_row_h + gap3 + label_h_dice
box_h = stack_h + 2*box_pad
assert box_h <= sh, f"DICE box overflows PATTERN row: needs {box_h:.1f}mm, row has {sh:.1f}mm"

# ---- horizontal: 4 equal columns, computed to fit within cluster_w ----
nudge_pair_w = R["nudge"]*4 + 1.0   # two nudge buttons + gap between them
col_w = max(dice_d, nudge_pair_w) + 3.0
col_gap = 3.0
row_w = col_w*4 + col_gap*3
box_w = row_w + 2*box_pad
assert box_w <= cluster_w, f"DICE box overflows horizontally: needs {box_w:.1f}mm, zone has {cluster_w:.1f}mm"

box_x0 = cluster_x0 + (cluster_w - box_w)/2
box_y0 = sy + (sh - box_h)/2   # center the whole DICE box within the PATTERN row, like every other row

add(f'<rect x="{box_x0}" y="{box_y0}" width="{box_w}" height="{box_h}" rx="1.5" fill="{SLOT}" opacity="0.6" stroke="{BORDER}" stroke-width="0.3" stroke-opacity="0.6"/>')
p, _ = txt("DICE", box_x0 + box_w/2, box_y0 + title_h - 1.0, 2.2, TEXT_DIM, anchor="middle")
add(p)

dice_y = box_y0 + title_h + gap1 + R["bezel_big"]
nudge_y = dice_y + R["bezel_big"] + gap2 + R["nudge"]
label_y_dice = nudge_y + R["nudge"] + gap3 + label_h_dice

row_x0 = box_x0 + box_pad
for i, (nm, pnm) in enumerate(zip(rand_names, rand_params)):
    col_cx = row_x0 + col_w/2 + i*(col_w + col_gap)
    micro(nm, col_cx, label_y_dice, size=1.5)
    layout["randomize"].append({"x": round(col_cx,2), "y": round(dice_y,2), "param": pnm, "name": nm})
    down_x = col_cx - (R["nudge"]*2 + 0.5)
    up_x = col_cx + (R["nudge"]*2 + 0.5)
    layout["randomize"][-1]["nudge_down"] = {"x": round(down_x,2), "y": round(nudge_y,2), "param": nudge_params[i][0]}
    layout["randomize"][-1]["nudge_up"] = {"x": round(up_x,2), "y": round(nudge_y,2), "param": nudge_params[i][1]}
svg.append('</svg>')
open("res/SpacesCommand.svg", "w").write("\n".join(svg))
json.dump(layout, open("command_layout.json","w"), indent=2)
print("controls placed")
