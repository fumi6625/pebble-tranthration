#!/usr/bin/env python3
"""Generate Pebble app store / CloudPebble image assets (emery 200x228).

The screenshot renderer mirrors the layout maths in src/main.c so the store
images stay in step with what the watch actually draws.
"""

from PIL import Image, ImageDraw, ImageFont
import math, os

# ── SVG face data (identical to SVG_FX/SVG_FY in main.c) ──────────────────
SVG_FX = [70,81,88,94,96,97,96,92,87,80,70,54,43,40,47,32,47,49,53,61]
SVG_FY = [22,26,33,41,51,61,71,80,87,93,98,99,93,84,73,62,53,41,31,24]

# ── Layout constants (must match main.c) ──────────────────────────────────
W, H      = 200, 228
STATUS_H  = 24
RAIL_W    = 34
CHIP      = 26

# ── Colour system (must match main.c) ─────────────────────────────────────
BG_JPEN   = ( 85, 170, 255)
BG_ENJP   = (255, 160,  50)
BAR_JPEN  = (  0,  85, 170)
BAR_ENJP  = (180,  95,   0)
INK       = (  0,  40,  90)
ACCENT    = (  0, 110, 200)
YELLOW    = (255, 255,   0)
WHITE     = (255, 255, 255)
BLACK     = (  0,   0,   0)
RED       = (255,   0,   0)

FONT_JP   = "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf"
FONT_SANS = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"


def load_font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except Exception:
        return ImageFont.load_default()


def text_w(draw, s, font):
    try:
        bb = draw.textbbox((0, 0), s, font=font)
        return bb[2] - bb[0]
    except Exception:
        return len(s) * font.size // 2


def centre_text(draw, s, font, x, y, w, fill):
    draw.text((x + (w - text_w(draw, s, font)) // 2, y), s, font=font, fill=fill)


# ── Face ──────────────────────────────────────────────────────────────────
def face_geometry(mirror):
    """Return (polygon, eye, mouth, bottom_y) exactly as build_layout does."""
    sn, sd  = H * 88 // 228, 77
    face_cy = STATUS_H + (H - STATUS_H) * 36 // 100
    face_cx = (W - RAIL_W) // 2
    ox = face_cx - 64 * sn // sd
    oy = face_cy - 60 * sn // sd

    def fx(v):
        x = v * sn // sd + ox
        return (2 * face_cx - x) if mirror else x

    def fy(v):
        return v * sn // sd + oy

    pts, bot = [], 0
    for i in range(20):
        px, py = fx(SVG_FX[i]), fy(SVG_FY[i])
        pts.append((px, py))
        bot = max(bot, py)

    eye   = (fx(62), fy(48), max(2, 4 * sn // sd))
    mouth = (fx(42), fy(78), max(4, 8 * sn // sd), max(2, 4 * sn // sd))
    return pts, eye, mouth, bot


def draw_face(d, mirror, mouth_fill):
    pts, eye, mouth, _ = face_geometry(mirror)
    for dx in range(-1, 3):
        for dy in range(-1, 3):
            d.polygon([(x + dx, y + dy) for x, y in pts], outline=YELLOW)
    ex, ey, er = eye
    d.ellipse((ex - er, ey - er, ex + er, ey + er), fill=YELLOW)
    mx, my, ro, ri = mouth
    d.ellipse((mx - ro, my - ro, mx + ro, my + ro), fill=YELLOW)
    d.ellipse((mx - ri, my - ri, mx + ri, my + ri), fill=mouth_fill)


# ── Chrome: status bar and button rail ────────────────────────────────────
def draw_status_bar(d, badge, bar_col, clock="12:04"):
    d.rectangle((0, 0, W, STATUS_H), fill=bar_col)
    f = load_font(FONT_JP, 18)
    pill_w = text_w(d, badge, f) + 14
    pill_h = 20
    py = (STATUS_H - pill_h) // 2
    d.rounded_rectangle((4, py, 4 + pill_w, py + pill_h), radius=5, fill=WHITE)
    centre_text(d, badge, f, 4, py - 1, pill_w, INK)
    fc = load_font(FONT_SANS, 18)
    d.text((W - 4 - text_w(d, clock, fc), py - 1), clock, font=fc, fill=WHITE)


def draw_icon(d, kind, cx, cy, col):
    if kind == "rotate":
        r = 7
        d.arc((cx - r, cy - r, cx + r, cy + r), start=-50, end=240,
              fill=col, width=2)
        d.polygon([(cx + 8, cy - 8), (cx, cy - 6), (cx + 6, cy)], fill=col)
    elif kind == "rec":
        d.ellipse((cx - 6, cy - 6, cx + 6, cy + 6), fill=col)
    elif kind == "stop":
        d.rounded_rectangle((cx - 5, cy - 5, cx + 5, cy + 5), radius=1, fill=col)
    elif kind == "list":
        for i in (-1, 0, 1):
            d.rounded_rectangle((cx - 7, cy + i * 5 - 1, cx + 7, cy + i * 5 + 1),
                                radius=1, fill=col)
    elif kind == "flip":
        d.polygon([(cx, cy - 8), (cx - 5, cy - 2), (cx + 5, cy - 2)], fill=col)
        d.polygon([(cx, cy + 8), (cx - 5, cy + 2), (cx + 5, cy + 2)], fill=col)


def draw_rail_chip(d, cy, kind, label, icon_col, bar_col):
    cx = W - RAIL_W // 2 - 1
    box = (cx - CHIP // 2, cy - CHIP // 2, cx + CHIP // 2, cy + CHIP // 2)
    d.rounded_rectangle(box, radius=6, fill=bar_col, outline=WHITE)
    draw_icon(d, kind, cx, cy, icon_col)
    if label:
        f = load_font(FONT_JP, 14)
        centre_text(d, label, f, cx - RAIL_W // 2, cy + CHIP // 2 - 1, RAIL_W, WHITE)


def draw_rail(d, state, bar_col):
    up_y, sel_y, dn_y = STATUS_H + 18, H // 2, H * 82 // 100
    if state == "listening":
        draw_rail_chip(d, up_y,  "rotate", "",   (170, 170, 170), bar_col)
        draw_rail_chip(d, sel_y, "stop",   "停止", RED, bar_col)
    elif state == "result":
        draw_rail_chip(d, up_y,  "flip",   "反転", WHITE, bar_col)
        draw_rail_chip(d, sel_y, "rec",    "録音", RED, bar_col)
    else:
        draw_rail_chip(d, up_y,  "rotate", "切替", WHITE, bar_col)
        draw_rail_chip(d, sel_y, "rec",    "録音", RED, bar_col)
    draw_rail_chip(d, dn_y, "list", "履歴", WHITE, bar_col)


# ── Screens ───────────────────────────────────────────────────────────────
def make_home(mode_ejp, path):
    bg      = BG_ENJP if mode_ejp else BG_JPEN
    bar     = BAR_ENJP if mode_ejp else BAR_JPEN
    badge   = "EN → JP" if mode_ejp else "JP → EN"
    img = Image.new("RGB", (W, H), bg)
    d   = ImageDraw.Draw(img)

    draw_face(d, mode_ejp, bar)
    _, _, _, bot = face_geometry(mode_ejp)

    lines = ["Please speak", "in English"] if mode_ejp else ["日本語で", "話して下さい"]
    f = load_font(FONT_SANS if mode_ejp else FONT_JP, 24)
    y = bot + 6
    for line in lines:
        centre_text(d, line, f, 4, y, W - RAIL_W - 8, WHITE)
        y += 30

    draw_status_bar(d, badge, bar)
    draw_rail(d, "home", bar)
    img.save(path)
    print(f"  Saved {path}")


def make_result(path, flipped=False):
    bar = BAR_JPEN
    img = Image.new("RGB", (W, H), BG_JPEN)
    d   = ImageDraw.Draw(img)

    cx, cy = 5, STATUS_H + 4
    cw, ch = W - 5 - RAIL_W, H - cy - 6

    card = Image.new("RGB", (cw, ch), WHITE)
    cd   = ImageDraw.Draw(card)
    msg  = "Nice to meet you."
    f    = load_font(FONT_SANS, 28)
    words, lines, cur = msg.split(), [], ""
    for wd in words:
        trial = (cur + " " + wd).strip()
        if text_w(cd, trial, f) <= cw - 12:
            cur = trial
        else:
            lines.append(cur); cur = wd
    if cur:
        lines.append(cur)
    ty = (ch - len(lines) * 32) // 2
    for line in lines:
        centre_text(cd, line, f, 0, ty, cw, INK)
        ty += 32
    if flipped:
        card = card.rotate(180)
    mask = Image.new("L", (cw, ch), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, cw - 1, ch - 1), radius=9, fill=255)
    img.paste(card, (cx, cy), mask)

    # Flip hint pill — drawn after the rotation so it always reads upright
    pw, ph = 50, 18
    px, py = cx + cw - pw - 6, cy + 6
    d.rounded_rectangle((px, py, px + pw, py + ph), radius=5, fill=ACCENT)
    draw_icon(d, "flip", px + 11, py + ph // 2, WHITE)
    d.text((px + 20, py + 1), "反転", font=load_font(FONT_JP, 14), fill=WHITE)

    draw_status_bar(d, "JP → EN", bar)
    draw_rail(d, "result", bar)
    img.save(path)
    print(f"  Saved {path}")


def make_listening(path):
    bar = BAR_JPEN
    img = Image.new("RGB", (W, H), BG_JPEN)
    d   = ImageDraw.Draw(img)

    cc     = (W - RAIL_W) // 2
    mic_cy = STATUS_H + (H - STATUS_H) * 30 // 100
    r      = 24
    d.ellipse((cc - r, mic_cy - r, cc + r, mic_cy + r), outline=WHITE, width=3)
    bw, bh = r * 4 // 9, r
    d.rounded_rectangle((cc - bw // 2, mic_cy - bh * 3 // 5,
                         cc + bw // 2, mic_cy - bh * 3 // 5 + bh),
                        radius=bw // 2, fill=WHITE)
    d.arc((cc - bw, mic_cy - bh // 4, cc + bw, mic_cy - bh // 4 + bh * 3 // 4 + 4),
          start=0, end=180, fill=WHITE, width=2)
    d.line((cc, mic_cy + bh // 2, cc, mic_cy + bh * 4 // 5), fill=WHITE, width=2)

    centre_text(d, "聞き取り中…", load_font(FONT_JP, 24), 4, mic_cy + 34,
                W - RAIL_W - 8, WHITE)

    bars = [5, 9, 14, 18, 20, 18, 14, 9, 5, 4, 6, 10, 14]
    x0, base = cc - (13 * 7 - 3) // 2, H - 18
    for i, bh2 in enumerate(bars):
        d.rounded_rectangle((x0 + i * 7, base - bh2, x0 + i * 7 + 4, base),
                            radius=1, fill=WHITE)

    draw_status_bar(d, "JP → EN", bar)
    draw_rail(d, "listening", bar)
    img.save(path)
    print(f"  Saved {path}")


def make_log(path):
    bar = BAR_JPEN
    img = Image.new("RGB", (W, H), BG_JPEN)
    d   = ImageDraw.Draw(img)
    draw_status_bar(d, "履歴", bar)

    items  = [("08:12", "Nice to meet you."), ("08:05", "Where is the station?")]
    lm     = 6
    card_h = (H - STATUS_H - 18) // 2
    y      = STATUS_H + 6
    ft     = load_font(FONT_SANS, 18)
    fb     = load_font(FONT_SANS, 18)
    for ts, msg in items:
        d.rounded_rectangle((lm, y, W - lm, y + card_h), radius=8, fill=WHITE)
        d.text((lm + 8, y + 3), ts, font=ft, fill=ACCENT)
        words, lines, cur = msg.split(), [], ""
        for wd in words:
            trial = (cur + " " + wd).strip()
            if text_w(d, trial, fb) <= W - lm * 2 - 16:
                cur = trial
            else:
                lines.append(cur); cur = wd
        if cur:
            lines.append(cur)
        ty = y + 24
        for line in lines:
            d.text((lm + 8, ty), line, font=fb, fill=INK)
            ty += 22
        y += card_h + 6
    img.save(path)
    print(f"  Saved {path}")


# ── Store icons ───────────────────────────────────────────────────────────
def make_menu_image(path):
    img = Image.new("RGB", (25, 25), WHITE)
    d   = ImageDraw.Draw(img)
    sn, sd = 18, 77
    ox, oy = 12 - 64 * sn // sd, 11 - 60 * sn // sd
    pts = [(SVG_FX[i] * sn // sd + ox, SVG_FY[i] * sn // sd + oy) for i in range(20)]
    d.polygon(pts, fill=BLACK, outline=BLACK)
    ex, ey = 62 * sn // sd + ox, 48 * sn // sd + oy
    d.ellipse((ex - 1, ey - 1, ex + 1, ey + 1), fill=WHITE)
    img.save(path)
    print(f"  Saved {path}")


def make_icon(size, path):
    """The home-screen face, scaled up to own the whole tile.

    Rendered at 4x and downsampled so the profile keeps clean edges at 80 px.
    No lettering: at this size type only crowds the face out.
    """
    S = 4                      # supersampling factor
    px = size * S
    img = Image.new("RGB", (px, px), BG_JPEN)
    d   = ImageDraw.Draw(img)

    # Face fills 80% of the tile height and sits dead centre. The SVG's
    # drawing box is x 32..97, y 22..99, so (64, 60) is its middle.
    sn, sd = px * 80 // 100, 77
    cx = cy = px // 2
    ox, oy = cx - 64 * sn // sd, cy - 60 * sn // sd

    def fx(v):
        return v * sn // sd + ox

    def fy(v):
        return v * sn // sd + oy

    pts = [(fx(SVG_FX[i]), fy(SVG_FY[i])) for i in range(20)]

    # Slightly heavier than the watch draws it, but thin enough that the
    # pointed snout and the open mouth stay as separate shapes.
    sw = max(2, px // 40)
    for dx in range(-sw, sw + 1):
        for dy in range(-sw, sw + 1):
            d.polygon([(x + dx, y + dy) for x, y in pts], outline=YELLOW)

    er = max(2, 5 * sn // sd)
    ex, ey = fx(62), fy(48)
    d.ellipse((ex - er, ey - er, ex + er, ey + er), fill=YELLOW)

    mx, my = fx(42), fy(78)
    ro, ri = max(4, 9 * sn // sd), max(2, 4 * sn // sd)
    d.ellipse((mx - ro, my - ro, mx + ro, my + ro), fill=YELLOW)
    d.ellipse((mx - ri, my - ri, mx + ri, my + ri), fill=BAR_JPEN)

    img.resize((size, size), Image.LANCZOS).save(path)
    print(f"  Saved {path}")


if __name__ == "__main__":
    os.makedirs("assets", exist_ok=True)
    print("Generating assets…")
    make_menu_image("assets/menu_image.png")
    make_icon(48,  "assets/icon_48.png")
    make_icon(80,  "assets/icon_80.png")
    make_icon(144, "assets/icon_144.png")
    make_home(False, "assets/screenshot_jp_en.png")
    make_home(True,  "assets/screenshot_en_jp.png")
    make_listening("assets/screenshot_listening.png")
    make_result("assets/screenshot_result.png")
    make_log("assets/screenshot_log.png")
    print("Done.")
