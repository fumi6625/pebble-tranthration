#!/usr/bin/env python3
"""Generate Pebble app store / CloudPebble image assets."""

from PIL import Image, ImageDraw, ImageFont
import math, os

# ── SVG face data (from main.c) ───────────────────────────────────────────
SVG_FX = [70,81,88,94,96,97,96,92,87,80,70,54,43,40,47,32,47,49,53,61]
SVG_FY = [22,26,33,41,51,61,71,80,87,93,98,99,93,84,73,62,53,41,31,24]
# SVG face bounding box: x 32-97, y 22-99  centre ≈ (64, 60)

# ── Colour palette ────────────────────────────────────────────────────────
BLUE   = (85, 170, 255)
ORANGE = (255, 160,  50)
YELLOW = (255, 215,   0)
NAVY   = ( 0,  40,  90)
WHITE  = (255, 255, 255)
BLACK  = (  0,   0,   0)
RED    = (200,   0,   0)
DARK_RED=(120,   0,   0)
SKY_LIGHT=(130, 200, 255)

# ── Font paths ────────────────────────────────────────────────────────────
FONT_JP   = "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf"
FONT_SANS = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"

def load_font(path, size):
    try:
        return ImageFont.truetype(path, size)
    except Exception:
        return ImageFont.load_default()

# ── Face geometry helpers ─────────────────────────────────────────────────
def face_pts(W, H, face_pct=96, cy_pct=40, mirror=False):
    """Compute scaled face polygon, eye, mouth positions."""
    sn  = H * face_pct // 228   # match main.c formula  H*96/228
    sd  = 77
    cyx = H * cy_pct // 100
    cxx = W // 2
    ox  = cxx - 64 * sn // sd
    oy  = cyx - 60 * sn // sd

    pts = []
    bot = 0
    for i in range(20):
        px = SVG_FX[i] * sn // sd + ox
        py = SVG_FY[i] * sn // sd + oy
        if mirror:
            px = W - 1 - px
        pts.append((px, py))
        if py > bot:
            bot = py

    def fx(v):
        x = v * sn // sd + ox
        return (W - 1 - x) if mirror else x
    def fy(v):
        return v * sn // sd + oy

    eye    = (fx(62), fy(48), max(2, 4*sn//sd))
    mouth  = (fx(42), fy(78), max(4, 8*sn//sd), max(2, 4*sn//sd))
    return pts, eye, mouth, bot

def face_pts_icon(W, H, fill_pct=75, mirror=False):
    """Compute face geometry for square icon (custom scale)."""
    sn  = H * fill_pct // 100   # fill ~75% of height
    sd  = 77
    cyx = H * 47 // 100
    cxx = W // 2
    ox  = cxx - 64 * sn // sd
    oy  = cyx - 60 * sn // sd

    pts = []
    for i in range(20):
        px = SVG_FX[i] * sn // sd + ox
        py = SVG_FY[i] * sn // sd + oy
        if mirror:
            px = W - 1 - px
        pts.append((px, py))

    def fx(v):
        x = v * sn // sd + ox
        return (W - 1 - x) if mirror else x
    def fy(v):
        return v * sn // sd + oy

    eye   = (fx(62), fy(48), max(2, 4*sn//sd))
    mouth = (fx(42), fy(78), max(4, 8*sn//sd), max(2, 4*sn//sd))
    return pts, eye, mouth

# ── Face drawing ──────────────────────────────────────────────────────────
def draw_face(draw, pts, eye, mouth, stroke_col, stroke_w=2, mouth_fill=NAVY):
    """Draw the profile face: thick outline, eye dot, open-mouth ring."""
    # Outline (draw multiple offsets for thick stroke)
    for dx in range(-stroke_w+1, stroke_w+1):
        for dy in range(-stroke_w+1, stroke_w+1):
            shifted = [(x+dx, y+dy) for x,y in pts]
            draw.polygon(shifted, outline=stroke_col)

    ex, ey, er = eye
    draw.ellipse((ex-er, ey-er, ex+er, ey+er), fill=stroke_col)

    mx, my, mr_out, mr_in = mouth
    draw.ellipse((mx-mr_out, my-mr_out, mx+mr_out, my+mr_out), fill=stroke_col)
    draw.ellipse((mx-mr_in,  my-mr_in,  mx+mr_in,  my+mr_in),  fill=mouth_fill)

# ── 1. MENU IMAGE (25×25, black-on-white, high contrast) ─────────────────
def make_menu_image(path="assets/menu_image.png"):
    W, H = 25, 25
    img  = Image.new("RGB", (W, H), WHITE)
    draw = ImageDraw.Draw(img)

    # Small face — tight fill_pct to fit 25×25
    sn, sd = 18, 77
    cyx = 11
    cxx = 12
    ox  = cxx - 64 * sn // sd
    oy  = cyx - 60 * sn // sd

    pts = []
    for i in range(20):
        px = SVG_FX[i] * sn // sd + ox
        py = SVG_FY[i] * sn // sd + oy
        pts.append((px, py))

    # Filled solid black for maximum contrast on white
    draw.polygon(pts, fill=BLACK, outline=BLACK)

    # Eye (white dot punched in)
    ex = 62 * sn // sd + ox
    ey = 48 * sn // sd + oy
    er = max(1, 1)
    draw.ellipse((ex-er, ey-er, ex+er, ey+er), fill=WHITE)

    img.save(path)
    print(f"  Saved {path}")

# ── 2 & 3. Icon (80×80 and 144×144, colour) ──────────────────────────────
def make_icon(size, path):
    W = H = size
    img  = Image.new("RGB", (W, H), BLUE)
    draw = ImageDraw.Draw(img)

    # Rounded square blue background
    r = size // 8
    draw.rounded_rectangle((0, 0, W-1, H-1), radius=r, fill=BLUE)

    pts, eye, mouth = face_pts_icon(W, H, fill_pct=72)
    sw = max(1, size // 40)
    draw_face(draw, pts, eye, mouth,
              stroke_col=YELLOW, stroke_w=sw, mouth_fill=NAVY)

    # Small "J→E" label at bottom
    fsize = max(8, size // 12)
    font  = load_font(FONT_SANS, fsize)
    label = "J→E"
    try:
        bb = draw.textbbox((0,0), label, font=font)
        tw = bb[2]-bb[0]
    except Exception:
        tw = fsize * len(label) // 2
    draw.text(((W-tw)//2, H - fsize - size//14), label, font=font, fill=WHITE)

    img.save(path)
    print(f"  Saved {path}")

# ── 4 & 5. Home screen screenshots (200×228) ─────────────────────────────
def make_screenshot(mode_ejp, path):
    W, H = 200, 228
    bg   = ORANGE if mode_ejp else BLUE
    fg   = BLACK  if mode_ejp else WHITE

    img  = Image.new("RGB", (W, H), bg)
    draw = ImageDraw.Draw(img)

    # ── Title bar ──────────────────────────────────────────────────────
    title  = "Voice E to J" if mode_ejp else "Voice J to E"
    f_title = load_font(FONT_SANS, 18)
    try:
        bb = draw.textbbox((0,0), title, font=f_title)
        tw = bb[2]-bb[0]
    except Exception:
        tw = 18 * len(title) // 2
    draw.text(((W-tw)//2, 4), title, font=f_title, fill=fg)

    # ── Clock (top-right) ─────────────────────────────────────────────
    f_clock = load_font(FONT_SANS, 11)
    draw.text((W-46, 2), "12:00", font=f_clock, fill=fg)

    # ── Face ──────────────────────────────────────────────────────────
    pts, eye, mouth, bot_y = face_pts(W, H, face_pct=96, cy_pct=40, mirror=mode_ejp)
    sw = 3
    mouth_fill = (0, 85, 170) if not mode_ejp else (140, 70, 0)
    draw_face(draw, pts, eye, mouth,
              stroke_col=YELLOW, stroke_w=sw, mouth_fill=mouth_fill)

    # ── Right edge labels ─────────────────────────────────────────────
    f_lbl = load_font(FONT_SANS, 13)
    rec_col = DARK_RED if mode_ejp else RED
    draw.text((W-32, H//2-12), "REC", font=f_lbl, fill=rec_col)
    draw.text((W-32, int(H*0.80)), "LOG", font=f_lbl, fill=fg)

    # ── Rotation hint (arc + arrowhead near top-right) ───────────────
    cx, cy, r = W-18, H*11//100, 10
    draw.arc((cx-r, cy-r, cx+r, cy+r), start=30, end=330,
             fill=fg, width=2)
    # Arrowhead
    ax = cx + int(r * math.cos(math.radians(30)))
    ay = cy - int(r * math.sin(math.radians(30)))
    draw.polygon([(ax,ay),(ax-4,ay-5),(ax+3,ay-5)], fill=fg)

    # ── Prompt text ───────────────────────────────────────────────────
    prompt_y = bot_y + H // 28
    lm, pw   = 6, W - 38

    if mode_ejp:
        f_prompt = load_font(FONT_SANS, 20)
        lines = ["Please speak", "in English"]
        for i, line in enumerate(lines):
            try:
                bb = draw.textbbox((0,0), line, font=f_prompt)
                tw = bb[2]-bb[0]
            except Exception:
                tw = 20 * len(line) // 2
            draw.text((lm + (pw-tw)//2, prompt_y + i*26), line,
                      font=f_prompt, fill=fg)
    else:
        # Japanese prompt: "日本語で話して下さい"
        f_prompt = load_font(FONT_JP, 22)
        lines = ["日本語で",
                 "話して下さい"]
        for i, line in enumerate(lines):
            try:
                bb = draw.textbbox((0,0), line, font=f_prompt)
                tw = bb[2]-bb[0]
            except Exception:
                tw = 22 * len(line)
            draw.text((lm + (pw-tw)//2, prompt_y + i*28), line,
                      font=f_prompt, fill=WHITE)

    img.save(path)
    print(f"  Saved {path}")

# ── Main ──────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    os.makedirs("assets", exist_ok=True)
    print("Generating assets…")
    make_menu_image("assets/menu_image.png")
    make_icon(80,  "assets/icon_80.png")
    make_icon(144, "assets/icon_144.png")
    make_screenshot(False, "assets/screenshot_jp_en.png")
    make_screenshot(True,  "assets/screenshot_en_jp.png")
    print("Done.")
