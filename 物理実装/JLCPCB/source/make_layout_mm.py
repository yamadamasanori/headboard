#!/usr/bin/env python3
"""PCB layout v2 (mm). Parts keep 2.54 mm pitch within each part, but parts are placed freely.
Writes layout_mm.json consumed by gen_pcb2.py."""
import json
P = 2.54
def g(c, r):  # old perfboard grid -> mm (used for the XIAO / MOSFET / terminal block block that is unchanged)
    return [round(5.52 + (c - 1) * P, 3), round(5.72 + (r - 1) * P, 3)]

parts = []
def part(ref, typ, val, body, pins, **kw):
    d = dict(ref=ref, type=typ, val=val, body=body, pins=pins); d.update(kw); parts.append(d)
def pin(p, xy, net): return dict(p=p, x=xy[0], y=xy[1], net=net)

# XIAO (unchanged position)
top = ["D6", "D5", "D4", "D3", "D2", "D1", "D0"]; topnet = [None, "SCL", "SDA", None, None, None, None]
bot = ["D7", "D8", "D9", "D10", "3V3", "GND", "5V"]; botnet = ["G44", None, None, "G9", "V33", "GND", "V5"]
xp = [pin(top[i], g(18 + i, 1), topnet[i]) for i in range(7)] + [pin(bot[i], g(18 + i, 7), botnet[i]) for i in range(7)]
part("U3", "xiao", "XIAO_ESP32S3", [46.6, 4.45, 66.8, 22.23], xp)

# OLED 0.96" (27.3 x 27.8), header 4 pins at top, centred
ox0, oy0 = 5.8, 1.0   # clear of the top-left M3 hole (screw head r=2.75)
pc = ox0 + 27.3 / 2
oy = 2.5
ol = [pin(n, [round(pc + (i - 1.5) * P, 3), oy], net) for i, (n, net) in enumerate([("GND", "GND"), ("VCC", "V33"), ("SCL", "SCL"), ("SDA", "SDA")])]
part("U2", "oled", "OLED_0.96_I2C", [ox0, oy0, ox0 + 27.3, oy0 + 27.8], ol, raised=True)

# J2 DC jack DIP module 15.24 sq, opening down at board bottom edge; 2 columns x 4 pins, 12.7 mm apart.
jx0, jy1 = 8.0 - 2.54 + 0.7, 47.0   # 3.16 mm from the M3 hole centre (head r=2.75)
jy0 = jy1 - 15.24
colx = [jx0 + 1.27, jx0 + 1.27 + 12.7]
rowy = [round(jy0 + 3.81 + k * P, 3) for k in range(4)]
jp = []
for cx in colx:
    for k, y in enumerate(rowy):
        jp.append(pin("+" if k < 2 else "-", [round(cx, 3), y], "V12" if k < 2 else "GND"))
part("J2", "jack", "DC_JACK_DIP", [jx0, jy0, jx0 + 15.24, jy1], jp)

# U1 M78AR05-1 horizontal, pins y=38.54
ux = [30.5, 33.04, 35.58]
part("U1", "sip", "M78AR05-1", [27.24, 34.74, 38.84, 42.34],
     [pin("1", [ux[0], 38.54], "V12"), pin("2", [ux[1], 38.54], "GND"), pin("3", [ux[2], 38.54], "VDC5")])

# D1 vertical, K top (bridges the GND bus)
part("D1", "diode", "1N5819", [37.15, 19.66, 39.85, 27.36], [pin("K", [38.5, 18.42], "V5"), pin("A", [38.5, 28.58], "VDC5")], low=True)
# C2 output cap (5.08 mm), GND pad on the bus
part("C2", "ccap", "22uF", [33.25, 25.3, 36.75, 31.9], [pin("2", [35.0, 26.04], "GND"), pin("1", [35.0, 31.12], "VDC5")], low=True)
# C1 input cap (5.08 mm)
part("C1", "ccap", "22uF_25V", [23.45, 35.4, 26.55, 42.1], [pin("-", [25.0, 36.2], "GND"), pin("+", [25.0, 41.28], "V12")], low=True)

# Cells / MOSFETs / terminal blocks (unchanged)
part("R1", "res", "100", [55.87, 24.1, 56.77, 30.5], [pin("1", g(21, 8), "G9"), pin("2", g(21, 11), "GATE1")], low=True)
part("R3", "res", "100k", [53.33, 24.1, 54.23, 30.5], [pin("1", g(20, 8), "GND"), pin("2", g(20, 11), "GATE1")], low=True)
part("R2", "res", "100", [48.25, 24.1, 49.15, 30.5], [pin("1", g(18, 8), "G44"), pin("2", g(18, 11), "GATE2")], low=True)
part("R4", "res", "100k", [50.79, 24.1, 51.69, 30.5], [pin("1", g(19, 8), "GND"), pin("2", g(19, 11), "GATE2")], low=True)
part("Q1", "fet", "2SK4017", [57.7, 30.0, 65.1, 32.2], [pin("G", g(22, 11), "GATE1"), pin("D", g(23, 11), "LED1N"), pin("S", g(24, 11), "GND")])
part("Q2", "fet", "2SK4017", [39.9, 30.0, 47.3, 32.2], [pin("S", g(15, 11), "GND"), pin("D", g(16, 11), "LED2N"), pin("G", g(17, 11), "GATE2")])
# WJ141V: 2 pins per pole, 5.08 mm apart; body 12.8 deep, 2.9 mm from the lower pin row to the body edge
TU, TL = 38.10, 43.18
def tbpins(cols, nets):
    out = []
    for c, (lab, net) in zip(cols, nets):
        x = g(c, 1)[0]
        out += [pin(lab, [x, TU], net), pin(lab, [x, TL], net)]
    return out
part("TB3", "tb", "LED2/3", [41.08, TL - 9.9, 53.78, TL + 2.9], tbpins([16, 17, 18, 19], [("+", "V12"), ("-", "LED2N"), ("-", "LED2N"), ("+", "V12")]))
part("TB2", "tb", "LED1", [53.78, TL - 9.9, 61.4, TL + 2.9], tbpins([21, 22], [("+", "V12"), ("-", "LED1N")]))

B, F = "B.Cu", "F.Cu"
wires = []
def w(net, layer, pts, note): wires.append(dict(net=net, layer=layer, pts=pts, to=note))
# GND
w("GND", B, [[4.2, 40.65], [4.2, 26.04], [63.94, 26.04]], "J2(-) -> GND bus y=26.04")
w("GND", B, [[4.2, 40.65], [colx[0], 40.65]], "J2(-) left column")
w("GND", B, [[ol[0]["x"], 2.5], [ol[0]["x"], 26.04]], "OLED GND -> bus")
w("GND", B, [g(23, 7), [g(23, 7)[0], 26.04]], "XIAO GND -> bus")
w("GND", B, [g(24, 11), [g(24, 11)[0], 26.04]], "Q1 S -> bus")
w("GND", B, [g(15, 11), [g(15, 11)[0], 26.04]], "Q2 S -> bus")
w("GND", B, [g(19, 8), [g(19, 8)[0], 26.04]], "R4 -> bus")
w("GND", B, [g(20, 8), [g(20, 8)[0], 26.04]], "R3 -> bus")
w("GND", B, [[33.04, 38.54], [33.04, 26.04]], "U1 GND -> bus")
w("GND", B, [[25.0, 36.2], [25.0, 26.04]], "C1(-) -> bus")
# +12V
w("V12", B, [[colx[1], rowy[1]], [25.0, rowy[1]], [25.0, 41.28], [39.6, 41.28], [39.6, 45.4], [g(21, 1)[0], 45.4]], "J2(+) -> C1(+) -> 12V bus (y=41.28 / 45.4)")
w("V12", B, [[30.5, 38.54], [30.5, 41.28]], "U1 Vin -> bus")
for c in (16, 19, 21):
    w("V12", B, [[g(c, 1)[0], TU], [g(c, 1)[0], TL], [g(c, 1)[0], 45.4]], "TB+ (both pins) -> bus")
for c, n in ((17, "LED2N"), (18, "LED2N"), (22, "LED1N")):
    w(n, B, [[g(c, 1)[0], TU], [g(c, 1)[0], TL]], "TB- pin pair")
# DCDC out / 5V
w("VDC5", B, [[35.58, 38.54], [38.5, 38.54], [38.5, 28.58]], "U1 Vout -> D1 A")
w("VDC5", B, [[35.0, 31.12], [38.5, 31.12]], "C2 -> D1 A")
w("V5", B, [[38.5, 18.42], [38.5, 15.5], [g(24, 7)[0], 15.5], g(24, 7)], "D1 K -> XIAO 5V")
# OLED
w("V33", F, [g(22, 7), [g(22, 7)[0], 13.0], [ol[1]["x"], 13.0], [ol[1]["x"], 2.5]], "XIAO 3V3 -> OLED VCC (top)")
w("SCL", F, [g(19, 1), [g(19, 1)[0], 10.5], [ol[2]["x"], 10.5], [ol[2]["x"], 2.5]], "XIAO D5 -> OLED SCL (top)")
w("SDA", B, [g(20, 1), [g(20, 1)[0], 8.0], [ol[3]["x"], 8.0], [ol[3]["x"], 2.5]], "XIAO D4 -> OLED SDA")
# gates / LEDs
w("G44", B, [g(18, 7), g(18, 8)], "XIAO D7 -> R2")
w("G9", B, [g(21, 7), g(21, 8)], "XIAO D10 -> R1")
w("GATE1", B, [g(20, 11), g(21, 11), g(22, 11)], "R3,R1 -> Q1 G")
w("GATE2", B, [g(19, 11), g(18, 11), g(17, 11)], "R4,R2 -> Q2 G")
w("LED1N", B, [g(23, 11), g(23, 12), g(22, 12), [g(22, 1)[0], TU]], "Q1 D -> TB2-")
w("LED2N", B, [g(16, 11), g(16, 12), g(17, 12), [g(17, 1)[0], TU], [g(18, 1)[0], TU]], "Q2 D -> TB3-")

json.dump(dict(board=[72.0, 47.0], mount=[[3, 3], [69, 3], [3, 44], [69, 44]], parts=parts, wires=wires),
          open("layout_mm.json", "w"), ensure_ascii=False, indent=1)
print(len(parts), "parts", len(wires), "wires")
