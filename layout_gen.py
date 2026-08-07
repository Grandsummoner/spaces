"""
Single source of truth for the Navy Arp 2 panel: section geometry AND
control placement, computed from real VCV component radii so nothing
can overlap. Regenerates res/NavyArp2.svg, layout_final.json (consumed
to write the widget C++), and asserts the layout fits the fixed 128.5mm
Eurorack 3U panel height before writing anything.
"""
from gen_panel import text_to_path
import json

HP = 5.08
WIDTH_HP = 32
PANEL_W = HP * WIDTH_HP  # 162.56mm
PANEL_H = 128.5

R = {
    "big_knob": 36 / 2 / (75/25.4),
    "knob":     30 / 2 / (75/25.4),
    "trimpot":  17 / 2 / (75/25.4),
    "ckd6":     22 / 2 / (75/25.4),
    "port":     26 / 2 / (75/25.4),
    "light":    1.0,
    "switch_h": 3.4,
}

BG, BG2 = "#0D1E36", "#030508"
BORDER = "#5D8AA8"
TEXT_DIM = "#9AA6B8"
TEXT_BRIGHT = "#E8ECF2"
MICRO = "#8C97AC"
SLOT = "#181C24"

svg = []
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PANEL_W} {PANEL_H}" width="{PANEL_W}mm" height="{PANEL_H}mm">')
svg.append(f'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="{BG}"/><stop offset="100%" stop-color="{BG2}"/></linearGradient></defs>')
svg.append(f'<rect x="0" y="0" width="{PANEL_W}" height="{PANEL_H}" fill="url(#bg)"/>')
svg.append(f'<rect x="0.5" y="0.5" width="{PANEL_W-1}" height="{PANEL_H-1}" fill="none" stroke="{BORDER}" stroke-width="0.4" opacity="0.6"/>')

SCREW_R = 1.6
SCREW_MARGIN = 6.0
for sx, sy in [(SCREW_MARGIN, SCREW_MARGIN), (PANEL_W - SCREW_MARGIN, SCREW_MARGIN),
               (SCREW_MARGIN, PANEL_H - SCREW_MARGIN), (PANEL_W - SCREW_MARGIN, PANEL_H - SCREW_MARGIN)]:
    svg.append(f'<circle cx="{sx}" cy="{sy}" r="{SCREW_R}" fill="#0a0a0c" stroke="{BORDER}" stroke-width="0.3"/>')

def txt(text, x, y, size, color, anchor="start", tracking=1.05):
    p, w = text_to_path(text, x, y, size, tracking=tracking, anchor=anchor)
    return f'<g fill="{color}">{p}</g>', w

def add(s): svg.append(s)

# ---- Title ----
p, _ = txt("NAVY ARP 2", 8, 7.4, 4.6, TEXT_BRIGHT)
add(p)
p, _ = txt("dual-scene arpeggiator", 8, 10.8, 2.0, TEXT_DIM)
add(p)
TITLE_H = 13.0

GAP = 1.5
x0 = 6.0
full_w = PANEL_W - 2 * x0

TITLE_PAD = 3.6   # box_top -> title baseline
TITLE_GAP = 1.5   # title -> main control top clearance
LABEL_GAP = 1.5   # control bottom -> label
LABEL_H = 2.0
BOTTOM_PAD = 1.5

def row_height(radius, extra_top=0.0):
    return TITLE_PAD + TITLE_GAP + extra_top + radius*2 + LABEL_GAP + LABEL_H + BOTTOM_PAD

sections = {}
def section(name, x, y, w, h, title):
    sections[name] = (x, y, w, h, title)
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="1.2" fill="{SLOT}" opacity="0.55" stroke="{BORDER}" stroke-width="0.25" stroke-opacity="0.5"/>')
    p, _ = txt(title, x + 2.2, y + TITLE_PAD, 2.8, TEXT_DIM)
    add(p)

y = TITLE_H

# STEPS row needs extra headroom for the light above the knob
steps_h = row_height(R["big_knob"], extra_top=R["light"]*2 + 0.8)
section("steps", x0, y, full_w, steps_h, "STEPS  (probability / degree)")
y += steps_h + GAP

row2_h = row_height(R["knob"])
macro_w = full_w * 0.58
scene_w = full_w - macro_w - GAP
section("macro", x0, y, macro_w, row2_h, "MACRO")
section("scene", x0 + macro_w + GAP, y, scene_w, row2_h, "SCENE / MODE")
y += row2_h + GAP

row3_h = row_height(R["knob"])
key_w = full_w * 0.36
disp_w = full_w - key_w - GAP
section("key", x0, y, key_w, row3_h, "KEY / DENSITY")
section("display", x0 + key_w + GAP, y, disp_w, row3_h, "DISPLAY")
p, _ = txt("OLED / LFO matrix - coming soon", x0 + key_w + GAP + 3, y + row3_h/2 + 1, 2.4, TEXT_DIM)
add(p)
y += row3_h + GAP

row4_h = row_height(R["switch_h"])
voice_w = (full_w - GAP) / 2
section("voice1", x0, y, voice_w, row4_h, "VOICE 1")
section("voice2", x0 + voice_w + GAP, y, voice_w, row4_h, "VOICE 2")
y += row4_h + GAP

io_h = row_height(R["port"])
section("io", x0, y, full_w, io_h, "I/O")
y += io_h

margin = PANEL_H - y
assert margin >= 2, f"Layout overflows panel: content ends at {y:.1f}mm, panel is {PANEL_H}mm (margin {margin:.1f}mm)"

def grid_x(section_name, n, i, pad=None):
    x, sy, w, h, _ = sections[section_name]
    if pad is None:
        pad = 8.0 if section_name == "io" else 4.0
    usable = w - 2*pad
    step = usable / max(1, n-1) if n > 1 else 0
    return x + pad + step*i if n > 1 else x + w/2

def micro(text, x, y):
    p, _ = txt(text, x, y, 1.8, MICRO, anchor="middle")
    add(p)

layout = {}

# STEPS
sx, sy, sw, sh, _ = sections["steps"]
ctrl_top = sy + TITLE_PAD + TITLE_GAP
light_y = ctrl_top + R["light"]
knob_y = light_y + R["light"] + 0.8 + R["big_knob"]
label_y = knob_y + R["big_knob"] + LABEL_GAP + LABEL_H
layout["steps"] = []
for i in range(8):
    cx = grid_x("steps", 8, i)
    micro(str(i+1), cx, label_y)
    layout["steps"].append((round(cx,2), round(light_y,2), round(knob_y,2)))

# MACRO
mx, my, mw, mh, _ = sections["macro"]
ctrl_top = my + TITLE_PAD + TITLE_GAP
knob_y = ctrl_top + R["knob"]
label_y = knob_y + R["knob"] + LABEL_GAP + LABEL_H
macro_names = ["REST","LEGATO","RATE","ENTRO","HARMO","CHAOS","OCTAVE"]
macro_params = ["REST_PARAM","LEGATO_PARAM","RATE_PARAM","ENTROPY_PARAM","HARMONY_PARAM","CHAOS_PARAM","OCTAVES_PARAM"]
layout["macro"] = []
for i, (nm, pnm) in enumerate(zip(macro_names, macro_params)):
    cx = grid_x("macro", 7, i)
    micro(nm, cx, label_y)
    layout["macro"].append((round(cx,2), round(knob_y,2), pnm))

# SCENE (aligned to macro row)
scene_names = ["A","B","MORPH","LATCH","FREEZE"]
layout["scene"] = []
for i, nm in enumerate(scene_names):
    cx = grid_x("scene", 5, i)
    micro(nm, cx, label_y)
    layout["scene"].append((round(cx,2), round(knob_y,2), nm))

# KEY
kx, ky, kw, kh, _ = sections["key"]
ctrl_top = ky + TITLE_PAD + TITLE_GAP
knob_y_key = ctrl_top + R["knob"]
label_y_key = knob_y_key + R["knob"] + LABEL_GAP + LABEL_H
key_names = ["ROOT","SCALE","DENS","SWING"]
key_params = ["ROOT_KEY_PARAM","SCALE_TYPE_PARAM","DENSITY_PARAM","SWING_PARAM"]
layout["key"] = []
for i, (nm, pnm) in enumerate(zip(key_names, key_params)):
    cx = grid_x("key", 4, i)
    micro(nm, cx, label_y_key)
    layout["key"].append((round(cx,2), round(knob_y_key,2), pnm))

# VOICE1 / VOICE2
def voice_layout(section_name, prefix):
    vx, vy, vw, vh, _ = sections[section_name]
    ctrl_top = vy + TITLE_PAD + TITLE_GAP
    ctrl_y = ctrl_top + R["switch_h"]
    label_y_v = ctrl_y + R["switch_h"] + LABEL_GAP + LABEL_H
    names = ["WAVE","ATK","DEC","SUS","REL","TIMBRE"]
    params = [f"{prefix}_WAVE_PARAM", f"{prefix}_ATTACK_PARAM", f"{prefix}_DECAY_PARAM",
              f"{prefix}_SUSTAIN_PARAM", f"{prefix}_RELEASE_PARAM", f"{prefix}_TIMBRE_PARAM"]
    out = []
    for i, (nm, pnm) in enumerate(zip(names, params)):
        cx = grid_x(section_name, 6, i)
        micro(nm, cx, label_y_v)
        out.append((round(cx,2), round(ctrl_y,2), pnm))
    return out

layout["voice1"] = voice_layout("voice1", "VOICE1")
layout["voice2"] = voice_layout("voice2", "VOICE2")

# IO
iox, ioy, iow, ioh, _ = sections["io"]
ctrl_top = ioy + TITLE_PAD + TITLE_GAP
port_y = ctrl_top + R["port"]
label_y_io = port_y + R["port"] + LABEL_GAP + LABEL_H
io_names = ["V/OCT","GATE","PITCH","GATE","AUD L","AUD R"]
io_params = ["VOCT_INPUT","GATE_INPUT","PITCH_OUTPUT","GATE_OUTPUT","AUDIO_L_OUTPUT","AUDIO_R_OUTPUT"]
layout["io"] = []
for i, (nm, pnm) in enumerate(zip(io_names, io_params)):
    cx = grid_x("io", 6, i)
    micro(nm, cx, label_y_io)
    layout["io"].append((round(cx,2), round(port_y,2), pnm))

svg.append('</svg>')
open("res/NavyArp2.svg", "w").write("\n".join(svg))
json.dump(layout, open("layout_final.json","w"), indent=2)
print(f"OK: panel {PANEL_W:.1f}x{PANEL_H}mm, content ends at {y:.1f}mm, margin {margin:.1f}mm")
