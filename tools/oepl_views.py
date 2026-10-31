#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Nathan Bigelow
"""Hosted OEPL views: render weather and Deep Thoughts images and publish them.

Anyone with an OpenEPaperLink AP points a tag at one of the published URLs
using the AP's "Image URL" content mode (mode 7) and an interval; the AP
fetches and converts the picture itself. Nothing to install on their side.

    tools/oepl_views.py render [--out DIR] [--only weather|quotes]   render all images to DIR (default ./out)
    tools/oepl_views.py publish [--only ...]                          render and upload to the R2 bucket
    tools/oepl_views.py list                                          print the published URLs

Views (file names are what users paste after the base URL):
    weather-berkeley-6in.jpg   600x448 BWR     weather-oakland-6in.jpg
    weather-berkeley-29in.jpg  384x168 BWRY    weather-oakland-29in.jpg
    weather-berkeley-16in.jpg  200x200 BWRY    weather-oakland-16in.jpg
    deepthought-6in.jpg        600x448 BWR     deepthought-29in.jpg   384x168 BWRY
(baseline JPEG: the AP's Image URL mode decodes nothing else)

Weather: Open-Meteo (free, no key), US units, America/Los_Angeles.
Quotes: deep_thoughts.txt next to this file, one per line; the pick changes
every QUOTE_HOURS so every fetch in that window sees the same quote.

Config via environment (or ~/secrets.toml [cloudflare] when run on the dev Mac):
    R2_ENDPOINT, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, R2_BUCKET (default oepl),
    PUBLIC_BASE (default https://pub-27b52fd49cbf48ebaf32761165fe1614.r2.dev)
"""
import datetime as dt
import hashlib
import io
import json
import os
import sys
import urllib.request
import zoneinfo

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "weather_assets")
ASSET_URL = ("https://raw.githubusercontent.com/OpenEPaperLink/Home_Assistant_Integration/"
             "main/custom_components/open_epaper_link/imagegen/assets/")
TZ = zoneinfo.ZoneInfo("America/Los_Angeles")
QUOTE_HOURS = 4
PUBLIC_BASE = os.environ.get("PUBLIC_BASE", "https://pub-27b52fd49cbf48ebaf32761165fe1614.r2.dev").rstrip("/")

PLACES = {"berkeley": ("Berkeley", 37.8716, -122.2727), "oakland": ("Oakland", 37.8044, -122.2712)}
BLACK, WHITE, RED, YELLOW = (0, 0, 0), (255, 255, 255), (255, 0, 0), (255, 255, 0)

# WMO weather code -> (MDI icon, short text)
WMO = {0: ("weather-sunny", "Clear"), 1: ("weather-sunny", "Mostly Clear"), 2: ("weather-partly-cloudy", "Partly Cloudy"),
       3: ("weather-cloudy", "Cloudy"), 45: ("weather-fog", "Fog"), 48: ("weather-fog", "Rime Fog"),
       51: ("weather-rainy", "Light Drizzle"), 53: ("weather-rainy", "Drizzle"), 55: ("weather-rainy", "Heavy Drizzle"),
       56: ("weather-snowy-rainy", "Freezing Drizzle"), 57: ("weather-snowy-rainy", "Freezing Drizzle"),
       61: ("weather-rainy", "Light Rain"), 63: ("weather-rainy", "Rain"), 65: ("weather-pouring", "Heavy Rain"),
       66: ("weather-snowy-rainy", "Freezing Rain"), 67: ("weather-snowy-rainy", "Freezing Rain"),
       71: ("weather-snowy", "Light Snow"), 73: ("weather-snowy", "Snow"), 75: ("weather-snowy-heavy", "Heavy Snow"),
       77: ("weather-snowy", "Snow Grains"), 80: ("weather-rainy", "Showers"), 81: ("weather-rainy", "Showers"),
       82: ("weather-pouring", "Heavy Showers"), 85: ("weather-snowy", "Snow Showers"), 86: ("weather-snowy-heavy", "Snow Showers"),
       95: ("weather-lightning", "Thunderstorm"), 96: ("weather-lightning-rainy", "Thunderstorm"), 99: ("weather-lightning-rainy", "Thunderstorm")}


def wmo(code, night=False):
    icon, txt = WMO.get(int(code), ("weather-cloudy", "Cloudy"))
    if night and icon == "weather-sunny":
        icon, txt = "weather-night", "Clear"
    if night and icon == "weather-partly-cloudy":
        icon = "weather-night-partly-cloudy"
    return icon, txt


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

def fetch_weather(lat, lon):
    q = ("https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
         "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m,is_day,uv_index"
         "&hourly=temperature_2m,weather_code,precipitation,is_day"
         "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,sunrise,sunset,uv_index_max"
         "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch"
         "&timezone=America%%2FLos_Angeles&forecast_days=6" % (lat, lon))
    with urllib.request.urlopen(q, timeout=30) as r:
        d = json.load(r)
    now = dt.datetime.now(TZ).replace(minute=0, second=0, microsecond=0)
    hours = d["hourly"]
    idx = next((i for i, t in enumerate(hours["time"]) if dt.datetime.fromisoformat(t).replace(tzinfo=TZ) >= now), 0)
    hourly = [{"time": dt.datetime.fromisoformat(hours["time"][i]).replace(tzinfo=TZ), "temp": hours["temperature_2m"][i],
               "code": hours["weather_code"][i], "precip": hours["precipitation"][i], "day": hours["is_day"][i]}
              for i in range(idx, min(idx + 24, len(hours["time"])))]
    dd = d["daily"]
    daily = [{"date": dt.date.fromisoformat(dd["time"][i]), "hi": dd["temperature_2m_max"][i], "lo": dd["temperature_2m_min"][i],
              "code": dd["weather_code"][i], "precip": dd["precipitation_sum"][i],
              "sunrise": dt.datetime.fromisoformat(dd["sunrise"][i]), "sunset": dt.datetime.fromisoformat(dd["sunset"][i]),
              "uv": dd["uv_index_max"][i]} for i in range(len(dd["time"]))]
    c = d["current"]
    return {"now": dt.datetime.now(TZ), "temp": c["temperature_2m"], "humidity": c["relative_humidity_2m"], "code": c["weather_code"],
            "wind": c["wind_speed_10m"], "wind_dir": c["wind_direction_10m"], "day": c["is_day"], "uv": c["uv_index"],
            "hourly": hourly, "daily": daily}


def load_quotes():
    path = os.path.join(HERE, "deep_thoughts.txt")
    return [ln.strip() for ln in open(path) if ln.strip() and not ln.lstrip().startswith("#")]


def pick_quote(quotes, when=None):
    when = when or dt.datetime.now(TZ)
    slot = int(when.timestamp() // (QUOTE_HOURS * 3600))
    return quotes[int(hashlib.sha1(str(slot).encode()).hexdigest(), 16) % len(quotes)]


# ---------------------------------------------------------------------------
# Drawing (same fonts as the OEPL Home Assistant integration)
# ---------------------------------------------------------------------------

class Canvas:
    def __init__(self, w, h, accent2=None):
        from PIL import Image, ImageDraw, ImageFont
        os.makedirs(ASSETS, exist_ok=True)
        for f in ("ppb.ttf", "rbm.ttf", "materialdesignicons-webfont.ttf", "materialdesignicons-webfont_meta.json"):
            if not os.path.exists(os.path.join(ASSETS, f)):
                urllib.request.urlretrieve(ASSET_URL + f, os.path.join(ASSETS, f))
        self.mdi = {i["name"]: chr(int(i["codepoint"], 16))
                    for i in json.load(open(os.path.join(ASSETS, "materialdesignicons-webfont_meta.json")))}
        self.W, self.H = w, h
        self.yellow = accent2            # None on a BWR panel: yellow falls back to red
        self.img = Image.new("RGB", (w, h), WHITE)
        self.d = ImageDraw.Draw(self.img)
        self.d.fontmode = "1"
        self._fonts = {}
        self._ImageFont = ImageFont

    def font(self, name, size):
        key = (name, int(size))
        if key not in self._fonts:
            self._fonts[key] = self._ImageFont.truetype(os.path.join(ASSETS, name), max(6, int(size)))
        return self._fonts[key]

    def color(self, c):
        if c == "yellow":
            return self.yellow or RED
        return {"black": BLACK, "red": RED, "white": WHITE}.get(c, c)

    def text(self, s, x, y, size, anchor="la", color="black", font="rbm.ttf", max_width=None):
        f = self.font(font, size)
        s = str(s)
        if max_width:
            while len(s) > 1 and self.d.textlength(s, font=f) > max_width:
                s = s[:-1]
        self.d.text((x, y), s, fill=self.color(color), font=f, anchor=anchor)

    def wrap(self, s, x, y, size, width, anchor="mm", color="black", font="rbm.ttf", spacing=6, align="center"):
        f = self.font(font, size)
        lines, cur = [], []
        for w in s.split():
            t = " ".join(cur + [w])
            if not cur or self.d.textlength(t, font=f) <= width:
                cur.append(w)
            else:
                lines.append(" ".join(cur)); cur = [w]
        if cur:
            lines.append(" ".join(cur))
        self.d.multiline_text((x, y), "\n".join(lines), fill=self.color(color), font=f, anchor=anchor, spacing=spacing, align=align)
        return len(lines)

    def icon(self, name, x, y, size, anchor="mm", color="black"):
        self.d.text((x, y), self.mdi.get(name, self.mdi["help-circle-outline"]), fill=self.color(color),
                    font=self.font("materialdesignicons-webfont.ttf", size), anchor=anchor)

    def line(self, x0, y0, x1, y1, width=1, color="black"):
        self.d.line([(x0, y0), (x1, y1)], fill=self.color(color), width=width)

    def rect(self, x0, y0, x1, y1, outline="black", width=1, fill=None):
        self.d.rectangle([x0, y0, x1, y1], outline=self.color(outline), width=width, fill=self.color(fill) if fill else None)

    def jpeg(self):
        # The AP's Image URL mode decodes baseline JPEG only (it saves the download as temp.jpg).
        # Quality 95 with no chroma subsampling keeps the red/yellow areas clean for the dither.
        buf = io.BytesIO()
        self.img.save(buf, "JPEG", quality=95, subsampling=0, optimize=True, progressive=False)
        return buf.getvalue()


def compass(deg):
    return ["N", "NE", "E", "SE", "S", "SW", "W", "NW"][int(round(deg / 45)) % 8]


def hour_label(t):
    return t.strftime("%-I%p").lower()


# ---------------------------------------------------------------------------
# Weather layouts
# ---------------------------------------------------------------------------

def weather_6in(name, w):
    c = Canvas(600, 448)
    L, R = 16, 584
    c.line(0, 3, 600, 3, width=6, color="red")
    ic, txt = wmo(w["code"], not w["day"])
    c.icon(ic, 82, 84, 120)
    c.text("%d°" % round(w["temp"]), 160, 118, 104, anchor="ls", font="ppb.ttf")
    c.text(txt, 164, 126, 26, anchor="la", max_width=250)
    today = w["daily"][0]
    c.icon("arrow-up-bold", 366, 66, 22, color="red")
    c.text("%d°" % round(today["hi"]), 386, 66, 30, anchor="lm", color="red", font="ppb.ttf")
    c.icon("arrow-down-bold", 366, 102, 22)
    c.text("%d°" % round(today["lo"]), 386, 102, 30, anchor="lm", font="ppb.ttf")
    c.text(w["now"].strftime("%A, %b %-d"), R, 14, 22, anchor="ra", font="ppb.ttf")
    c.text(name, R, 40, 18, anchor="ra", color="red")
    cx_icon, x_txt, y = 452, 470, 66
    for icn, val in (("water-percent", "%d%% humidity" % w["humidity"]),
                     ("weather-windy", "%d mph %s" % (round(w["wind"]), compass(w["wind_dir"]))),
                     ("white-balance-sunny", "UV index %d" % round(today["uv"] or 0))):
        c.icon(icn, cx_icon, y, 22); c.text(val, x_txt, y, 20, anchor="lm"); y += 28
    c.icon("weather-sunset-up", cx_icon, y, 22); c.text(today["sunrise"].strftime("%-I:%M"), x_txt, y, 20, anchor="lm")
    c.icon("weather-sunset-down", cx_icon + 68, y, 22); c.text(today["sunset"].strftime("%-I:%M"), x_txt + 68, y, 20, anchor="lm")
    c.line(L, 162, R, 162)
    cols, cw = 8, (R - L) / 8
    for i in range(cols):
        h = w["hourly"][min(2 * i, len(w["hourly"]) - 1)]
        cx = int(L + cw * (i + 0.5))
        c.text(hour_label(h["time"]), cx, 174, 17, anchor="mt")
        c.icon(wmo(h["code"], not h["day"])[0], cx, 218, 42)
        c.text("%d°" % round(h["temp"]), cx, 244, 24, anchor="mt", font="ppb.ttf")
        if h["precip"] > 0:
            c.text('%.2f"' % h["precip"], cx, 274, 14, anchor="mt", color="red")
    c.line(L, 296, R, 296)
    cols, cw = 5, (R - L) / 5
    for i in range(cols):
        dday = w["daily"][i]
        cx = int(L + cw * (i + 0.5))
        c.text("Today" if i == 0 else dday["date"].strftime("%a"), cx, 305, 22, anchor="mt", font="ppb.ttf")
        c.icon(wmo(dday["code"])[0], cx - 26, 358, 48)
        c.text("%d°" % round(dday["hi"]), cx + 2, 344, 28, anchor="lm", color="red", font="ppb.ttf")
        c.text("%d°" % round(dday["lo"]), cx + 2, 374, 22, anchor="lm")
        if dday["precip"] > 0:
            c.text('%.2f" rain' % dday["precip"], cx, 398, 14, anchor="mt", color="red")
    c.line(L, 418, R, 418)
    c.text("Updated " + w["now"].strftime("%-I:%M %p"), L, 440, 14, anchor="ls")
    c.text("Forecast: Open-Meteo", R, 440, 14, anchor="rs")
    return c


def weather_29in(name, w):
    c = Canvas(384, 168, accent2=YELLOW)
    c.line(0, 2, 384, 2, width=4, color="red")
    ic, txt = wmo(w["code"], not w["day"])
    c.icon(ic, 46, 52, 72)
    c.text("%d°" % round(w["temp"]), 92, 72, 64, anchor="ls", font="ppb.ttf")
    c.text(txt, 94, 76, 17, anchor="la", max_width=120)
    today = w["daily"][0]
    c.icon("arrow-up-bold", 218, 44, 16, color="red"); c.text("%d°" % round(today["hi"]), 232, 44, 22, anchor="lm", color="red", font="ppb.ttf")
    c.icon("arrow-down-bold", 218, 70, 16); c.text("%d°" % round(today["lo"]), 232, 70, 22, anchor="lm", font="ppb.ttf")
    c.text(w["now"].strftime("%a, %b %-d"), 378, 10, 16, anchor="ra", font="ppb.ttf")
    c.text(name, 378, 30, 14, anchor="ra", color="red")
    c.icon("weather-sunset-up", 290, 58, 16, color="yellow"); c.text(today["sunrise"].strftime("%-I:%M"), 302, 58, 14, anchor="lm")
    c.icon("weather-sunset-down", 290, 78, 16, color="yellow"); c.text(today["sunset"].strftime("%-I:%M"), 302, 78, 14, anchor="lm")
    c.line(8, 98, 376, 98)
    cols, cw = 4, 368 / 4
    for i in range(cols):
        dday = w["daily"][i]
        cx = int(8 + cw * (i + 0.5))
        c.text("Today" if i == 0 else dday["date"].strftime("%a"), cx, 104, 15, anchor="mt", font="ppb.ttf")
        c.icon(wmo(dday["code"])[0], cx - 22, 140, 36, color="yellow" if wmo(dday["code"])[0] == "weather-sunny" else "black")
        c.text("%d°" % round(dday["hi"]), cx + 2, 130, 20, anchor="lm", color="red", font="ppb.ttf")
        c.text("%d°" % round(dday["lo"]), cx + 2, 152, 16, anchor="lm")
    return c


def weather_16in(name, w):
    c = Canvas(200, 200, accent2=YELLOW)
    c.line(0, 2, 200, 2, width=4, color="red")
    c.text(name, 6, 10, 14, anchor="la", color="red", font="ppb.ttf")
    c.text(w["now"].strftime("%a %-d"), 194, 10, 14, anchor="ra", font="ppb.ttf")
    ic, txt = wmo(w["code"], not w["day"])
    c.icon(ic, 46, 72, 72, color="yellow" if ic in ("weather-sunny", "weather-partly-cloudy") else "black")
    c.text("%d°" % round(w["temp"]), 194, 92, 58, anchor="rs", font="ppb.ttf")
    c.text(txt, 100, 112, 15, anchor="mm", max_width=190)
    today = w["daily"][0]
    c.icon("arrow-up-bold", 36, 136, 14, color="red"); c.text("%d°" % round(today["hi"]), 48, 136, 18, anchor="lm", color="red", font="ppb.ttf")
    c.icon("arrow-down-bold", 108, 136, 14); c.text("%d°" % round(today["lo"]), 120, 136, 18, anchor="lm", font="ppb.ttf")
    c.line(6, 152, 194, 152)
    for i, x in ((1, 50), (2, 150)):
        dday = w["daily"][i]
        c.text(dday["date"].strftime("%a"), x, 158, 13, anchor="mt", font="ppb.ttf")
        c.icon(wmo(dday["code"])[0], x - 24, 184, 26)
        c.text("%d°" % round(dday["hi"]), x - 6, 178, 15, anchor="lm", color="red", font="ppb.ttf")
        c.text("%d°" % round(dday["lo"]), x - 6, 193, 13, anchor="lm")
    return c


# ---------------------------------------------------------------------------
# Deep Thoughts layouts (same look as the HA automation version)
# ---------------------------------------------------------------------------

def quote_size(q, table):
    return next((px, sp) for n, px, sp in table if len(q) <= n)


def deepthought_6in(q):
    c = Canvas(600, 448)
    c.rect(10, 10, 589, 437, outline="black", width=2)
    c.rect(16, 16, 583, 431, outline="red", width=1)
    c.text("DEEP THOUGHTS", 300, 44, 26, anchor="mm", font="ppb.ttf")
    c.text("by Jack Handey", 300, 70, 18, anchor="mm", color="red")
    c.line(200, 88, 400, 88, width=2, color="red")
    c.icon("format-quote-open", 28, 100, 64, anchor="lt", color="red")
    c.icon("format-quote-close", 572, 396, 64, anchor="rb", color="red")
    px, sp = quote_size(q, [(90, 40, 8), (150, 34, 8), (220, 30, 8), (300, 27, 6), (380, 24, 5), (460, 22, 4), (10**9, 20, 3)])
    c.wrap(q, 300, 246, px, 470, spacing=sp)
    return c


def deepthought_29in(q):
    c = Canvas(384, 168, accent2=YELLOW)
    c.rect(4, 4, 379, 163, outline="black", width=2)
    c.rect(8, 8, 375, 159, outline="red", width=1)
    c.text("DEEP THOUGHTS", 192, 20, 15, anchor="mm", font="ppb.ttf")
    c.text("by Jack Handey", 192, 34, 11, anchor="mm", color="red")
    c.icon("format-quote-open", 14, 42, 30, anchor="lt", color="yellow")
    c.icon("format-quote-close", 370, 158, 30, anchor="rb", color="yellow")
    px, sp = quote_size(q, [(90, 20, 4), (150, 17, 3), (220, 15, 3), (300, 13, 2), (400, 12, 2), (10**9, 11, 1)])
    c.wrap(q, 192, 100, px, 330, spacing=sp)
    return c


# ---------------------------------------------------------------------------
# Render / publish
# ---------------------------------------------------------------------------

def render_all(only=None):
    out = {}
    if only in (None, "weather"):
        for key, (name, lat, lon) in PLACES.items():
            w = fetch_weather(lat, lon)
            out[f"weather-{key}-6in.jpg"] = weather_6in(name, w).jpeg()
            out[f"weather-{key}-29in.jpg"] = weather_29in(name, w).jpeg()
            out[f"weather-{key}-16in.jpg"] = weather_16in(name, w).jpeg()
    if only in (None, "quotes"):
        q = pick_quote(load_quotes())
        out["deepthought-6in.jpg"] = deepthought_6in(q).jpeg()
        out["deepthought-29in.jpg"] = deepthought_29in(q).jpeg()
    return out


def r2_client():
    import boto3
    cfg = {k: os.environ.get(k) for k in ("R2_ENDPOINT", "R2_ACCESS_KEY_ID", "R2_SECRET_ACCESS_KEY")}
    if not all(cfg.values()):
        import tomllib
        cf = tomllib.load(open(os.path.expanduser("~/secrets.toml"), "rb"))["cloudflare"]
        cfg = {"R2_ENDPOINT": cf["s3_api_endpoint"], "R2_ACCESS_KEY_ID": cf["access_key_id"], "R2_SECRET_ACCESS_KEY": cf["secret_access_key"]}
    return boto3.client("s3", endpoint_url=cfg["R2_ENDPOINT"], aws_access_key_id=cfg["R2_ACCESS_KEY_ID"],
                        aws_secret_access_key=cfg["R2_SECRET_ACCESS_KEY"], region_name="auto")


def publish(only=None):
    s3 = r2_client()
    bucket = os.environ.get("R2_BUCKET", "oepl")
    for name, data in render_all(only).items():
        s3.put_object(Bucket=bucket, Key=name, Body=data, ContentType="image/jpeg", CacheControl="public, max-age=300")
        print(f"{dt.datetime.now(TZ):%H:%M} uploaded {name} ({len(data)} bytes) -> {PUBLIC_BASE}/{name}")


def main(argv):
    cmd = argv[0] if argv else "render"
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    if cmd == "render":
        outdir = argv[argv.index("--out") + 1] if "--out" in argv else "out"
        os.makedirs(outdir, exist_ok=True)
        for name, data in render_all(only).items():
            open(os.path.join(outdir, name), "wb").write(data); print("wrote", os.path.join(outdir, name))
    elif cmd == "publish":
        publish(only)
    elif cmd == "list":
        for n in ("weather-berkeley-6in", "weather-berkeley-29in", "weather-berkeley-16in", "weather-oakland-6in",
                  "weather-oakland-29in", "weather-oakland-16in", "deepthought-6in", "deepthought-29in"):
            print(f"{PUBLIC_BASE}/{n}.jpg")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
