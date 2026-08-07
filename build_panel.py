from gen_panel import text_to_path

HP = 5.08
WIDTH_HP = 32
PANEL_W = HP * WIDTH_HP  # 162.56mm
PANEL_H = 128.5

# Navy Cyber theme (from AppTheme.h)
BG = "#0D1E36"
BG2 = "#030508"
BORDER = "#5D8AA8"
TEXT_DIM = "#9AA6B8"
TEXT_BRIGHT = "#E8ECF2"
SLOT = "#181C24"
KNOB_AMBER = "#FFB300"
CYAN = "#00D2FF"

svg_parts = []
svg_parts.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {PANEL_W} {PANEL_H}" width="{PANEL_W}mm" height="{PANEL_H}mm">')

# background
svg_parts.append(f'<defs><linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
                  f'<stop offset="0%" stop-color="{BG}"/><stop offset="100%" stop-color="{BG2}"/></linearGradient></defs>')
svg_parts.append(f'<rect x="0" y="0" width="{PANEL_W}" height="{PANEL_H}" fill="url(#bg)"/>')
svg_parts.append(f'<rect x="0.5" y="0.5" width="{PANEL_W-1}" height="{PANEL_H-1}" fill="none" stroke="{BORDER}" stroke-width="0.4" opacity="0.6"/>')

# mounting screw holes (standard eurorack positions, 3U)
screw_r = 1.6
for sx in [7.5, PANEL_W - 7.5]:
    for sy in [5.5, PANEL_H - 5.5]:
        svg_parts.append(f'<circle cx="{sx}" cy="{sy}" r="{screw_r}" fill="#0a0a0c" stroke="{BORDER}" stroke-width="0.3"/>')

def label(text, x, y, size=3.6, color=TEXT_BRIGHT, anchor="start", font=None, weight_track=1.06):
    kwargs = {}
    if font:
        kwargs['font_path'] = font
    p, w = text_to_path(text, x, y, size, tracking=weight_track, anchor=anchor, **kwargs)
    return f'<g fill="{color}">{p}</g>', w

def section_box(x, y, w, h, title):
    parts = []
    parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="1.2" fill="{SLOT}" opacity="0.55" stroke="{BORDER}" stroke-width="0.25" stroke-opacity="0.5"/>')
    lp, lw = label(title, x + 2.2, y + 5.0, size=3.0, color=TEXT_DIM)
    parts.append(lp)
    return "\n".join(parts)

# Title
tp, tw = label("NAVY ARP 2", 8, 9.5, size=6.2, color=TEXT_BRIGHT)
svg_parts.append(tp)
sp, sw = label("dual-scene arpeggiator", 8, 14.0, size=2.4, color=TEXT_DIM)
svg_parts.append(sp)

# === SECTION LAYOUT (all coordinates in mm; SECTIONS dict is the single
# source of truth, also consumed by build_widget_layout.py so panel art
# and control placement can never drift apart again) ===
SECTIONS = {
    "steps":   (6,  18, PANEL_W - 12, 26, "STEPS  (probability / degree)"),
    "macro":   (6,  46, 86,          22, "MACRO"),
    "scene":   (94, 46, PANEL_W - 100, 22, "SCENE / MODE"),
    "key":     (6,  70, 62,          16, "KEY / DENSITY"),
    "display": (70, 70, PANEL_W - 76, 16, "DISPLAY"),
    "voice1":  (6,  88, (PANEL_W - 12) / 2 - 1, 20, "VOICE 1"),
    "voice2":  (6 + (PANEL_W - 12) / 2 + 1, 88, (PANEL_W - 12) / 2 - 1, 20, "VOICE 2"),
    "io":      (6, 110, PANEL_W - 12, 16, "I/O"),
}

for key, (x, y, w, h, title) in SECTIONS.items():
    svg_parts.append(section_box(x, y, w, h, title))

dp, dw = label("OLED / LFO matrix - coming soon", SECTIONS["display"][0] + 3,
               SECTIONS["display"][1] + 11, size=2.6, color=TEXT_DIM)
svg_parts.append(dp)

svg_parts.append('</svg>')

with open("res/NavyArp2.svg", "w") as f:
    f.write("\n".join(svg_parts))

import json
with open("panel_sections.json", "w") as f:
    json.dump({"PANEL_W": PANEL_W, "PANEL_H": PANEL_H, "SECTIONS": SECTIONS}, f, indent=2)

print(f"Panel: {PANEL_W:.2f}mm x {PANEL_H}mm ({WIDTH_HP}HP)")

# --- pass 2: per-control micro-labels, appended after main render ---
