#include <pebble.h>

// ── Platform screen sizes ─────────────────────────────────────────────────
// basalt  (Pebble Time/Steel):       144×168, color, rect
// chalk   (Pebble Time Round):       180×180, color, round
// diorite (Pebble 2 / 2 SE):         144×168, B&W,   rect
// emery   (Pebble Time 2):           200×228, color, rect

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define KEY_TEXT             0
#define KEY_LANG             1
#define KEY_RESULT           2
#define MSG_BUFFER_SIZE      512
#define TRANSLATE_TIMEOUT_MS 20000
#define WAVE_TIMER_MS        120
#define LOG_MAX              10

typedef enum { STATE_HOME, STATE_LISTENING, STATE_RESULT } AppState;

typedef struct {
  char original[150];
  char translated[201];
  char timestamp[8];
} LogEntry;

// ── App state ─────────────────────────────────────────────────────────────
static AppState          s_state    = STATE_HOME;
static char              s_dictated[512];
static char              s_translated[512];
static char              s_clock_buf[8];
static LogEntry          s_log[LOG_MAX];
static int               s_log_n    = 0;
static AppTimer         *s_watchdog  = NULL;
static AppTimer         *s_wave_timer = NULL;
static int               s_wave_phase = 0;
static bool              s_mode_ejp       = false; // false=JP→EN, true=EN→JP
static bool              s_result_flipped = false; // 180° flip to show partner
static int               s_flip_phase  = -1;       // -1=off; 0-7=mode-toggle squish anim
static AppTimer         *s_flip_timer  = NULL;
static int               s_card_phase  = -1;       // -1=off; 0-5=result-flip squish anim
static AppTimer         *s_card_timer  = NULL;
// Squish % per animation phase (100=full width, 0=collapsed)
static const uint8_t FLIP_SQ[8] = {80, 55, 30, 5, 5, 30, 55, 80};
static const uint8_t CARD_SQ[6] = {75, 40, 10, 10, 40, 75};

// ── Windows / layers ──────────────────────────────────────────────────────
static Window           *s_win      = NULL;
static Layer            *s_canvas   = NULL;
static TextLayer        *s_clock_tl = NULL;
static Window           *s_logwin   = NULL;
static Layer            *s_logcanv  = NULL;
static DictationSession *s_dictation = NULL;

// ── Face GPath ────────────────────────────────────────────────────────────
// SVG source path (viewBox 0 0 120 120, facing left, strokeWidth 7):
//   M70 22 C90 26 98 44 97 61 C96 78 88 92 70 98
//   C58 101 48 100 43 93 L40 84 L47 73 L32 62 L47 53
//   C49 45 47 37 53 31 C58 25 63 22 70 22 Z
// Bezier curves sampled at t=0, 0.2, 0.4, 0.6, 0.8, 1.0
// Face bounds: x [32..97] = 65 wide, y [22..99] = 77 tall
// SVG center: approximately (64, 60)
static const int16_t SVG_FX[20] = {
  70, 81, 88, 94, 96, 97, 96, 92, 87, 80,
  70, 54, 43, 40, 47, 32, 47, 49, 53, 61
};
static const int16_t SVG_FY[20] = {
  22, 26, 33, 41, 51, 61, 71, 80, 87, 93,
  98, 99, 93, 84, 73, 62, 53, 41, 31, 24
};

static GPoint s_face_pts[20];
static const GPathInfo s_face_info = { .num_points = 20, .points = s_face_pts };
static GPath *s_face_path = NULL;

// ── Computed layout (set once per window load) ────────────────────────────
static GPoint s_eye_pos, s_mouth_pos;
static int    s_eye_r, s_mouth_r_out, s_mouth_r_in;
static GPoint s_sw1a, s_sw1b;          // sw1: horizontal line at mouth
static GPoint s_sw2a, s_sw2b, s_sw2c; // sw2: arc curving down-left
static GPoint s_sw3a, s_sw3b, s_sw3c; // sw3: wider arc
static int    s_face_bot_y;            // y of face bottom (for prompt placement)

// ── Forward declarations ──────────────────────────────────────────────────
static void build_layout(GRect bounds);
static void start_dictation(void);
static void send_translation_request(void);
static void flip_tick(void *ctx);
static void card_flip_tick(void *ctx);

// ── Dictation count (persistent, resets each calendar month) ─────────────
// ── Helpers ───────────────────────────────────────────────────────────────
static void cancel_watchdog(void) {
  if (s_watchdog) { app_timer_cancel(s_watchdog); s_watchdog = NULL; }
}
static void stop_wave(void) {
  if (s_wave_timer) { app_timer_cancel(s_wave_timer); s_wave_timer = NULL; }
}
static void canvas_dirty(void) { if (s_canvas) layer_mark_dirty(s_canvas); }

// ── Log management ────────────────────────────────────────────────────────
static void add_to_log(const char *orig, const char *trans) {
  int n = (s_log_n < LOG_MAX) ? s_log_n : LOG_MAX - 1;
  memmove(&s_log[1], &s_log[0], n * sizeof(LogEntry));
  if (s_log_n < LOG_MAX) s_log_n++;
  snprintf(s_log[0].original,   sizeof(s_log[0].original),   "%.149s", orig);
  snprintf(s_log[0].translated, sizeof(s_log[0].translated), "%.200s", trans);
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  snprintf(s_log[0].timestamp, sizeof(s_log[0].timestamp),
           "%02d:%02d", t->tm_hour, t->tm_min);
}

// ── Tick handler ──────────────────────────────────────────────────────────
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  snprintf(s_clock_buf, sizeof(s_clock_buf),
           "%02d:%02d", tick_time->tm_hour, tick_time->tm_min);
  if (s_clock_tl) text_layer_set_text(s_clock_tl, s_clock_buf);
}

// ── Wave animation ────────────────────────────────────────────────────────
static void wave_timer_cb(void *ctx) {
  s_wave_phase = (s_wave_phase + 1) % 4;
  canvas_dirty();
  s_wave_timer = (s_state == STATE_LISTENING)
    ? app_timer_register(WAVE_TIMER_MS, wave_timer_cb, NULL) : NULL;
}
static void start_wave(void) {
  stop_wave();
  s_wave_phase = 0;
  s_wave_timer = app_timer_register(WAVE_TIMER_MS, wave_timer_cb, NULL);
}

// ── Mode-toggle squish animation (8 frames, 80ms each) ───────────────────
// Phases 0-3: screen squishes to zero (card disappears into vertical axis)
// At phase 4: the mode actually toggles (face flips, colours swap)
// Phases 4-7: screen unsquishes back to full width
static void flip_tick(void *context) {
  s_flip_phase++;
  if (s_flip_phase == 4) {
    // Mid-point: commit the mode toggle
    s_mode_ejp       = !s_mode_ejp;
    s_dictated[0]    = '\0';
    s_translated[0]  = '\0';
    s_result_flipped = false;
    if (s_face_path) { gpath_destroy(s_face_path); s_face_path = NULL; }
    build_layout(layer_get_bounds(window_get_root_layer(s_win)));
    s_face_path = gpath_create(&s_face_info);
    if (s_clock_tl) {
#ifdef PBL_COLOR
      text_layer_set_text_color(s_clock_tl,
        s_mode_ejp ? GColorBlack : GColorWhite);
#endif
    }
  }
  canvas_dirty();
  if (s_flip_phase < 7) {
    s_flip_timer = app_timer_register(80, flip_tick, NULL);
  } else {
    s_flip_phase = -1;
    s_flip_timer = NULL;
    canvas_dirty();
  }
}

// ── Result card squish animation (6 frames, 80ms each) ───────────────────
// Phases 0-2: card squishes to zero
// At phase 3: s_result_flipped toggles
// Phases 3-5: card unsquishes
static void card_flip_tick(void *context) {
  s_card_phase++;
  if (s_card_phase == 3) {
    s_result_flipped = !s_result_flipped;
  }
  canvas_dirty();
  if (s_card_phase < 5) {
    s_card_timer = app_timer_register(80, card_flip_tick, NULL);
  } else {
    s_card_phase = -1;
    s_card_timer = NULL;
    canvas_dirty();
  }
}

// ── Layout builder ────────────────────────────────────────────────────────
// Called once in main_load to compute all positions from actual screen bounds.
// Scales the SVG face to 37% of screen height, centered on screen.
static void build_layout(GRect bounds) {
  int W = bounds.size.w;
  int H = bounds.size.h;

  // Face scale: 1.5× previous size — large and prominent
  int face_h = H * 96 / 228;  // ~42% of H
  int sn = face_h;             // scale numerator
  int sd = 77;                 // scale denominator (SVG face height 22..99 = 77)

  // Face center: balanced to leave room for title above and prompt below
#ifdef PBL_ROUND
  int face_cy = H * 46 / 100;
#else
  int face_cy = H * 40 / 100;
#endif
  int face_cx = W / 2;

  // Offset so SVG center (64, 60) maps to (face_cx, face_cy)
  int ox = face_cx - 64 * sn / sd;
  int oy = face_cy - 60 * sn / sd;

  // Build 20-point face outline; flip x around screen centre in EN→JP mode
  s_face_bot_y = 0;
  for (int i = 0; i < 20; i++) {
    int px = SVG_FX[i] * sn / sd + ox;
    s_face_pts[i].x = (int16_t)(s_mode_ejp ? W - 1 - px : px);
    s_face_pts[i].y = (int16_t)(SVG_FY[i] * sn / sd + oy);
    if (s_face_pts[i].y > s_face_bot_y) s_face_bot_y = s_face_pts[i].y;
  }

// Flip x when in EN→JP mode so every feature mirrors the face
#define FPX(v) (s_mode_ejp ? (W - 1 - (v)) : (v))

  s_eye_pos = GPoint(FPX(62 * sn / sd + ox), 48 * sn / sd + oy);
  s_eye_r   = MAX(2, 4 * sn / sd);

  s_mouth_pos   = GPoint(FPX(42 * sn / sd + ox), 78 * sn / sd + oy);
  s_mouth_r_out = MAX(4, 8 * sn / sd);
  s_mouth_r_in  = MAX(2, 4 * sn / sd);

  s_sw1a = GPoint(FPX(22 * sn / sd + ox), 78 * sn / sd + oy);
  s_sw1b = GPoint(FPX(15 * sn / sd + ox), 78 * sn / sd + oy);
  s_sw2a = GPoint(FPX(18 * sn / sd + ox), 70 * sn / sd + oy);
  s_sw2b = GPoint(FPX(11 * sn / sd + ox), 76 * sn / sd + oy);
  s_sw2c = GPoint(FPX( 9 * sn / sd + ox), 86 * sn / sd + oy);
  s_sw3a = GPoint(FPX(15 * sn / sd + ox), 62 * sn / sd + oy);
  s_sw3b = GPoint(FPX( 7 * sn / sd + ox), 74 * sn / sd + oy);
  s_sw3c = GPoint(FPX( 5 * sn / sd + ox), 93 * sn / sd + oy);
#undef FPX
}

// ── 180° rotation of a rectangular region only ────────────────────────────
// Rotates pixels within `region` only — buttons/clock outside are untouched.
// Color platforms only (B&W bit-packed framebuffer not supported).
#ifdef PBL_COLOR
static void rotate_region_180(GContext *ctx, GRect region) {
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  GRect fb_b  = gbitmap_get_bounds(fb);
  int FW      = fb_b.size.w;
  int FH      = fb_b.size.h;
  int stride  = gbitmap_get_bytes_per_row(fb);
  uint8_t *data = (uint8_t *)gbitmap_get_data(fb);

  int rx = region.origin.x;
  int ry = region.origin.y;
  int rw = region.size.w;
  int rh = region.size.h;
  // Clamp to framebuffer bounds
  if (rx < 0) { rw += rx; rx = 0; }
  if (ry < 0) { rh += ry; ry = 0; }
  if (rx + rw > FW) rw = FW - rx;
  if (ry + rh > FH) rh = FH - ry;
  if (rw <= 0 || rh <= 0) { graphics_release_frame_buffer(ctx, fb); return; }

  for (int y = 0; y < rh / 2; y++) {
    uint8_t *top = data + (ry + y)          * stride + rx;
    uint8_t *bot = data + (ry + rh - 1 - y) * stride + rx;
    for (int x = 0; x < rw; x++) {
      uint8_t tmp      = top[x];
      top[x]           = bot[rw - 1 - x];
      bot[rw - 1 - x]  = tmp;
    }
  }
  if (rh & 1) {
    uint8_t *mid = data + (ry + rh / 2) * stride + rx;
    for (int x = 0; x < rw / 2; x++) {
      uint8_t tmp = mid[x]; mid[x] = mid[rw-1-x]; mid[rw-1-x] = tmp;
    }
  }
  graphics_release_frame_buffer(ctx, fb);
}
#endif

// ── 360° rotation-arrow hint near the UP button ───────────────────────────
// Draws a circular arrow (arc + arrowhead) on the right edge near the top,
// indicating that the UP button cycles translation mode.
static void draw_rotate_hint(GContext *ctx, int W, int H) {
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_fill_color(ctx, GColorWhite);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif

#ifdef PBL_ROUND
  int cx = W - W * 18 / 100;
  int cy = H * 14 / 100 + 10;
  int r  = H * 7 / 100;
#else
  int cx = W - 18;
  int cy = H * 11 / 100 + 10;
  int r  = 10;
#endif

  // Draw arc ~300° (leaving a gap at the top-right where the arrowhead goes)
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_arc(ctx, GRect(cx - r, cy - r, r * 2, r * 2),
                    GOvalScaleModeFitCircle,
                    DEG_TO_TRIGANGLE(30), DEG_TO_TRIGANGLE(330));

  // Arrowhead at the end of the arc (near 330°) pointing clockwise
  // 330° on unit circle: cos=-0.866, sin=-0.5
  int ax = cx + r * 87 / 100;   // cos(30°)≈0.866
  int ay = cy - r * 50 / 100;   // sin(30°)≈0.5 (above center)
  GPoint tip = GPoint(ax, ay);
  GPoint p1  = GPoint(ax - 4, ay - 5);
  GPoint p2  = GPoint(ax + 3, ay - 5);
  GPoint pts[3] = { tip, p1, p2 };
  const GPathInfo arrow_info = { .num_points = 3, .points = pts };
  GPath *arrow = gpath_create(&arrow_info);
  if (arrow) {
    gpath_draw_filled(ctx, arrow);
    gpath_destroy(arrow);
  }
  graphics_context_set_stroke_width(ctx, 1);
}

// ── Draw face ─────────────────────────────────────────────────────────────
static void draw_face(GContext *ctx) {
  if (!s_face_path) return;

#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorYellow);
  graphics_context_set_fill_color(ctx,   GColorYellow);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx,   GColorBlack);
#endif

  // Outline drawn 16× with (-1..2)×(-1..2) offsets for ~4px stroke (2× original)
  int dx, dy;
  for (dx = -1; dx <= 2; dx++) {
    for (dy = -1; dy <= 2; dy++) {
      gpath_move_to(s_face_path, GPoint(dx, dy));
      gpath_draw_outline(ctx, s_face_path);
    }
  }
  gpath_move_to(s_face_path, GPoint(0, 0));

  // Eye
  graphics_fill_circle(ctx, s_eye_pos, s_eye_r);

  // Mouth: yellow outer ring, blue inner fill (open-mouth effect)
  graphics_fill_circle(ctx, s_mouth_pos, s_mouth_r_out);
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(0, 85, 170));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_circle(ctx, s_mouth_pos, s_mouth_r_in);

  // Sound waves visible only when listening
  if (s_state != STATE_LISTENING) return;
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorYellow);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  // sw1: horizontal line at mouth height
  graphics_draw_line(ctx, s_sw1a, s_sw1b);
  // sw2: arc downward
  graphics_draw_line(ctx, s_sw2a, s_sw2b);
  graphics_draw_line(ctx, s_sw2b, s_sw2c);
  // sw3: wider arc
  graphics_draw_line(ctx, s_sw3a, s_sw3b);
  graphics_draw_line(ctx, s_sw3b, s_sw3c);
}

// ── Right-edge action labels (REC / LOG) ──────────────────────────────────
// SELECT = REC/STP (vertical center), DOWN = LOG (lower). Drawn in all states.
static void draw_right_labels(GContext *ctx, int W, int H) {
#ifdef PBL_ROUND
  int lx = W - W * 22 / 100;   // inset from the curved right edge
  int lw = W * 20 / 100;
  int rec_y = H / 2 - 24;
  int log_y = H / 2 + 4;
#else
  int lx = W - 32;
  int lw = 30;
  int rec_y = H / 2 - 10;
  int log_y = H * 80 / 100;
#endif

  // REC (or STP while listening)
#ifdef PBL_COLOR
  if (s_state == STATE_LISTENING) {
    graphics_context_set_text_color(ctx, GColorWhite);
  } else {
    graphics_context_set_text_color(ctx,
      s_mode_ejp ? GColorFromRGB(150, 0, 0) : GColorRed);
  }
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx,
    (s_state == STATE_LISTENING) ? "STP" : "REC",
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(lx, rec_y, lw, 22),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);

  // LOG
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, s_mode_ejp ? GColorBlack : GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx, "LOG",
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
    GRect(lx, log_y, lw, 20),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

// ── Pick the largest readable font that suits the text length ──────────────
static const char *font_for_len(int len) {
  if (len <= 18)  return FONT_KEY_GOTHIC_28_BOLD;
  if (len <= 40)  return FONT_KEY_GOTHIC_24_BOLD;
  if (len <= 90)  return FONT_KEY_GOTHIC_18_BOLD;
  return FONT_KEY_GOTHIC_14_BOLD;
}

// ── Result screen: full-area, large, high-contrast translation ─────────────
static void draw_result(GContext *ctx, int W, int H) {
  // Direction header — adapts to current mode
  const char *hdr = s_mode_ejp ? "EN \xe2\x86\x92 JP" : "JP \xe2\x86\x92 EN";
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, s_mode_ejp ? GColorBlack : GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
#ifdef PBL_ROUND
  graphics_draw_text(ctx, hdr,
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(0, H / 20, W, 22),
    GTextOverflowModeFill, GTextAlignmentCenter, NULL);
#else
  graphics_draw_text(ctx, hdr,
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(8, 2, W - 60, 22),
    GTextOverflowModeFill, GTextAlignmentLeft, NULL);
#endif

  // Card region (leaves room for the right-edge REC/LOG labels)
#ifdef PBL_ROUND
  int top = H / 5;
  int lm  = W / 7;
  int rm  = W / 7;
#else
  int top = H / 7;
  int lm  = 6;
  int rm  = 36;
#endif
  int card_x = lm;
  int card_y = top;
  int card_w = W - lm - rm;
  int card_h = H - top - H / 16;

  // Apply card squish during animation: scale x around card centre
  int sq_pct = (s_card_phase >= 0 && s_card_phase < 6)
               ? CARD_SQ[s_card_phase] : 100;
  if (sq_pct < 100) {
    int mid   = card_x + card_w / 2;
    int hw    = card_w / 2 * sq_pct / 100;
    card_x    = mid - hw;
    card_w    = hw * 2;
    if (card_w < 2) card_w = 2;
  }

  GFont font = fonts_get_system_font(font_for_len((int)strlen(s_translated)));
  GRect card_rect = GRect(card_x, card_y, card_w, card_h);
  GRect pad  = GRect(card_x + 5, card_y + 4, card_w - 10, card_h - 8);

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, card_rect, 6, GCornersAll);
  graphics_context_set_text_color(ctx, GColorFromRGB(0, 40, 90));
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif

  if (sq_pct >= 20 && pad.size.w > 10) {
    GSize ts = graphics_text_layout_get_content_size(s_translated, font,
      pad, GTextOverflowModeWordWrap, GTextAlignmentCenter);
    int ty = pad.origin.y;
    if (ts.h < pad.size.h) ty += (pad.size.h - ts.h) / 2;
    graphics_draw_text(ctx, s_translated, font,
      GRect(pad.origin.x, ty, pad.size.w, pad.size.h),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }

  // After drawing the card, rotate only the card region if flipped
#ifdef PBL_COLOR
  if (s_result_flipped && sq_pct >= 95) {
    rotate_region_180(ctx, card_rect);
  }
#endif
}

// ── Main canvas ───────────────────────────────────────────────────────────
static void canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int W = b.size.w;
  int H = b.size.h;

  // Squish factor for mode-toggle animation (100=full, 0=collapsed)
  int sq_pct = (s_flip_phase >= 0 && s_flip_phase < 8)
               ? FLIP_SQ[s_flip_phase] : 100;

  // Background: sky-blue (JP→EN) / warm-orange (EN→JP)
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx,
    s_mode_ejp ? GColorFromRGB(255, 160, 50) : GColorFromRGB(85, 170, 255));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // During squish: draw a vertical stripe narrower than full width
  if (sq_pct < 100) {
    int mid = W / 2;
    int hw  = W / 2 * sq_pct / 100;
    // Reveal background colour outside the stripe (already filled above)
    // Draw the content stripe clipped — we use a narrowed version of all rects
    // by offsetting x toward centre and reducing widths by (1 - sq_pct/100).
    // Implemented below by adjusting all draw positions proportionally.
    (void)mid; (void)hw; // positions computed per-element below
  }

  // Helper macro: squish an x-coordinate toward screen centre
  // sq_pct=100 → identity; sq_pct=0 → all x collapse to W/2
  #define SQX(x) (W/2 + ((x) - W/2) * sq_pct / 100)
  #define SQW(w) ((w) * sq_pct / 100)

  // ── RESULT: dedicate the whole screen to the readable translation ───────
  if (s_state == STATE_RESULT && s_translated[0]) {
    draw_result(ctx, W, H);
    draw_right_labels(ctx, W, H);
    return;
  }

  // ── HOME / LISTENING: title + face + prompt ─────────────────────────────

  // Draw face (uses precomputed s_face_pts which are already squished via build_layout;
  // during flip animation we manually squish the gpath points inline)
  if (sq_pct < 100 && s_face_path) {
    // Temporarily squish face points
    GPoint tmp_pts[20];
    for (int i = 0; i < 20; i++) {
      tmp_pts[i].x = (int16_t)SQX(s_face_pts[i].x);
      tmp_pts[i].y = s_face_pts[i].y;
    }
    GPathInfo tmp_info = { .num_points = 20, .points = tmp_pts };
    GPath *tmp_path = gpath_create(&tmp_info);
    if (tmp_path) {
      GPath *saved = s_face_path;
      s_face_path = tmp_path;
      draw_face(ctx);
      s_face_path = saved;
      gpath_destroy(tmp_path);
    }
  } else {
    draw_face(ctx);
  }

  // App title
  const char *title_text = s_mode_ejp ? "Voice E to J" : "Voice J to E";
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, s_mode_ejp ? GColorBlack : GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  {
#ifdef PBL_ROUND
    int tx = SQX(W / 8), tw = SQW(W * 6 / 8);
    if (tw < 4) tw = 4;
    graphics_draw_text(ctx, title_text,
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(tx, 4, tw, 30),
      GTextOverflowModeFill, GTextAlignmentCenter, NULL);
#else
    int tx = SQX(0), tw = SQW(W);
    if (tw < 4) tw = 4;
    graphics_draw_text(ctx, title_text,
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(tx, 2, tw, 30),
      GTextOverflowModeFill, GTextAlignmentCenter, NULL);
#endif
  }

  draw_right_labels(ctx, W, H);

  // Rotation hint (↻) near UP button — only on HOME, not animating
  if (s_state == STATE_HOME && sq_pct == 100) {
    draw_rotate_hint(ctx, W, H);
  }

  // ── Prompt / listening text ────────────────────────────────────────────
  int prompt_y  = s_face_bot_y + H / 28;
  // Larger font for the prompt (~2× GOTHIC_18)
  const char *prompt_font = (H > 200)
    ? FONT_KEY_GOTHIC_28_BOLD : FONT_KEY_GOTHIC_24_BOLD;
  int prompt_h = (H > 200) ? 70 : 60;

#ifdef PBL_ROUND
  int prompt_lm = SQX(W / 8);
  int prompt_w  = SQW(W * 6 / 8);
#else
  int prompt_lm = SQX(6);
  int prompt_w  = SQW(W - 38);
#endif
  if (prompt_w < 4) prompt_w = 4;

#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif

  if (s_state == STATE_LISTENING) {
    // "聞き取り中…"
    graphics_draw_text(ctx,
      "\xe8\x81\x9e\xe3\x81\x8d\xe5\x8f\x96\xe3\x82\x8a"
      "\xe4\xb8\xad\xe2\x80\xa6",
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(prompt_lm, prompt_y, prompt_w, H / 8),
      GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    // Animated wave bars
    static const int8_t wh[4][13] = {
      { 5, 8,12,16,20,22,20,16,12, 8, 5, 4, 3},
      { 3, 5, 8,14,20,22,20,14, 8, 5, 3, 5, 8},
      { 5, 9,14,18,20,18,14, 9, 5, 4, 6,10,14},
      { 4, 7,12,18,22,20,16,12, 7, 5, 8,12,16},
    };
    int bar_total = 13 * 7 - 3;
    int x0 = (W - bar_total) / 2;
    int bar_base = H - H / 14;
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorWhite);
#else
    graphics_context_set_fill_color(ctx, GColorBlack);
#endif
    for (int i = 0; i < 13; i++) {
      int h = wh[s_wave_phase][i];
      graphics_fill_rect(ctx, GRect(x0 + i * 7, bar_base - h, 4, h),
                         1, GCornersAll);
    }
  } else {
    // HOME prompt
    if (s_mode_ejp) {
      graphics_draw_text(ctx, "Please speak\nin English",
        fonts_get_system_font(prompt_font),
        GRect(prompt_lm, prompt_y, prompt_w, prompt_h),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    } else {
      graphics_draw_text(ctx,
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xa7\n"
        "\xe8\xa9\xb1\xe3\x81\x97\xe3\x81\xa6\xe4\xb8\x8b"
        "\xe3\x81\x95\xe3\x81\x84",
        fonts_get_system_font(prompt_font),
        GRect(prompt_lm, prompt_y, prompt_w, prompt_h),
        GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }
  }
  #undef SQX
  #undef SQW
}

// ── Log canvas ────────────────────────────────────────────────────────────
static void log_canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int W = b.size.w;
  int H = b.size.h;

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(85, 170, 255));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Header bar
  int hdr_h = H / 8;
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(0, 85, 170));
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx, GRect(0, 0, W, hdr_h), 0, GCornerNone);

  int hdr_font_y = hdr_h / 2 - 9;
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorBlack);
#else
  graphics_context_set_text_color(ctx, GColorWhite);
#endif
  graphics_draw_text(ctx, "LOG",
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(8, hdr_font_y, 50, 22),
    GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  // "直近10件" — \x91 followed by "10" needs string split
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorFromRGB(170, 220, 255));
#else
  graphics_context_set_text_color(ctx, GColorWhite);
#endif
  graphics_draw_text(ctx,
    "\xe7\x9b\xb4\xe8\xbf\x91" "10\xe4\xbb\xb6",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(W - 62, hdr_font_y + 3, 58, 16),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);

  if (s_log_n == 0) {
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorWhite);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, "No history yet",
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(8, hdr_h + 8, W - 16, 20),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    return;
  }

  // Each card shows a bold timestamp + the English translation, large & legible.
  // (The recognized Japanese is mostly kanji, which the system font can't
  //  render, so the card focuses on the readable English result.)
  int card_h   = H * 56 / 168;
  int card_gap = H * 6 / 168;
  int lm = W > 160 ? 10 : 6;
  int y  = hdr_h + 4;

  for (int i = 0; i < s_log_n && y + card_h <= H; i++) {
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorWhite);
#else
    graphics_context_set_fill_color(ctx, GColorWhite);
#endif
    graphics_fill_rect(ctx, GRect(lm, y, W - lm*2, card_h), 5, GCornersAll);

    // Timestamp (top-left, accent color)
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorFromRGB(0, 110, 200));
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, s_log[i].timestamp,
      fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(lm+6, y+3, 48, 16),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // English translation — the main content, large and high-contrast
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorFromRGB(0, 40, 90));
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, s_log[i].translated,
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(lm+6, y + 20, W - lm*2 - 12, card_h - 24),
      GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    y += card_h + card_gap;
  }
}

// ── Watchdog ──────────────────────────────────────────────────────────────
static void watchdog_cb(void *ctx) {
  s_watchdog = NULL;
  stop_wave();
  s_state = STATE_HOME;
  canvas_dirty();
}

// ── Dictation ─────────────────────────────────────────────────────────────
static void dictation_cb(DictationSession *session,
                         DictationSessionStatus status,
                         char *transcription, void *context) {
  stop_wave();
  if (status == DictationSessionStatusSuccess) {
    snprintf(s_dictated, sizeof(s_dictated), "%s", transcription);
    s_state = STATE_HOME;
    canvas_dirty();
    send_translation_request();
  } else {
    s_state = STATE_HOME;
    canvas_dirty();
  }
}

// ── AppMessage ────────────────────────────────────────────────────────────
static void send_translation_request(void) {
  cancel_watchdog();
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_cstring(iter, KEY_TEXT, s_dictated);
  dict_write_cstring(iter, KEY_LANG, s_mode_ejp ? "en|ja" : "ja|en");
  if (app_message_outbox_send() != APP_MSG_OK) return;
  s_watchdog = app_timer_register(TRANSLATE_TIMEOUT_MS, watchdog_cb, NULL);
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  cancel_watchdog();
  Tuple *t = dict_find(iter, KEY_RESULT);
  if (!t) return;
  const char *r = t->value->cstring;
  if (strncmp(r, "JS OK", 5) == 0) return;
  snprintf(s_translated, sizeof(s_translated), "%s", r);
  add_to_log(s_dictated, s_translated);
  s_state = STATE_RESULT;
  canvas_dirty();
}
static void inbox_dropped(AppMessageResult reason, void *ctx) { cancel_watchdog(); }
static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) { cancel_watchdog(); }
static void outbox_sent(DictionaryIterator *iter, void *ctx) {}

// ── Buttons ───────────────────────────────────────────────────────────────
static void btn_up(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_RESULT) {
    // Start card flip animation (if not already animating)
    if (s_card_phase < 0) {
      s_card_phase = 0;
      if (s_card_timer) { app_timer_cancel(s_card_timer); s_card_timer = NULL; }
      s_card_timer = app_timer_register(80, card_flip_tick, NULL);
      canvas_dirty();
    }
  } else if (s_state != STATE_LISTENING) {
    // Start mode-toggle squish animation (if not already animating)
    if (s_flip_phase < 0) {
      s_flip_phase = 0;
      if (s_flip_timer) { app_timer_cancel(s_flip_timer); s_flip_timer = NULL; }
      s_flip_timer = app_timer_register(80, flip_tick, NULL);
      canvas_dirty();
    }
  }
}
static void btn_select(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_LISTENING) {
    dictation_session_stop(s_dictation);
    stop_wave();
    s_state = STATE_HOME;
    canvas_dirty();
  } else {
    s_result_flipped = false;  // reset flip when starting fresh recording
    start_dictation();
  }
}
static void btn_down(ClickRecognizerRef r, void *ctx) {
  if (s_logwin) window_stack_push(s_logwin, true);
}
static void click_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP,     btn_up);
  window_single_click_subscribe(BUTTON_ID_SELECT, btn_select);
  window_single_click_subscribe(BUTTON_ID_DOWN,   btn_down);
}

static void start_dictation(void) {
  if (!s_dictation) return;
  s_state = STATE_LISTENING;
  canvas_dirty();
  start_wave();
  dictation_session_start(s_dictation);
}

// ── Log window ────────────────────────────────────────────────────────────
static void log_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_logcanv = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_logcanv, log_canvas_draw);
  layer_add_child(root, s_logcanv);
}
static void log_appear(Window *w) { if (s_logcanv) layer_mark_dirty(s_logcanv); }
static void log_unload(Window *w) {
  if (s_logcanv) { layer_destroy(s_logcanv); s_logcanv = NULL; }
}
static void log_back(ClickRecognizerRef r, void *ctx) { window_stack_pop(true); }
static void log_clicks(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, log_back);
  window_single_click_subscribe(BUTTON_ID_BACK,   log_back);
}

// ── Main window ───────────────────────────────────────────────────────────
static void main_load(Window *w) {
  Layer *root   = window_get_root_layer(w);
  GRect bounds  = layer_get_bounds(root);
  int   W       = bounds.size.w;

  // Build face path and all element positions for this screen size
  build_layout(bounds);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_draw);
  layer_add_child(root, s_canvas);

  // Clock TextLayer — top-right on rect, top-center on round
#ifdef PBL_ROUND
  int H = bounds.size.h;
  GRect clock_frame = GRect(W/2 - 24, H/20, 48, 14);
#else
  GRect clock_frame = GRect(W - 50, 1, 48, 14);
#endif

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  snprintf(s_clock_buf, sizeof(s_clock_buf), "%02d:%02d", t->tm_hour, t->tm_min);

  s_clock_tl = text_layer_create(clock_frame);
  text_layer_set_background_color(s_clock_tl, GColorClear);
#ifdef PBL_COLOR
  text_layer_set_text_color(s_clock_tl, GColorWhite);
#else
  text_layer_set_text_color(s_clock_tl, GColorBlack);
#endif
  text_layer_set_font(s_clock_tl, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
#ifdef PBL_ROUND
  text_layer_set_text_alignment(s_clock_tl, GTextAlignmentCenter);
#else
  text_layer_set_text_alignment(s_clock_tl, GTextAlignmentRight);
#endif
  text_layer_set_text(s_clock_tl, s_clock_buf);
  layer_add_child(root, text_layer_get_layer(s_clock_tl));

  s_face_path = gpath_create(&s_face_info);
  s_dictation = dictation_session_create(sizeof(s_dictated), dictation_cb, NULL);
}

static void main_unload(Window *w) {
  cancel_watchdog();
  stop_wave();
  if (s_flip_timer)  { app_timer_cancel(s_flip_timer);          s_flip_timer = NULL; }
  if (s_card_timer)  { app_timer_cancel(s_card_timer);          s_card_timer = NULL; }
  s_flip_phase = -1;
  s_card_phase = -1;
  if (s_face_path)  { gpath_destroy(s_face_path);              s_face_path  = NULL; }
  if (s_dictation)  { dictation_session_destroy(s_dictation);  s_dictation  = NULL; }
  if (s_clock_tl)   { text_layer_destroy(s_clock_tl);          s_clock_tl   = NULL; }
  if (s_canvas)     { layer_destroy(s_canvas);                  s_canvas     = NULL; }
}

// ── App lifecycle ─────────────────────────────────────────────────────────
static void init(void) {
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_register_outbox_sent(outbox_sent);
  app_message_open(MSG_BUFFER_SIZE, MSG_BUFFER_SIZE);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  s_logwin = window_create();
  window_set_window_handlers(s_logwin, (WindowHandlers){
    .load = log_load, .appear = log_appear, .unload = log_unload });
  window_set_click_config_provider(s_logwin, log_clicks);

  s_win = window_create();
  window_set_window_handlers(s_win, (WindowHandlers){
    .load = main_load, .unload = main_unload });
  window_set_click_config_provider(s_win, click_provider);
  window_stack_push(s_win, true);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  if (s_logwin) { window_destroy(s_logwin); s_logwin = NULL; }
  if (s_win)    { window_destroy(s_win);    s_win    = NULL; }
}

int main(void) { init(); app_event_loop(); deinit(); return 0; }
