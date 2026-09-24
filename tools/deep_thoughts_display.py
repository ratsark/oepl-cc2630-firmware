#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""A random Deep Thought (Jack Handey) on a 6" BWR tag, driven by Home Assistant.

Nothing runs on the dev host: this writes one HA automation that, every few
hours, picks a random entry from QUOTES and renders it on the tag through the
OpenEPaperLink integration's `drawcustom` service. The quote list lives inside
the automation, so HA is self-contained once installed.

    tools/deep_thoughts_display.py yaml MAC              print the automation config
    tools/deep_thoughts_display.py install MAC [--hours N] [--alias NAME]
                                                          write the automation into HA (asks first),
                                                          optionally set the tag's alias
    tools/deep_thoughts_display.py show MAC              trigger it now (new quote on the tag)
    tools/deep_thoughts_display.py preview [prefix]      render three quotes to PNGs locally (needs Pillow)

MAC is the tag's 16-hex-digit address as the AP shows it, e.g. 00124B00174A7A50.

Quotes live in tools/deep_thoughts.txt, one per line (created with a small seed
on first run, and gitignored: Deep Thoughts are Jack Handey's copyrighted work,
so fill it from your own copies). Any length; the font size adapts. Re-run
`install` after editing it -- the list is copied into the automation.

Environment: HA_URL (default http://ha-home.local:8123), HA token from
~/secrets.toml [ha-local] long_lived_access_token.
"""
import json
import os
import sys
import tomllib
import urllib.request

HA_URL = os.environ.get("HA_URL", "http://ha-home.local:8123").rstrip("/")
AUTOMATION_ID = "deep_thoughts_oepl_display"
W, H = 600, 448

QUOTES_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "deep_thoughts.txt")

# A small seed, written to QUOTES_FILE on first use. Put your own in that file.
SEED_QUOTES = [
    "If you ever fall off the Sears Tower, just go real limp, because maybe you'll look like a dummy "
    "and people will try to catch you because, hey, free dummy.",
    "It's easy to sit there and say you'd like to have more money. And I guess that's what I like about it. "
    "It's easy. Just sitting there, rocking back and forth, wanting that money.",
    "If trees could scream, would we be so cavalier about cutting them down? We might, if they screamed "
    "all the time, for no good reason.",
    "I hope if dogs ever take over the world, and they chose a king, they don't just go by size, because "
    "I bet there are some Chihuahuas with some good ideas.",
    "Before you criticize someone, you should walk a mile in their shoes. That way when you criticize them, "
    "you are a mile away from them and you have their shoes.",
    "I can picture in my mind a world without war, a world without hate. And I can picture us attacking "
    "that world, because they'd never expect it.",
    "The face of a child can say it all, especially the mouth part of the face.",
    "To me, clowns aren't funny. In fact, they're kind of scary. I've wondered where this started and I "
    "think it goes back to the time I went to the circus, and a clown killed my dad.",
]


def load_quotes():
    """One quote per line in tools/deep_thoughts.txt; blank lines and # comments ignored."""
    if not os.path.exists(QUOTES_FILE):
        with open(QUOTES_FILE, "w") as f:
            f.write("# Deep Thoughts, one per line. Blank lines and lines starting with # are ignored.\n"
                    "# This file is gitignored: it is your copy of Jack Handey's copyrighted work.\n\n")
            f.write("\n".join(SEED_QUOTES) + "\n")
    with open(QUOTES_FILE) as f:
        qs = [ln.strip() for ln in f if ln.strip() and not ln.lstrip().startswith("#")]
    if not qs:
        sys.exit(f"{QUOTES_FILE} has no quotes")
    return qs


QUOTES = load_quotes()


# ---------------------------------------------------------------------------
# Layout (a drawcustom payload; `q` is set by the automation from QUOTES)
# ---------------------------------------------------------------------------

SIZE_TPL = "{% if q|length > 300 %}24{% elif q|length > 220 %}27{% elif q|length > 150 %}30{% elif q|length > 90 %}34{% else %}40{% endif %}"


def size_for(q):
    """Same rule as SIZE_TPL, for the local preview."""
    n = len(q)
    return 24 if n > 300 else 27 if n > 220 else 30 if n > 150 else 34 if n > 90 else 40


def build_payload(q="{{ q }}", size=SIZE_TPL):
    L, R = 28, 572
    return [
        # frame: a thin black rule with a red inner rule
        {"type": "rectangle", "x_start": 10, "y_start": 10, "x_end": 589, "y_end": 437, "fill": "white", "outline": "black", "width": 2},
        {"type": "rectangle", "x_start": 16, "y_start": 16, "x_end": 583, "y_end": 431, "fill": "white", "outline": "red", "width": 1},
        # header
        {"type": "text", "value": "DEEP THOUGHTS", "x": 300, "y": 44, "size": 26, "font": "ppb.ttf",
         "color": "black", "anchor": "mm", "align": "center"},
        {"type": "text", "value": "by Jack Handey", "x": 300, "y": 70, "size": 18, "font": "rbm.ttf",
         "color": "red", "anchor": "mm", "align": "center"},
        {"type": "line", "x_start": 200, "y_start": 88, "x_end": 400, "y_end": 88, "width": 2, "fill": "red"},
        # big red quotation marks bracketing the text block
        {"type": "icon", "value": "format-quote-open", "x": L, "y": 100, "size": 64, "color": "red", "anchor": "lt"},
        {"type": "icon", "value": "format-quote-close", "x": R, "y": 396, "size": 64, "color": "red", "anchor": "rb"},
        # the thought, wrapped and centred in the space between the marks
        {"type": "text", "value": q, "x": 300, "y": 246, "size": size, "font": "rbm.ttf",
         "color": "black", "anchor": "mm", "align": "center", "max_width": 470, "spacing": 8},
    ]


def build_automation(device_id, hours):
    return {
        "id": AUTOMATION_ID,
        "alias": 'Deep Thoughts Display (OEPL 6")',
        "description": "A random Jack Handey Deep Thought on a 6 inch BWR e-paper tag. "
                       "Generated by tools/deep_thoughts_display.py in oepl-cc2630-firmware; "
                       "add quotes there or edit the `quotes` list below.",
        "triggers": [{"trigger": "time_pattern", "minutes": "3", "hours": f"/{hours}"},
                     {"trigger": "homeassistant", "event": "start"}],
        "variables": {"quotes": QUOTES},
        "actions": [
            {"variables": {"q": "{{ quotes | random }}"}},
            {"action": "open_epaper_link.drawcustom", "target": {"device_id": device_id},
             # ttl: how long the tag may sleep between check-ins (AP caps at maxsleep, tag at 1 h)
             "data": {"background": "white", "rotate": 0, "dither": 0, "ttl": hours * 3600,
                      "payload": build_payload()}},
        ],
        "mode": "single",
    }


# ---------------------------------------------------------------------------
# Home Assistant access
# ---------------------------------------------------------------------------

def ha_token():
    return tomllib.load(open(os.path.expanduser("~/secrets.toml"), "rb"))["ha-local"]["long_lived_access_token"]


def ha(path, data=None):
    hdr = {"Authorization": f"Bearer {ha_token()}", "Content-Type": "application/json"}
    req = urllib.request.Request(HA_URL + path, headers=hdr,
                                 data=json.dumps(data).encode() if data is not None else None,
                                 method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=30) as r:
        body = r.read()
    try:
        return json.loads(body or b"null")
    except json.JSONDecodeError:
        return body.decode()


def device_id_for(mac):
    mac = mac.lower().replace(":", "")
    ent = f"sensor.{mac}_battery_percentage"
    did = ha("/api/template", {"template": "{{ device_id('%s') }}" % ent})
    if not did or did == "None":
        sys.exit(f"HA has no OpenEPaperLink device for {mac} ({ent} missing). "
                 "Has the tag checked in to the AP yet?")
    return did, mac


ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "weather_assets")
ASSET_URL = ("https://raw.githubusercontent.com/OpenEPaperLink/Home_Assistant_Integration/"
             "main/custom_components/open_epaper_link/imagegen/assets/")


def render(payload):
    """Local emulation of the integration's drawcustom for the element types used here."""
    from PIL import Image, ImageDraw, ImageFont
    os.makedirs(ASSETS, exist_ok=True)
    for f in ("ppb.ttf", "rbm.ttf", "materialdesignicons-webfont.ttf", "materialdesignicons-webfont_meta.json"):
        if not os.path.exists(os.path.join(ASSETS, f)):
            urllib.request.urlretrieve(ASSET_URL + f, os.path.join(ASSETS, f))
    colors = {"black": (0, 0, 0), "white": (255, 255, 255), "red": (255, 0, 0)}
    mdi = {i["name"]: chr(int(i["codepoint"], 16))
           for i in json.load(open(os.path.join(ASSETS, "materialdesignicons-webfont_meta.json")))}
    font = lambda name, size: ImageFont.truetype(os.path.join(ASSETS, name), size)
    img = Image.new("RGB", (W, H), "white")
    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    for el in payload:
        t = el["type"]
        if t == "text":
            f = font(el.get("font", "ppb.ttf"), el["size"])
            s = str(el["value"])
            if el.get("max_width"):                      # the integration's word wrap
                lines, cur = [], []
                for w in s.split():
                    test = " ".join(cur + [w])
                    if not cur or d.textlength(test, font=f) <= el["max_width"]:
                        cur.append(w)
                    else:
                        lines.append(" ".join(cur)); cur = [w]
                if cur:
                    lines.append(" ".join(cur))
                s = "\n".join(lines)
            d.text((el["x"], el["y"]), s, fill=colors[el.get("color", "black")], font=f,
                   anchor=el.get("anchor", "la"), align=el.get("align", "left"), spacing=el.get("spacing", 5))
        elif t == "icon":
            d.text((el["x"], el["y"]), mdi[el["value"]], fill=colors[el.get("color", "black")],
                   font=font("materialdesignicons-webfont.ttf", el["size"]), anchor=el.get("anchor", "la"))
        elif t == "line":
            d.line([(el["x_start"], el["y_start"]), (el["x_end"], el["y_end"])],
                   fill=colors[el.get("fill", "black")], width=el.get("width", 1))
        elif t == "rectangle":
            d.rectangle((el["x_start"], el["y_start"], el["x_end"], el["y_end"]),
                        fill=colors.get(el.get("fill")), outline=colors.get(el.get("outline", "black")),
                        width=el.get("width", 1))
    return img


def cmd_preview(out_prefix):
    """Render the shortest, a middling and the longest quote to PNGs."""
    qs = sorted(QUOTES, key=len)
    for tag, q in (("short", qs[0]), ("mid", qs[len(qs) // 2]), ("long", qs[-1])):
        path = f"{out_prefix}_{tag}.png"
        render(build_payload(q, size_for(q))).save(path)
        print(path, len(q), "chars, size", size_for(q))


def parse(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    cmd, mac, hours, alias = argv[0], argv[1], 4, None
    rest = argv[2:]
    while rest:
        k = rest.pop(0)
        if k == "--hours":
            hours = int(rest.pop(0))
        elif k == "--alias":
            alias = rest.pop(0)
        else:
            sys.exit(f"unknown option {k}")
    return cmd, mac, hours, alias


def main():
    if sys.argv[1:2] == ["preview"]:
        return cmd_preview(sys.argv[2] if len(sys.argv) > 2 else "/tmp/deep_thought")
    cmd, mac, hours, alias = parse(sys.argv[1:])
    device_id, mac = device_id_for(mac)
    if cmd == "yaml":
        import yaml
        print(yaml.safe_dump(build_automation(device_id, hours), sort_keys=False, width=110, allow_unicode=True))
    elif cmd == "install":
        if input(f"Write HA automation '{AUTOMATION_ID}' for {mac.upper()} (every {hours} h)? [y/N] ").strip().lower() != "y":
            return 1
        print(ha(f"/api/config/automation/config/{AUTOMATION_ID}", build_automation(device_id, hours)))
        if alias:
            try:
                ha("/api/services/text/set_value", {"entity_id": f"text.{mac}_alias", "value": alias})
                print(f"alias set to {alias}")
            except OSError as e:   # the integration relays this to the AP and can be slow to answer
                print(f"alias request sent but HA did not answer in time ({e}); check the AP's tag list")
        print("installed; HA reloads automations automatically. Run `show` to put the first quote up.")
    elif cmd == "show":
        ent = ha("/api/template", {"template": "{{ states.automation | selectattr('attributes.id', 'eq', '%s') "
                                               "| map(attribute='entity_id') | first }}" % AUTOMATION_ID})
        if not ent or not ent.startswith("automation."):
            sys.exit(f"automation {AUTOMATION_ID} not found in HA; run install first")
        ha("/api/services/automation/trigger", {"entity_id": ent})
        print(f"triggered {ent}; the tag picks it up at its next check-in (up to a minute)")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main() or 0)
