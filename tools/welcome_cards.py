#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""Welcome cards: a different little character for each tag that checks in.

Watches an OpenEPaperLink AP; the first time a real tag appears it gets a
one-off card sized to its own screen (size and colours from the AP repo's
tagtypes/*.json), drawn with nine hand-made characters that are dealt out in
turn. Pushed once per MAC; the MACs are remembered in tools/welcome_state.json.

    tools/welcome_cards.py watch [--minutes N]       poll the AP, card each new tag (default 10 min)
    tools/welcome_cards.py push MAC [DESIGN]          card one tag now (DESIGN 0-8, default: next in line)
    tools/welcome_cards.py preview [WxH ...] [--dir D] render all nine designs at those sizes to PNGs
    tools/welcome_cards.py forget MAC                 let a tag be carded again

Environment: OEPL_AP (required: the AP's URL, e.g. http://openepaperlink-xxxx.local),
OEPL_REPO (default ~/Code/OpenEPaperLink, for resources/tagtypes). Needs Pillow.
"""
import io
import json
import math
import os
import random
import sys
import time
import urllib.request

AP = os.environ.get("OEPL_AP", "").rstrip("/")


def need_ap():
    if not AP:
        sys.exit("set OEPL_AP to the access point's URL, e.g. OEPL_AP=http://openepaperlink-xxxx.local")
REPO = os.environ.get("OEPL_REPO", os.path.expanduser("~/Code/OpenEPaperLink"))
HERE = os.path.dirname(os.path.abspath(__file__))
STATE = os.path.join(HERE, "welcome_state.json")
ASSETS = os.path.join(HERE, "weather_assets")
ASSET_URL = ("https://raw.githubusercontent.com/OpenEPaperLink/Home_Assistant_Integration/"
             "main/custom_components/open_epaper_link/imagegen/assets/")
TAGTYPES_URL = "https://raw.githubusercontent.com/OpenEPaperLink/OpenEPaperLink/master/resources/tagtypes/"
HW_AP_DISPLAY = 0xE0      # the AP's own TFT shows up in get_db; not a tag

BLACK, WHITE, RED, YELLOW = (0, 0, 0), (255, 255, 255), (255, 0, 0), (255, 255, 0)


# ---------------------------------------------------------------------------
# AP access
# ---------------------------------------------------------------------------

def get(path):
    need_ap()
    with urllib.request.urlopen(f"{AP}/{path}", timeout=20) as r:
        return r.read()


def all_tags():
    out = []
    while True:
        page = json.loads(get(f"get_db?pos={len(out)}"))["tags"]
        if not page:
            return out
        out += page


def upload(mac, jpeg):
    need_ap()
    boundary = "----welcome%d" % int(time.time() * 1000)
    body = io.BytesIO()
    for k, v in {"mac": mac, "dither": "0"}.items():
        body.write(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode())
    body.write(f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"welcome.jpg\"\r\n"
               f"Content-Type: application/octet-stream\r\n\r\n".encode())
    body.write(jpeg + b"\r\n" + f"--{boundary}--\r\n".encode())
    req = urllib.request.Request(f"{AP}/imgupload", data=body.getvalue(), method="POST")
    req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.status, r.read().decode(errors="replace").strip()


def tagtype(hw):
    """width, height, accent colour for a hwType, from the OEPL repo (or GitHub)."""
    name = "%02X.json" % hw
    path = os.path.join(REPO, "resources", "tagtypes", name)
    if os.path.exists(path):
        d = json.load(open(path))
    else:
        d = json.load(urllib.request.urlopen(TAGTYPES_URL + name, timeout=20))
    table = d.get("colortable") or {}
    accent = RED if "red" in table else YELLOW if "yellow" in table else BLACK
    return int(d["width"]), int(d["height"]), accent, d.get("name", name)


# ---------------------------------------------------------------------------
# Drawing helpers
# ---------------------------------------------------------------------------

def fonts():
    from PIL import ImageFont
    os.makedirs(ASSETS, exist_ok=True)
    for f in ("ppb.ttf", "rbm.ttf"):
        if not os.path.exists(os.path.join(ASSETS, f)):
            urllib.request.urlretrieve(ASSET_URL + f, os.path.join(ASSETS, f))
    return (lambda size: ImageFont.truetype(os.path.join(ASSETS, "ppb.ttf"), max(8, int(size))),
            lambda size: ImageFont.truetype(os.path.join(ASSETS, "rbm.ttf"), max(8, int(size))))


def wrap(d, text, font, width):
    lines, cur = [], []
    for w in text.split():
        test = " ".join(cur + [w])
        if not cur or d.textlength(test, font=font) <= width:
            cur.append(w)
        else:
            lines.append(" ".join(cur)); cur = [w]
    if cur:
        lines.append(" ".join(cur))
    return lines


def poly(d, pts, fill, outline=BLACK, w=2):
    d.polygon(pts, fill=fill)
    if w:
        d.line(pts + [pts[0]], fill=outline, width=w)


# Each character draws inside a box (x, y, s, s), s = box side. `a` = accent colour.
# Coordinates are fractions of s so the same art works at 128 px and 448 px.

def art_rocket(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    poly(d, [f(.5, .05), f(.66, .3), f(.66, .72), f(.34, .72), f(.34, .3)], WHITE, w=w)   # body
    poly(d, [f(.5, .05), f(.66, .3), f(.34, .3)], a, w=w)                                  # nose
    poly(d, [f(.34, .5), f(.18, .8), f(.34, .72)], a, w=w)                                 # fins
    poly(d, [f(.66, .5), f(.82, .8), f(.66, .72)], a, w=w)
    d.ellipse([f(.42, .38), f(.58, .54)], fill=WHITE, outline=BLACK, width=w)              # window
    d.ellipse([f(.45, .41), f(.55, .51)], fill=BLACK)
    poly(d, [f(.4, .72), f(.6, .72), f(.5, .97)], a, w=w)                                  # flame
    poly(d, [f(.45, .72), f(.55, .72), f(.5, .86)], WHITE, w=0)


def art_ghost(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    d.pieslice([f(.2, .1), f(.8, .7)], 180, 360, fill=WHITE, outline=BLACK, width=w)
    d.rectangle([f(.2, .4), f(.8, .8)], fill=WHITE)
    d.line([f(.2, .4), f(.2, .8)], fill=BLACK, width=w)
    d.line([f(.8, .4), f(.8, .8)], fill=BLACK, width=w)
    pts = [f(.2, .8)]
    for i in range(6):
        pts.append(f(.2 + .1 * i + .05, .9 if i % 2 == 0 else .8))
    pts.append(f(.8, .8))
    d.line(pts, fill=BLACK, width=w)
    d.polygon([f(.2, .8)] + pts[1:-1] + [f(.8, .8)], fill=WHITE)
    d.line(pts, fill=BLACK, width=w)
    d.ellipse([f(.33, .35), f(.45, .5)], fill=BLACK)                                        # eyes
    d.ellipse([f(.55, .35), f(.67, .5)], fill=BLACK)
    d.ellipse([f(.36, .38), f(.41, .43)], fill=WHITE)
    d.ellipse([f(.58, .38), f(.63, .43)], fill=WHITE)
    d.ellipse([f(.44, .58), f(.56, .68)], fill=a)                                           # mouth


def art_robot(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    d.line([f(.5, .18), f(.5, .06)], fill=BLACK, width=w)                                   # antenna
    d.ellipse([f(.45, .0), f(.55, .1)], fill=a, outline=BLACK, width=w)
    d.rounded_rectangle([f(.15, .18), f(.85, .8)], radius=int(s * .08), fill=WHITE, outline=BLACK, width=w)
    d.rectangle([f(.05, .38), f(.15, .58)], fill=a, outline=BLACK, width=w)                # ears
    d.rectangle([f(.85, .38), f(.95, .58)], fill=a, outline=BLACK, width=w)
    d.rectangle([f(.26, .3), f(.44, .48)], fill=BLACK)                                     # eyes
    d.rectangle([f(.56, .3), f(.74, .48)], fill=BLACK)
    d.rectangle([f(.31, .35), f(.38, .42)], fill=WHITE)
    d.rectangle([f(.61, .35), f(.68, .42)], fill=WHITE)
    for i in range(5):                                                                     # grille mouth
        d.rectangle([f(.28 + i * .09, .58), f(.34 + i * .09, .7)], fill=a if i % 2 else BLACK)
    d.rectangle([f(.3, .8), f(.7, .95)], fill=WHITE, outline=BLACK, width=w)               # neck


def art_cat(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    poly(d, [f(.15, .45), f(.18, .08), f(.42, .3)], WHITE, w=w)                             # ears
    poly(d, [f(.85, .45), f(.82, .08), f(.58, .3)], WHITE, w=w)
    poly(d, [f(.2, .4), f(.22, .16), f(.38, .32)], a, w=0)
    poly(d, [f(.8, .4), f(.78, .16), f(.62, .32)], a, w=0)
    d.ellipse([f(.12, .22), f(.88, .9)], fill=WHITE, outline=BLACK, width=w)                # head
    d.ellipse([f(.3, .42), f(.42, .56)], fill=BLACK)                                        # eyes
    d.ellipse([f(.58, .42), f(.7, .56)], fill=BLACK)
    d.ellipse([f(.33, .45), f(.37, .49)], fill=WHITE)
    d.ellipse([f(.61, .45), f(.65, .49)], fill=WHITE)
    poly(d, [f(.45, .62), f(.55, .62), f(.5, .69)], a, w=0)                                # nose
    d.arc([f(.4, .64), f(.5, .76)], 0, 180, fill=BLACK, width=w)
    d.arc([f(.5, .64), f(.6, .76)], 0, 180, fill=BLACK, width=w)
    for dy in (-.04, 0, .04):                                                              # whiskers
        d.line([f(.05, .62 + dy), f(.3, .66 + dy * .5)], fill=BLACK, width=w)
        d.line([f(.95, .62 + dy), f(.7, .66 + dy * .5)], fill=BLACK, width=w)


def art_planet(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    rnd = random.Random(7)
    for _ in range(14):                                                                    # stars
        sx, sy = rnd.random(), rnd.random()
        if math.hypot(sx - .5, sy - .5) > .36:
            r = s * .012
            d.ellipse([f(sx, sy), (x + sx * s + 2 * r, y + sy * s + 2 * r)], fill=BLACK)
    d.ellipse([f(.25, .25), f(.75, .75)], fill=WHITE, outline=BLACK, width=w)               # planet
    d.chord([f(.25, .25), f(.75, .75)], 200, 330, fill=a)                                  # cap
    d.ellipse([f(.35, .5), f(.45, .58)], outline=BLACK, width=w)                            # craters
    d.ellipse([f(.55, .58), f(.62, .64)], outline=BLACK, width=w)
    d.arc([f(.05, .38), f(.95, .62)], 20, 200, fill=BLACK, width=w * 2)                      # ring (front)
    d.arc([f(.05, .38), f(.95, .62)], 200, 340, fill=a, width=w * 2)                          # ring (back)


def art_cactus(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    d.rounded_rectangle([f(.4, .15), f(.6, .72)], radius=int(s * .1), fill=WHITE, outline=BLACK, width=w)
    d.rounded_rectangle([f(.18, .3), f(.32, .55)], radius=int(s * .07), fill=WHITE, outline=BLACK, width=w)
    d.rectangle([f(.28, .45), f(.42, .55)], fill=WHITE)
    d.line([f(.3, .45), f(.4, .45)], fill=BLACK, width=w); d.line([f(.3, .55), f(.4, .55)], fill=BLACK, width=w)
    d.rounded_rectangle([f(.68, .22), f(.82, .5)], radius=int(s * .07), fill=WHITE, outline=BLACK, width=w)
    d.rectangle([f(.58, .4), f(.72, .5)], fill=WHITE)
    d.line([f(.6, .4), f(.7, .4)], fill=BLACK, width=w); d.line([f(.6, .5), f(.7, .5)], fill=BLACK, width=w)
    for i in range(6):                                                                     # spines
        d.line([f(.47, .22 + i * .08), f(.43, .2 + i * .08)], fill=BLACK, width=w)
        d.line([f(.53, .26 + i * .08), f(.57, .24 + i * .08)], fill=BLACK, width=w)
    d.ellipse([f(.44, .1), f(.56, .2)], fill=a, outline=BLACK, width=w)                    # flower
    poly(d, [f(.28, .72), f(.72, .72), f(.66, .95), f(.34, .95)], a, w=w)                  # pot
    d.rectangle([f(.25, .68), f(.75, .76)], fill=WHITE, outline=BLACK, width=w)


def art_lighthouse(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    poly(d, [f(.5, .18), f(.95, .02), f(.95, .3)], WHITE, outline=BLACK, w=w)               # beam
    poly(d, [f(.38, .3), f(.62, .3), f(.7, .9), f(.3, .9)], WHITE, w=w)                     # tower
    for i, (t, b) in enumerate([(.42, .55), (.66, .78)]):                                  # stripes
        d.polygon([f(.38 + (t - .3) * .133, t), f(.62 - (t - .3) * .133, t),
                   f(.62 - (b - .3) * .133 + .0, b), f(.38 + (b - .3) * .133, b)], fill=a)
        pass
    d.rectangle([f(.4, .18), f(.6, .3)], fill=WHITE, outline=BLACK, width=w)                # lamp room
    d.rectangle([f(.44, .2), f(.56, .28)], fill=a)
    poly(d, [f(.36, .18), f(.64, .18), f(.5, .06)], BLACK, w=0)                             # roof
    d.rectangle([f(.2, .9), f(.8, .97)], fill=BLACK)                                        # ground
    for i in range(4):                                                                     # waves
        d.arc([f(.0 + i * .25, .86), f(.2 + i * .25, .96)], 180, 360, fill=BLACK, width=w)


def art_mushroom(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    d.rounded_rectangle([f(.38, .5), f(.62, .9)], radius=int(s * .08), fill=WHITE, outline=BLACK, width=w)
    d.pieslice([f(.08, .1), f(.92, .7)], 180, 360, fill=a, outline=BLACK, width=w)          # cap
    d.line([f(.08, .4), f(.92, .4)], fill=BLACK, width=w)
    for cx, cy, r in ((.3, .27, .06), (.55, .18, .05), (.72, .3, .05), (.45, .34, .03)):
        d.ellipse([f(cx - r, cy - r), f(cx + r, cy + r)], fill=WHITE)
    d.ellipse([f(.43, .58), f(.48, .64)], fill=BLACK)                                       # face
    d.ellipse([f(.52, .58), f(.57, .64)], fill=BLACK)
    d.arc([f(.44, .64), f(.56, .74)], 0, 180, fill=BLACK, width=w)
    for i in range(3):                                                                     # grass
        d.line([f(.15 + i * .3, .92), f(.2 + i * .3, .8)], fill=BLACK, width=w)
        d.line([f(.2 + i * .3, .92), f(.25 + i * .3, .82)], fill=BLACK, width=w)


def art_sailboat(d, x, y, s, a):
    f = lambda px, py: (x + px * s, y + py * s)
    w = max(2, int(s / 40))
    d.ellipse([f(.7, .05), f(.9, .25)], fill=WHITE, outline=BLACK, width=w)                 # sun
    d.line([f(.5, .1), f(.5, .7)], fill=BLACK, width=w * 2)                                 # mast
    poly(d, [f(.5, .12), f(.5, .62), f(.2, .62)], WHITE, w=w)                               # main sail
    poly(d, [f(.53, .2), f(.53, .62), f(.8, .62)], a, w=w)                                  # jib
    poly(d, [f(.12, .7), f(.88, .7), f(.78, .85), f(.22, .85)], BLACK, w=0)                 # hull
    d.rectangle([f(.3, .74), f(.7, .77)], fill=WHITE)
    for i in range(5):                                                                     # waves
        d.arc([f(-.05 + i * .22, .86), f(.17 + i * .22, .98)], 180, 360, fill=BLACK, width=w)


DESIGNS = [
    ("Rocket", art_rocket, "Liftoff! I'm Rocket.", "Fresh out of the box and already at cruising altitude. Snacks are not included."),
    ("Ghost", art_ghost, "Boo. I'm Casper-ish.", "Friendly, mostly. I only haunt the fridge. Please leave the light on."),
    ("Robot", art_robot, "Beep. I am Unit 3.", "Emotions loaded: 100%. Awaiting instructions. Or pizza. Pizza is fine."),
    ("Cat", art_cat, "Meow. I'm Miso.", "I have already knocked something off a shelf in your honour. You're welcome."),
    ("Planet", art_planet, "Greetings from Zorb.", "Population: one tag. Gravity: optional. Visitors get a free ring."),
    ("Cactus", art_cactus, "I'm Spike.", "Low maintenance, high attitude. Water me never. Compliment me daily."),
    ("Lighthouse", art_lighthouse, "I'm Beacon.", "Guiding your snacks safely home since tonight. No ships lost so far."),
    ("Mushroom", art_mushroom, "I'm Morel.", "A fun guy, I'm told. I grow on people. Slowly. In the dark."),
    ("Sailboat", art_sailboat, "Ahoy. I'm Skipper.", "Current speed: zero knots. Absolutely nailing it. Land ho, probably."),
]


def render(W, H, accent, design, ctx):
    """One card at W x H. `ctx`: dict with mac, when, bat, n (tag number)."""
    from PIL import Image, ImageDraw
    name, art, headline, blurb = DESIGNS[design % len(DESIGNS)]
    bold, reg = fonts()
    img = Image.new("RGB", (W, H), WHITE)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    m = max(3, int(min(W, H) * 0.04))
    border = max(2, int(min(W, H) / 60))
    d.rectangle([0, 0, W - 1, H - 1], outline=BLACK, width=border)
    d.rectangle([border + 1, border + 1, W - border - 2, H - border - 2], outline=accent, width=1)

    landscape = W / H >= 1.5
    if landscape:
        art_s = int(H * 0.72)
        ax, ay = m * 2, (H - art_s) // 2
        tx, ty, tw = ax + art_s + m * 2, m * 2, W - (ax + art_s + m * 4)
    else:
        art_s = int(min(W * 0.55, H * 0.42))
        ax, ay = (W - art_s) // 2, m * 2
        tx, ty, tw = m * 2, ay + art_s + m, W - m * 4
    art(d, ax, ay, art_s, accent)

    head_size = min(tw / 9, H * (0.16 if landscape else 0.075))
    hf = bold(head_size)
    while d.textlength(headline, font=hf) > tw and head_size > 8:
        head_size -= 1; hf = bold(head_size)
    d.text((tx, ty), headline, fill=BLACK, font=hf)
    ty += int(head_size * 1.25)
    d.line([tx, ty, tx + int(tw * 0.35), ty], fill=accent, width=max(1, border // 2))
    ty += m

    body_size = head_size * 0.62
    bf = reg(body_size)
    foot_size = max(7, head_size * 0.42)
    ff = reg(foot_size)
    foot_h = int(foot_size * 2.6) + m * 2
    while body_size > 7:
        lines = wrap(d, blurb, bf, tw)
        if ty + len(lines) * int(body_size * 1.25) <= H - foot_h - m:
            break
        body_size -= 1; bf = reg(body_size)
    for ln in lines:
        d.text((tx, ty), ln, fill=BLACK, font=bf)
        ty += int(body_size * 1.25)

    foot1 = f"Tag #{ctx['n']} · woke up {ctx['when']}"
    foot2 = f"{ctx['mac']}" + (f" · {ctx['bat']}" if ctx.get("bat") else "")
    fy = H - m * 2 - int(foot_size * 2.5)
    d.text((tx, fy), foot1, fill=accent, font=ff)
    d.text((tx, fy + int(foot_size * 1.25)), foot2, fill=BLACK, font=ff)
    return img


def jpeg(img):
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=95)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def load_state():
    return json.load(open(STATE)) if os.path.exists(STATE) else {"pushed": {}}


def save_state(st):
    json.dump(st, open(STATE, "w"), indent=1)


def card_for(t, st, design=None):
    mac = t["mac"]
    W, H, accent, tname = tagtype(t["hwType"])
    n = len(st["pushed"]) + 1
    if design is None:
        design = (n - 1) % len(DESIGNS)
    bat = f"{t['batteryMv']} mV" if t.get("batteryMv") else ""
    ctx = {"mac": mac, "when": time.strftime("%-I:%M %p"), "bat": bat, "n": n}
    img = render(W, H, accent, design, ctx)
    status, body = upload(mac, jpeg(img))
    st["pushed"][mac] = {"design": DESIGNS[design][0], "n": n, "hw": "0x%02X" % t["hwType"], "size": f"{W}x{H}",
                         "at": time.strftime("%Y-%m-%d %H:%M:%S")}
    save_state(st)
    print(f"{time.strftime('%H:%M:%S')} {mac} ({tname}, {W}x{H}) -> #{n} {DESIGNS[design][0]}: {status} {body}")


def cmd_watch(args):
    minutes = float(args[args.index("--minutes") + 1]) if "--minutes" in args else 10
    st = load_state()
    print(f"watching {AP} for {minutes:g} min; {len(st['pushed'])} tag(s) already carded")
    t0 = time.time()
    while time.time() - t0 < minutes * 60:
        try:
            for t in all_tags():
                if t["hwType"] == HW_AP_DISPLAY or t["mac"] in st["pushed"]:
                    continue
                card_for(t, st)
        except Exception as e:
            print(f"{time.strftime('%H:%M:%S')} AP hiccup: {e.__class__.__name__}: {e}")
        time.sleep(10)
    print("done")


def cmd_push(args):
    mac = args[0].upper()
    design = int(args[1]) if len(args) > 1 else None
    t = next((t for t in all_tags() if t["mac"].upper() == mac), None)
    if not t:
        sys.exit(f"{mac} not on {AP}")
    st = load_state()
    st["pushed"].pop(mac, None)
    card_for(t, st, design)


def cmd_forget(args):
    st = load_state()
    st["pushed"].pop(args[0].upper(), None)
    save_state(st)


def cmd_preview(args):
    out = args[args.index("--dir") + 1] if "--dir" in args else "/tmp/welcome"
    sizes = [a for a in args if "x" in a and a[0].isdigit()] or ["296x128", "600x448"]
    os.makedirs(out, exist_ok=True)
    for sz in sizes:
        W, H = map(int, sz.split("x"))
        for i, (name, *_rest) in enumerate(DESIGNS):
            ctx = {"mac": "00124B00DEADBEEF", "when": "7:42 PM", "bat": "3012 mV", "n": i + 1}
            path = os.path.join(out, f"{sz}_{i}_{name}.png")
            render(W, H, RED, i, ctx).save(path)
    print(f"{len(DESIGNS) * len(sizes)} PNGs in {out}")


def main():
    cmds = {"watch": cmd_watch, "push": cmd_push, "preview": cmd_preview, "forget": cmd_forget}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    return cmds[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    sys.exit(main() or 0)
