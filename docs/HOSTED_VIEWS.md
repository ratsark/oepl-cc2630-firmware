# Hosted views: weather and Deep Thoughts for any OpenEPaperLink AP

Ready-made pictures for e-paper tags, refreshed on a schedule and served from
`https://oepl.ratsark.com/`. You don't install anything: your access point
fetches the picture itself.

## Using a view

1. In your AP's web UI, click the tag.
2. Set the content mode to **Image URL**.
3. Paste one of the URLs below. Pick the one that matches your tag's size.
4. Set the **interval** to **5** (minutes). The AP downloads the picture every
   5 minutes and only sends it to the tag when it has actually changed, so the
   tag's battery is not affected by polling; the picture itself changes on the
   schedule in the table.
5. Save. The tag updates at its next check-in.

| View | Tag | URL | Picture changes |
|------|-----|-----|-----------------|
| Berkeley weather | 6" 600x448 BWR (Solum M3 6.0", hwType 0x35) | `https://oepl.ratsark.com/weather-berkeley-6in.jpg` | hourly |
| Berkeley weather | 2.9" 384x168 BWRY (Solum M3 2.9" BWRY) | `https://oepl.ratsark.com/weather-berkeley-29in.jpg` | hourly |
| Berkeley weather | 1.6" 200x200 BWRY (Solum M3 1.6" BWRY) | `https://oepl.ratsark.com/weather-berkeley-16in.jpg` | hourly |
| Oakland weather | 6" | `https://oepl.ratsark.com/weather-oakland-6in.jpg` | hourly |
| Oakland weather | 2.9" | `https://oepl.ratsark.com/weather-oakland-29in.jpg` | hourly |
| Oakland weather | 1.6" | `https://oepl.ratsark.com/weather-oakland-16in.jpg` | hourly |
| Deep Thought (Jack Handey) | 6" | `https://oepl.ratsark.com/deepthought-6in.jpg` | every 4 hours |
| Deep Thought (Jack Handey) | 2.9" | `https://oepl.ratsark.com/deepthought-29in.jpg` | every 4 hours |

The AP rotates and dithers the picture for the panel, so a BWRY picture on a
BWR tag simply loses the yellow. A picture of the wrong size will be scaled
and look poor; use the one for your panel.

Weather is from [Open-Meteo](https://open-meteo.com/), in °F and mph, Pacific
time. The current conditions, today's high and low, humidity, wind, UV, sunrise
and sunset, an hourly strip (6" only) and a multi-day outlook: 5 days on the
6", 4 on the 2.9", 2 on the 1.6".

## How it runs

`tools/oepl_views.py` renders every view with Pillow, using the same fonts and
icon set as the OpenEPaperLink Home Assistant integration, and uploads them
to a Cloudflare R2 bucket (`oepl`) that is served at `oepl.ratsark.com`.
Pictures are baseline JPEGs because that is the only format the AP's Image
URL mode decodes.

It runs from cron on a home server in a small Docker container:

```
5 * * * *    run_views.sh --only weather     # Berkeley + Oakland, 3 sizes each
10 */4 * * * run_views.sh --only quotes      # Deep Thought, 6" and 2.9"
```

The Deep Thought is chosen deterministically from the 4-hour time slot, so
every fetch within a slot sees the same quote and the two sizes agree.
The quotes file (`tools/deep_thoughts.txt`) is not in this repo: it is Jack
Handey's copyrighted work, kept privately.

To run it elsewhere: `pip install pillow boto3`, set `R2_ENDPOINT`,
`R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY` (an R2 API token with object
write), optionally `R2_BUCKET` and `PUBLIC_BASE`, and call
`tools/oepl_views.py publish`. `tools/oepl_views.py render --out DIR` writes
the pictures locally for checking a layout change, with no credentials.

## Adding a view

Add a layout function in `tools/oepl_views.py` that returns a `Canvas`, and
list its file name in `render_all()`. For another city, add it to `PLACES`
with its coordinates; the three weather sizes follow automatically.
