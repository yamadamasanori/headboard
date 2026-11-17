#!/usr/bin/env python3
"""Generate a KiCad 7 PCB (.kicad_pcb) from the perfboard layout (layout.json).

Board: Sunhayato ICB-88 outline, 72 x 47 mm, 4x M3 (3.2 mm) mounting holes 3.0 mm from edges.
Grid: 25 x 15 holes, 2.54 mm pitch, first hole at (5.52, 5.72) mm.
"""
import json, sys, uuid, math

L = json.load(open(sys.argv[1]))
OUT = sys.argv[2]

PITCH = 2.54
X0, Y0 = 5.52, 5.72
W, H = 72.0, 47.0
def gx(i): return round(X0 + (i - 1) * PITCH, 4)
def gy(j): return round(Y0 + (j - 1) * PITCH, 4)
def uid(): return str(uuid.uuid4())

# ---- nets ----
netnames = ["GND", "+12V", "+5V_XIAO", "VDC5", "+3V3", "SDA", "SCL", "LED1_PWM", "LED23_PWM",
            "GATE1", "GATE2", "LED1-", "LED23-"]
key2name = {"GND": "GND", "V12": "+12V", "V5": "+5V_XIAO", "VDC5": "VDC5", "V33": "+3V3",
            "SDA": "SDA", "SCL": "SCL", "G9": "LED1_PWM", "G44": "LED23_PWM",
            "GATE1": "GATE1", "GATE2": "GATE2", "LED1N": "LED1-", "LED2N": "LED23-"}
netid = {n: i + 1 for i, n in enumerate(netnames)}
POWER = {"GND", "+12V", "+5V_XIAO", "VDC5", "LED1-", "LED23-"}
def net_s(k):
    if not k: return ""
    n = key2name[k]
    return f'(net {netid[n]} "{n}")'

# ---- pad specs per part type: (pad dia, drill) ----
SPEC = {
    "xiao": (1.7, 1.0), "oled": (1.7, 1.0), "jack": (1.7, 1.0), "sip": (1.8, 1.0),
    "diode": (2.0, 1.1), "ccap": (1.6, 0.8), "res": (1.6, 0.8), "fet": (1.7, 1.0), "tb": (2.0, 1.2),
}

VALUES = {"U3": "XIAO_ESP32S3", "U2": "OLED_0.96_I2C", "J2": "DC_JACK_DIP", "U1": "M78AR05-1",
          "D1": "1N5819", "C1": "22uF_25V", "C2": "22uF", "R1": "100", "R2": "100", "R3": "100k",
          "R4": "100k", "Q1": "2SK4017", "Q2": "2SK4017", "TB2": "LED1", "TB3": "LED2/3"}

def silk_line(x1, y1, x2, y2, w=0.15, layer="F.SilkS"):
    return f'(fp_line (start {x1:.3f} {y1:.3f}) (end {x2:.3f} {y2:.3f}) (stroke (width {w}) (type solid)) (layer "{layer}") (tstamp {uid()}))'
def silk_rect(x1, y1, x2, y2, w=0.15):
    return "\n".join([silk_line(x1, y1, x2, y1, w), silk_line(x2, y1, x2, y2, w),
                      silk_line(x2, y2, x1, y2, w), silk_line(x1, y2, x1, y1, w)])
def silk_text(t, x, y, size=1.0, layer="F.SilkS", kind="user", rot=0):
    th = round(size * 0.15, 3)
    head = f'(fp_text {kind} "{t}"' if kind == "user" else f'(fp_text {kind} "{t}"'
    return f'{head} (at {x:.3f} {y:.3f} {rot}) (layer "{layer}") (effects (font (size {size} {size}) (thickness {th}))) (tstamp {uid()}))'

def bx(b):  # body box in hole units -> mm, clipped to board
    x1 = X0 + (b[0] - 1) * PITCH; y1 = Y0 + (b[1] - 1) * PITCH
    x2 = X0 + (b[2] - 1) * PITCH; y2 = Y0 + (b[3] - 1) * PITCH
    return x1, y1, x2, y2

def clip(v, lo, hi): return max(lo, min(hi, v))

fps = []
for p in L["parts"]:
    dia, drill = SPEC[p["type"]]
    items = []
    x1, y1, x2, y2 = bx(p["body"])
    cx, cy = (x1 + x2) / 2, (y1 + y2) / 2
    ref = p["ref"]
    # reference text position
    rx, ry, rsize = cx, cy, 1.0
    if p["type"] == "fet": ry = y2 + 0.9
    if p["type"] == "tb": ry = y2 - 1.6
    if p["type"] == "sip": ry = cy + 1.6
    if p["type"] == "ccap" and ref == "C1": rx = cx + 2.6
    items.append(f'(fp_text reference "{ref}" (at {rx:.3f} {ry:.3f} {90 if p["type"]=="res" else 0}) (layer "F.SilkS") (effects (font (size {rsize} {rsize}) (thickness 0.15))) (tstamp {uid()}))')
    items.append(f'(fp_text value "{VALUES.get(ref, p["val"])}" (at {cx:.3f} {cy+1.2:.3f}) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))) (tstamp {uid()}))')
    # outline (clipped to board for silkscreen)
    cx1, cy1, cx2, cy2 = clip(x1, 0.4, W - 0.4), clip(y1, 0.4, H - 0.4), clip(x2, 0.4, W - 0.4), clip(y2, 0.4, H - 0.4)
    if p["type"] not in ("res",):
        items.append(silk_rect(cx1, cy1, cx2, cy2, 0.12))
    else:
        items.append(silk_rect(x1, y1, x2, y2, 0.12))
    # fab outline (full size)
    items.append("\n".join(l.replace('"F.SilkS"', '"F.Fab"') for l in silk_rect(x1, y1, x2, y2, 0.1).split("\n")))
    # pads
    for n, q in enumerate(p["pins"], start=1):
        px, py = gx(q["x"]), gy(q["y"])
        shape = "rect" if n == 1 and p["type"] in ("xiao", "oled", "tb", "jack") else "circle"
        if p["type"] == "sip":
            shape = "rect" if q["p"] == "1" else "circle"
            n = q["p"]
        if p["type"] == "diode" and q["p"] == "K": shape = "rect"
        items.append(f'(pad "{n}" thru_hole {shape} (at {px} {py}) (size {dia} {dia}) (drill {drill}) (layers "*.Cu" "*.Mask") {net_s(q["net"])} (tstamp {uid()}))')
    # pin labels
    if p["type"] in ("xiao", "oled"):
        for q in p["pins"]:
            off = -1.7 if q["y"] in (1, 4) and p["type"] == "oled" else (1.6 if q["y"] == 1 else -1.6)
            if p["type"] == "oled": off = 1.6
            items.append(silk_text(q["p"], gx(q["x"]), gy(q["y"]) + off, 0.7))
    if p["type"] in ("tb", "jack"):
        for q in p["pins"]:
            t = "+" if q["p"] == "+" else ("-" if q["p"] in ("−", "-") else q["p"])
            dy = -1.5 if p["type"] == "tb" else (1.5 if q["y"] == 10 else -1.5)
            items.append(silk_text(t, gx(q["x"]), gy(q["y"]) + dy, 1.0))
    if p["type"] == "fet":
        for q in p["pins"]:
            items.append(silk_text(q["p"], gx(q["x"]), gy(q["y"]) - 1.5, 0.7))
    if p["type"] == "sip":
        items.append(silk_text("1:Vin 2:GND 3:Vout", cx, y1 - 0.8 if y1 > 2 else y2 + 0.8, 0.7))
    if p["type"] == "diode":
        k = [q for q in p["pins"] if q["p"] == "K"][0]
        items.append(silk_line(gx(k["x"]) - 0.8, gy(k["y"]) + 1.5, gx(k["x"]) + 0.8, gy(k["y"]) + 1.5, 0.3))
    if p["type"] == "ccap" and ref == "C1":
        pass
    fp = f'''(footprint "LEDdimmer:{ref}" (layer "F.Cu") (tstamp {uid()}) (at 0 0)
  (attr through_hole)
  {chr(10).join(items)}
)'''
    fps.append(fp)

# ---- tracks ----
segs, vias = [], []
for w in L["wires"]:
    n = key2name[w["net"]]
    width = 1.0 if n in POWER else 0.4
    layer = "F.Cu" if w["how"] == "ジャンパー" else "B.Cu"
    pts = w["pts"]
    for a, b in zip(pts, pts[1:]):
        segs.append(f'(segment (start {gx(a[0])} {gy(a[1])}) (end {gx(b[0])} {gy(b[1])}) (width {width}) (layer "{layer}") (net {netid[n]}) (tstamp {uid()}))')
    if w["how"] == "ジャンパー":
        for e in (pts[0], pts[-1]):
            vias.append(f'(via (at {gx(e[0])} {gy(e[1])}) (size 1.0) (drill 0.5) (layers "F.Cu" "B.Cu") (net {netid[n]}) (tstamp {uid()}))')

# ---- board outline + mounting holes ----
edge = []
r = 1.0  # corner radius
edge.append(f'(gr_line (start {r} 0) (end {W-r} 0) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts") (tstamp {uid()}))')
edge.append(f'(gr_line (start {W} {r}) (end {W} {H-r}) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts") (tstamp {uid()}))')
edge.append(f'(gr_line (start {W-r} {H}) (end {r} {H}) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts") (tstamp {uid()}))')
edge.append(f'(gr_line (start 0 {H-r}) (end 0 {r}) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts") (tstamp {uid()}))')
for (sx, sy, ex, ey, mx, my) in [(r, 0, 0, r, r - r*math.cos(math.radians(45)), r - r*math.sin(math.radians(45))),
                                  (W, r, W - r, 0, W - r + r*math.cos(math.radians(45)), r - r*math.sin(math.radians(45))),
                                  (W - r, H, W, H - r, W - r + r*math.cos(math.radians(45)), H - r + r*math.sin(math.radians(45))),
                                  (0, H - r, r, H, r - r*math.cos(math.radians(45)), H - r + r*math.sin(math.radians(45)))]:
    edge.append(f'(gr_arc (start {sx} {sy}) (mid {mx:.4f} {my:.4f}) (end {ex} {ey}) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts") (tstamp {uid()}))')

mh = []
for i, (mx, my) in enumerate([(3.0, 3.0), (W - 3.0, 3.0), (3.0, H - 3.0), (W - 3.0, H - 3.0)], start=1):
    mh.append(f'''(footprint "LEDdimmer:MountingHole_3.2mm" (layer "F.Cu") (tstamp {uid()}) (at {mx} {my})
  (attr exclude_from_pos_files exclude_from_bom)
  (fp_text reference "H{i}" (at 0 -2.6) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))) (tstamp {uid()}))
  (fp_text value "M3" (at 0 2.6) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))) (tstamp {uid()}))
  (fp_circle (center 0 0) (end 2.75 0) (stroke (width 0.05) (type solid)) (fill none) (layer "Cmts.User") (tstamp {uid()}))
  (pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2) (layers "*.Cu" "*.Mask") (tstamp {uid()}))
)''')

# ---- board-level silkscreen ----
gtext = []
def gr_text(t, x, y, size=1.0, layer="F.SilkS", rot=0):
    mir = " (justify mirror)" if layer.startswith("B.") else ""
    return f'(gr_text "{t}" (at {x} {y} {rot}) (layer "{layer}") (tstamp {uid()}) (effects (font (size {size} {size}) (thickness {round(size*0.15,3)})){mir}))'
gtext.append(gr_text("ESP32-S3 LED DIMMER  v1.0", 36.0, 1.6, 1.0))
gtext.append(gr_text("12V IN", gx(11.5), H - 1.2, 1.0))
gtext.append(gr_text("USB-C ->", 66.5, 23.0, 0.8, rot=90))
gtext.append(gr_text("LED1=GPIO9  LED2/3=GPIO44  SDA=GPIO5 SCL=GPIO6", 36.0, 45.6, 0.7, layer="B.SilkS"))
gtext.append(gr_text("JLCJLCJLCJLC", 36.0, 2.0, 1.0, layer="B.SilkS"))


layers = '''(layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive")
    (33 "F.Adhes" user "F.Adhesive")
    (34 "B.Paste" user)
    (35 "F.Paste" user)
    (36 "B.SilkS" user "B.Silkscreen")
    (37 "F.SilkS" user "F.Silkscreen")
    (38 "B.Mask" user)
    (39 "F.Mask" user)
    (40 "Dwgs.User" user "User.Drawings")
    (41 "Cmts.User" user "User.Comments")
    (42 "Eco1.User" user "User.Eco1")
    (43 "Eco2.User" user "User.Eco2")
    (44 "Edge.Cuts" user)
    (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard")
    (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user)
    (49 "F.Fab" user)
  )'''

nets = '(net 0 "")\n  ' + "\n  ".join(f'(net {netid[n]} "{n}")' for n in netnames)

pcb = f'''(kicad_pcb (version 20221018) (generator pcbnew)
  (general (thickness 1.6))
  (paper "A4")
  (title_block (title "ESP32-S3 LED Dimmer") (rev "1.0") (company "") (comment 1 "ICB-88 outline 72x47mm"))
  {layers}
  (setup
    (pad_to_mask_clearance 0)
    (pcbplotparams
      (layerselection 0x00010fc_ffffffff)
      (plot_on_all_layers_selection 0x0000000_00000000)
      (disableapertmacros false) (usegerberextensions true) (usegerberattributes true)
      (usegerberadvancedattributes true) (creategerberjobfile false) (dashed_line_dash_ratio 12.000000)
      (dashed_line_gap_ratio 3.000000) (svgprecision 4) (plotframeref false) (viasonmask false) (mode 1)
      (useauxorigin false) (hpglpennumber 1) (hpglpenspeed 20) (hpglpendiameter 15.000000)
      (dxfpolygonmode true) (dxfimperialunits true) (dxfusepcbnewfont true) (psnegative false) (psa4output false)
      (plotreference true) (plotvalue false) (plotinvisibletext false) (sketchpadsonfab false)
      (subtractmaskfromsilk true) (outputformat 1) (mirror false) (drillshape 0) (scaleselection 1)
      (outputdirectory "gerber/"))
  )
  {nets}
  {chr(10).join(fps)}
  {chr(10).join(mh)}
  {chr(10).join(edge)}
  {chr(10).join(gtext)}
  {chr(10).join(segs)}
  {chr(10).join(vias)}
)
'''
open(OUT, "w").write(pcb)
print("wrote", OUT, len(fps), "footprints", len(segs), "segments", len(vias), "vias")
