"""
Spaces Command panel layout v2. Computed from real component sizes,
asserted to fit the fixed 128.5mm Rack panel height before anything is
written. Changes from v1 (per explicit feedback):
  - Section titles moved to vertical text OUTSIDE the content boxes,
    reclaiming horizontal space inside each row.
  - MELO/ARTI/TIME/NAVY randomize buttons grouped together, top-right
    of PATTERN, all sized the same (small squares).
  - ROOT/SCALE upgraded to the same knob size as DENS/SWING (was
    Trimpot, mismatched).
  - FEEL knobs redistributed evenly now that dice buttons moved out;
    alternating knobs get a subtle colored accent ring baked into the
    panel art so it doesn't read as a flat wall of identical knobs.
  - Crossfader gets a real horizontal track (paired with a custom
    ParamWidget in the module C++, not a knob) with a two-tone
    amber->cyan fill so the track itself shows the A/B blend visually.
  - Scene A/B focus buttons and every button on the panel (dice, wave
    select, mode toggles) are square-footprint zones, paired with
    LEDBezel/LEDBezelLight components in the widget code so the whole
    button face lights up, not a separate dot beside it.
"""
from gen_panel import text_to_path
import json

HP = 5.08
PANEL_H = 128.5

R = {
    "knob":       30 / 2 / (75/25.4),
    "bezel":      2.6,     # square LEDBezel button half-size
    "port":       26 / 2 / (75/25.4),
    "light":      1.0,
    "fader_half": 13.0,    # generous travel per feedback -- nicer, more usable fader caps
    "track_half": 3.0,     # crossfader track half-height
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

TITLE_PAD = 3.0
TITLE_GAP = 1.2
LABEL_GAP = 1.2
LABEL_H = 1.8
BOTTOM_PAD = 1.2
GAP = 3.5
SIDE_TITLE_W = 7.0

def row_height(r, extra=0.0):
    return TITLE_GAP + extra + r*2 + LABEL_GAP + LABEL_H + BOTTOM_PAD

STEP_PITCH = 13.5
steps_w_needed = STEP_PITCH * 8 + 30
VOICE_PITCH = 13.0
voice_w_needed = VOICE_PITCH * 9 + 10
two_voice_w = voice_w_needed * 2 + 4
macrokey_w_needed = 15.0 * 11

content_w_needed = max(steps_w_needed, two_voice_w, macrokey_w_needed)
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
    p, w = text_to_path(title, 0, 0, 2.6, tracking=1.1, anchor="middle")
    add(f'<g fill="{color}" transform="translate({cx},{cy}) rotate(-90)">{p}</g>')

y = TITLE_H

steps_h = row_height(R["fader_half"], extra=R["light"]*2 + 0.8)
section("steps", x0, y, full_w, steps_h, "PATTERN")
y += steps_h + GAP

xfader_h = row_height(R["bezel"])
section("crossfader", x0, y, full_w, xfader_h, "SCENE")
y += xfader_h + GAP

row_mk_h = row_height(R["knob"])
macro_w = full_w * 0.64
key_w = full_w - macro_w - GAP
section("macro", x0, y, macro_w, row_mk_h, "FEEL")
section("key", x0 + macro_w + GAP, y, key_w, row_mk_h, "KEY")
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

def micro(text, x, y, color=MICRO, size=1.7):
    p, _ = txt(text, x, y, size, color, anchor="middle")
    add(p)

layout = {"steps": [], "crossfader": {}, "macro": [], "mode": [], "key": [],
          "voice1": [], "voice2": [], "io": [], "randomize": []}

def grid_x(name, n, i, pad=6.0):
    x, sy, w, h, _ = sections[name]
    usable = w - 2*pad
    step = usable / max(1, n-1) if n > 1 else 0
    return x + pad + step*i if n > 1 else x + w/2

sx, sy, sw, sh, _ = sections["steps"]
ctrl_top = sy + TITLE_GAP
light_y = ctrl_top + R["light"]
fader_y = light_y + R["light"] + 0.8 + R["fader_half"]
label_y = fader_y + R["fader_half"] + LABEL_GAP + LABEL_H

CLUSTER_W = 22.0
faders_w = sw - CLUSTER_W
fader_pad = 8.0
for i in range(8):
    step_x = fader_pad + (faders_w - 2*fader_pad) / 7 * i
    cx = sx + step_x
    micro(str(i+1), cx, label_y)
    layout["steps"].append({"x": round(cx,2), "light_y": round(light_y,2), "fader_y": round(fader_y,2)})

divider_x = sx + faders_w + 4.0
add(f'<line x1="{divider_x}" y1="{sy+5}" x2="{divider_x}" y2="{sy+sh-5}" stroke="{BORDER}" stroke-width="0.3" stroke-opacity="0.5"/>')
cluster_x0 = sx + faders_w + 4.0
cluster_w = sw - faders_w - 4.0
rand_names = ["MELO", "ARTI", "TIME", "NAVY"]
rand_params = ["MELO_PARAM", "DICE_ARTI", "DICE_TIME", "DICE_NAVY"]
rx = [cluster_x0 + cluster_w*0.32, cluster_x0 + cluster_w*0.68]
ry = [fader_y - 6.0, fader_y + 6.0]
positions = [(rx[0], ry[0]), (rx[1], ry[0]), (rx[0], ry[1]), (rx[1], ry[1])]
for (px, py), nm, pnm in zip(positions, rand_names, rand_params):
    micro(nm, px, py + R["bezel"] + LABEL_GAP + 1.0, size=1.4)
    layout["randomize"].append({"x": round(px,2), "y": round(py,2), "param": pnm, "name": nm})
p, _ = txt("randomize", cluster_x0 + cluster_w/2, sy + 5.0, 1.4, TEXT_DIM, anchor="middle")
add(p)

cx0, cy0, cw0, ch0, _ = sections["crossfader"]
ctrl_y = cy0 + TITLE_GAP + R["bezel"]
label_y_xf = ctrl_y + R["bezel"] + LABEL_GAP + LABEL_H

xfader_zone_w = cw0 * 0.60
a_x = cx0 + 6
b_x = cx0 + xfader_zone_w - 6
track_x0 = a_x + R["bezel"] + 4
track_x1 = b_x - R["bezel"] - 4
grad_id = "xfgrad"
svg.insert(3, f'<linearGradient id="{grad_id}" x1="0" y1="0" x2="1" y2="0"><stop offset="0%" stop-color="{AMBER}" stop-opacity="0.35"/><stop offset="100%" stop-color="{CYAN}" stop-opacity="0.35"/></linearGradient>')
add(f'<rect x="{track_x0}" y="{ctrl_y-R["track_half"]}" width="{track_x1-track_x0}" height="{R["track_half"]*2}" rx="{R["track_half"]}" fill="url(#{grad_id})"/>')
add(f'<rect x="{track_x0}" y="{ctrl_y-R["track_half"]}" width="{track_x1-track_x0}" height="{R["track_half"]*2}" rx="{R["track_half"]}" fill="none" stroke="{TEXT_DIM}" stroke-width="0.3"/>')
micro("A", a_x, label_y_xf, color=AMBER)
micro("B", b_x, label_y_xf, color=CYAN)
micro("MORPH", (track_x0+track_x1)/2, label_y_xf)
layout["crossfader"] = {"a_x": round(a_x,2), "b_x": round(b_x,2), "track_x0": round(track_x0,2),
                          "track_x1": round(track_x1,2), "y": round(ctrl_y,2)}

mode_names = ["LATCH", "ARP-SEQ", "POLY", "FREEZE", "ROUTING"]
mode_zone_x0 = cx0 + xfader_zone_w + 4
mode_zone_w = cw0 - xfader_zone_w - 4
for i, nm in enumerate(mode_names):
    mx = mode_zone_x0 + (mode_zone_w / (len(mode_names)+1)) * (i+1)
    micro(nm, mx, label_y_xf, size=1.5)
    layout["mode"].append({"x": round(mx,2), "y": round(ctrl_y,2), "name": nm})

mx0, my0, mw0, mh0, _ = sections["macro"]
ctrl_top = my0 + TITLE_GAP
knob_y = ctrl_top + R["knob"]
label_y_m = knob_y + R["knob"] + LABEL_GAP + LABEL_H
macro_names = ["REST","LEGATO","RATE","ENTROPY","HARMONY","CHAOS","OCTAVES"]
macro_params = ["REST_PARAM","LEGATO_PARAM","RATE_PARAM","ENTROPY_PARAM","HARMONY_PARAM","CHAOS_PARAM","OCTAVES_PARAM"]
n = 7
for i, (nm, pnm) in enumerate(zip(macro_names, macro_params)):
    cx = grid_x("macro", n, i, pad=8.0)
    ring_color = ACCENT_RING_COLORS[i]
    if ring_color:
        add(f'<circle cx="{cx}" cy="{knob_y}" r="{R["knob"]+1.0}" fill="none" stroke="{ring_color}" stroke-width="0.5" opacity="0.55"/>')
    micro(nm, cx, label_y_m)
    layout["macro"].append({"x": round(cx,2), "y": round(knob_y,2), "param": pnm})

kx0, ky0, kw0, kh0, _ = sections["key"]
ctrl_top = ky0 + TITLE_GAP
knob_y_k = ctrl_top + R["knob"]
label_y_k = knob_y_k + R["knob"] + LABEL_GAP + LABEL_H
key_names = ["ROOT","SCALE","DENS","SWING"]
key_params = ["ROOT_KEY_PARAM","SCALE_TYPE_PARAM","DENSITY_PARAM","SWING_PARAM"]
for i, (nm, pnm) in enumerate(zip(key_names, key_params)):
    cx = grid_x("key", 4, i, pad=8.0)
    micro(nm, cx, label_y_k)
    layout["key"].append({"x": round(cx,2), "y": round(knob_y_k,2), "param": pnm})

def voice_layout(name, prefix, accent):
    vx, vy, vw, vh, _ = sections[name]
    ctrl_top = vy + TITLE_GAP
    knob_y_v = ctrl_top + R["knob"]
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
        micro(nm, cx, label_y_v, size=1.4 if kind == "wave" else 1.7)
        out.append({"x": round(cx,2), "y": round(knob_y_v,2), "param": pnm, "kind": kind})
    return out

layout["voice1"] = voice_layout("voice1", "VOICE1", AMBER)
layout["voice2"] = voice_layout("voice2", "VOICE2", CYAN)

iox, ioy, iow, ioh, _ = sections["io"]
ctrl_top = ioy + TITLE_GAP
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
