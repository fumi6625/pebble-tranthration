#include <pebble.h>

// ── Target platform ───────────────────────────────────────────────────────
// emery (Pebble Time 2): 200×228, colour, rect. Sole supported platform.
// Screen is ~200 ppi (0.126 mm/px), so text sizing follows:
//   body text >= 16 px (2 mm); absolute floor 14 px (~1.8 mm).
//   GOTHIC_14 is therefore the smallest font used anywhere.

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

// ── Layout constants (emery 200×228) ──────────────────────────────────────
#define STATUS_H   24    // top status bar height
#define RAIL_W     34    // right-edge button rail width
#define CHIP       26    // rail chip (icon button) size

// ── Colour system ─────────────────────────────────────────────────────────
#define COL_BG_JPEN   GColorFromRGB( 85, 170, 255)  // sky blue   (JP→EN)
#define COL_BG_ENJP   GColorFromRGB(255, 160,  50)  // warm orange(EN→JP)
#define COL_BAR_JPEN  GColorFromRGB(  0,  85, 170)  // status bar / chips
#define COL_BAR_ENJP  GColorFromRGB(180,  95,   0)
#define COL_INK       GColorFromRGB(  0,  40,  90)  // text on white cards
#define COL_ACCENT    GColorFromRGB(  0, 110, 200)  // timestamps, hint pill

// Current-mode colour helpers
#define BG_COL()   (s_mode_ejp ? COL_BG_ENJP  : COL_BG_JPEN)
#define BAR_COL()  (s_mode_ejp ? COL_BAR_ENJP : COL_BAR_JPEN)

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
// The clock lives in the status bar, so a new minute just redraws the canvas.
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  snprintf(s_clock_buf, sizeof(s_clock_buf),
           "%02d:%02d", tick_time->tm_hour, tick_time->tm_min);
  canvas_dirty();
  if (s_logcanv) layer_mark_dirty(s_logcanv);
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
// Called from main_load (and on every mode toggle) to compute face geometry.
// The face is centred in the *content* area — i.e. the screen minus the right
// button rail — and sits between the status bar and the prompt text.
static void build_layout(GRect bounds) {
  int W = bounds.size.w;
  int H = bounds.size.h;

  int face_h = H * 88 / 228;  // 88 px on emery
  int sn = face_h;             // scale numerator
  int sd = 77;                 // scale denominator (SVG face height 22..99 = 77)

  // Vertically: 36% down the area below the status bar.
  // Horizontally: centre of the content area (screen minus the rail).
  int face_cy = STATUS_H + (H - STATUS_H) * 36 / 100;
  int face_cx = (W - RAIL_W) / 2;

  // Offset so SVG center (64, 60) maps to (face_cx, face_cy)
  int ox = face_cx - 64 * sn / sd;
  int oy = face_cy - 60 * sn / sd;

  // Build 20-point face outline; mirror about the content centre in EN→JP mode
  s_face_bot_y = 0;
  for (int i = 0; i < 20; i++) {
    int px = SVG_FX[i] * sn / sd + ox;
    s_face_pts[i].x = (int16_t)(s_mode_ejp ? 2 * face_cx - px : px);
    s_face_pts[i].y = (int16_t)(SVG_FY[i] * sn / sd + oy);
    if (s_face_pts[i].y > s_face_bot_y) s_face_bot_y = s_face_pts[i].y;
  }

// Mirror x about the content centre so every feature follows the face
#define FPX(v) (s_mode_ejp ? (2 * face_cx - (v)) : (v))

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
// Rotates pixels within `region` only — status bar and rail stay upright.
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

// ── Button icons ──────────────────────────────────────────────────────────
// Drawn with primitives rather than glyphs: the system font has no reliable
// coverage for ↻ ⇅ ☰, and vector icons stay crisp at any chip size.
typedef enum { ICON_ROTATE, ICON_REC, ICON_STOP, ICON_LIST, ICON_FLIP } IconKind;

static void draw_icon(GContext *ctx, IconKind kind, int cx, int cy, GColor c) {
  graphics_context_set_stroke_color(ctx, c);
  graphics_context_set_fill_color(ctx, c);

  switch (kind) {
    case ICON_ROTATE: {          // circular arrow — switch direction
      int r = 7;
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_arc(ctx, GRect(cx - r, cy - r, r * 2, r * 2),
                        GOvalScaleModeFitCircle,
                        DEG_TO_TRIGANGLE(40), DEG_TO_TRIGANGLE(330));
      graphics_context_set_stroke_width(ctx, 1);
      // arrowhead closing the gap at the arc's 40° end (upper right)
      GPoint pts[3] = { GPoint(cx + 8, cy - 8),
                        GPoint(cx,     cy - 6),
                        GPoint(cx + 6, cy     ) };
      GPathInfo info = { .num_points = 3, .points = pts };
      GPath *p = gpath_create(&info);
      if (p) { gpath_draw_filled(ctx, p); gpath_destroy(p); }
      break;
    }
    case ICON_REC:               // filled dot — start recording
      graphics_fill_circle(ctx, GPoint(cx, cy), 6);
      break;
    case ICON_STOP:              // filled square — stop recording
      graphics_fill_rect(ctx, GRect(cx - 5, cy - 5, 11, 11), 1, GCornersAll);
      break;
    case ICON_LIST:              // three bars — history
      for (int i = -1; i <= 1; i++) {
        graphics_fill_rect(ctx, GRect(cx - 7, cy + i * 5 - 1, 15, 3),
                           1, GCornersAll);
      }
      break;
    case ICON_FLIP: {            // up + down triangles — flip the card
      GPoint up[3]   = { GPoint(cx, cy - 8), GPoint(cx - 5, cy - 2),
                         GPoint(cx + 5, cy - 2) };
      GPoint down[3] = { GPoint(cx, cy + 8), GPoint(cx - 5, cy + 2),
                         GPoint(cx + 5, cy + 2) };
      GPathInfo iu = { .num_points = 3, .points = up };
      GPathInfo id = { .num_points = 3, .points = down };
      GPath *a = gpath_create(&iu);
      GPath *b = gpath_create(&id);
      if (a) { gpath_draw_filled(ctx, a); gpath_destroy(a); }
      if (b) { gpath_draw_filled(ctx, b); gpath_destroy(b); }
      break;
    }
  }
}

// ── Top status bar: direction badge + clock ───────────────────────────────
// Shared by every screen so the translation direction never depends on the
// background colour alone. `badge` is the pill text (e.g. "JP → EN").
static void draw_status_bar(GContext *ctx, int W, const char *badge) {
  graphics_context_set_fill_color(ctx, BAR_COL());
  graphics_fill_rect(ctx, GRect(0, 0, W, STATUS_H), 0, GCornerNone);

  // White pill with the direction, sized to the text
  GFont bf = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  GSize bs = graphics_text_layout_get_content_size(badge, bf,
    GRect(0, 0, W, STATUS_H), GTextOverflowModeFill, GTextAlignmentLeft);
  int pill_w = bs.w + 14;
  int pill_h = 20;
  int pill_y = (STATUS_H - pill_h) / 2;
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(4, pill_y, pill_w, pill_h), 5, GCornersAll);

  graphics_context_set_text_color(ctx, COL_INK);
  graphics_draw_text(ctx, badge, bf,
    GRect(4, pill_y - 2, pill_w, pill_h + 2),
    GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // Clock, right-aligned
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_clock_buf,
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(W - 60, pill_y - 2, 56, pill_h + 2),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

// ── Right-edge button rail ────────────────────────────────────────────────
// One chip per physical button, at that button's height, so UP / SELECT /
// DOWN read at a glance. Icon carries the meaning; the label confirms it.
static void draw_rail_chip(GContext *ctx, int W, int cy,
                           IconKind kind, const char *label, GColor icon_col) {
  // Centred so the wider label box below still lands inside the screen
  int cx = W - RAIL_W / 2 - 1;

  graphics_context_set_fill_color(ctx, BAR_COL());
  graphics_fill_rect(ctx, GRect(cx - CHIP / 2, cy - CHIP / 2, CHIP, CHIP),
                     6, GCornersAll);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_round_rect(ctx, GRect(cx - CHIP / 2, cy - CHIP / 2, CHIP, CHIP), 6);

  draw_icon(ctx, kind, cx, cy, icon_col);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, label,
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(cx - RAIL_W / 2, cy + CHIP / 2 - 1, RAIL_W, 18),
    GTextOverflowModeFill, GTextAlignmentCenter, NULL);
}

// Rail contents depend on the current screen.
static void draw_rail(GContext *ctx, int W, int H) {
  int up_y = STATUS_H + 18;
  int sel_y = H / 2;
  int dn_y = H * 82 / 100;

  if (s_state == STATE_LISTENING) {
    // Mode switch is unavailable mid-recording — show it muted.
    draw_rail_chip(ctx, W, up_y,  ICON_ROTATE, "",
                   GColorFromRGB(170, 170, 170));
    draw_rail_chip(ctx, W, sel_y, ICON_STOP, "\xe5\x81\x9c\xe6\xad\xa2",   // 停止
                   GColorRed);
  } else if (s_state == STATE_RESULT && s_translated[0]) {
    draw_rail_chip(ctx, W, up_y,  ICON_FLIP, "\xe5\x8f\x8d\xe8\xbb\xa2",   // 反転
                   GColorWhite);
    draw_rail_chip(ctx, W, sel_y, ICON_REC, "\xe9\x8c\xb2\xe9\x9f\xb3",    // 録音
                   GColorRed);
  } else {
    draw_rail_chip(ctx, W, up_y,  ICON_ROTATE, "\xe5\x88\x87\xe6\x9b\xbf", // 切替
                   GColorWhite);
    draw_rail_chip(ctx, W, sel_y, ICON_REC, "\xe9\x8c\xb2\xe9\x9f\xb3",    // 録音
                   GColorRed);
  }

  draw_rail_chip(ctx, W, dn_y, ICON_LIST, "\xe5\xb1\xa5\xe6\xad\xb4",      // 履歴
                 GColorWhite);
}

// Badge text for the current translation direction
static const char *direction_badge(void) {
  return s_mode_ejp ? "EN \xe2\x86\x92 JP" : "JP \xe2\x86\x92 EN";
}

// ── Microphone glyph inside a ring (recording screen focal point) ─────────
static void draw_mic(GContext *ctx, int cx, int cy, int r) {
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 3);
  graphics_draw_circle(ctx, GPoint(cx, cy), r);
  graphics_context_set_stroke_width(ctx, 1);

  int bw = r * 4 / 9;          // capsule width
  int bh = r;                  // capsule height
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, GRect(cx - bw / 2, cy - bh * 3 / 5, bw, bh),
                     bw / 2, GCornersAll);

  // Cradle under the capsule, plus a short stem
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_arc(ctx,
    GRect(cx - bw, cy - bh / 4, bw * 2, bh * 3 / 4 + 4),
    GOvalScaleModeFitCircle, DEG_TO_TRIGANGLE(90), DEG_TO_TRIGANGLE(270));
  graphics_draw_line(ctx, GPoint(cx, cy + bh / 2), GPoint(cx, cy + bh * 4 / 5));
  graphics_context_set_stroke_width(ctx, 1);
}

// ── Draw face ─────────────────────────────────────────────────────────────
static void draw_face(GContext *ctx) {
  if (!s_face_path) return;

  graphics_context_set_stroke_color(ctx, GColorYellow);
  graphics_context_set_fill_color(ctx,   GColorYellow);

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

  // Mouth: yellow outer ring, darker inner fill (open-mouth effect)
  graphics_fill_circle(ctx, s_mouth_pos, s_mouth_r_out);
  graphics_context_set_fill_color(ctx, BAR_COL());
  graphics_fill_circle(ctx, s_mouth_pos, s_mouth_r_in);

  // Sound waves visible only when listening
  if (s_state != STATE_LISTENING) return;
  graphics_context_set_stroke_color(ctx, GColorYellow);
  // sw1: horizontal line at mouth height
  graphics_draw_line(ctx, s_sw1a, s_sw1b);
  // sw2: arc downward
  graphics_draw_line(ctx, s_sw2a, s_sw2b);
  graphics_draw_line(ctx, s_sw2b, s_sw2c);
  // sw3: wider arc
  graphics_draw_line(ctx, s_sw3a, s_sw3b);
  graphics_draw_line(ctx, s_sw3b, s_sw3c);
}

// ── Pick the largest readable font that suits the text length ──────────────
// Floor is GOTHIC_18 (18 px ≈ 2.3 mm) so even long results stay readable.
static const char *font_for_len(int len) {
  if (len <= 18)  return FONT_KEY_GOTHIC_28_BOLD;
  if (len <= 45)  return FONT_KEY_GOTHIC_24_BOLD;
  return FONT_KEY_GOTHIC_18_BOLD;
}

// ── Result screen: one big white card holding the translation ─────────────
static void draw_result(GContext *ctx, int W, int H) {
  int card_x = 5;
  int card_y = STATUS_H + 4;
  int card_w = W - card_x - RAIL_W;
  int card_h = H - card_y - 6;

  // Apply card squish during the flip animation: scale x about the card centre
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
  GRect pad = GRect(card_x + 6, card_y + 6, card_w - 12, card_h - 12);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, card_rect, 9, GCornersAll);

  if (sq_pct >= 20 && pad.size.w > 10) {
    graphics_context_set_text_color(ctx, COL_INK);
    GSize ts = graphics_text_layout_get_content_size(s_translated, font,
      pad, GTextOverflowModeWordWrap, GTextAlignmentCenter);
    int ty = pad.origin.y;
    if (ts.h < pad.size.h) ty += (pad.size.h - ts.h) / 2;
    graphics_draw_text(ctx, s_translated, font,
      GRect(pad.origin.x, ty, pad.size.w, pad.size.h),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }

  // Rotate the card — and only the card — when showing it to the other person
  if (s_result_flipped && sq_pct >= 95) {
    rotate_region_180(ctx, card_rect);
  }

  // Flip affordance, drawn after the rotation so it always reads upright
  if (sq_pct >= 95) {
    int pw = 50, ph = 18;
    int px = card_x + card_w - pw - 6;
    int py = card_y + 6;
    graphics_context_set_fill_color(ctx, COL_ACCENT);
    graphics_fill_rect(ctx, GRect(px, py, pw, ph), 5, GCornersAll);
    draw_icon(ctx, ICON_FLIP, px + 11, py + ph / 2, GColorWhite);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "\xe5\x8f\x8d\xe8\xbb\xa2",   // 反転
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(px + 20, py - 1, pw - 22, ph),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);
  }
}

// ── Main canvas ───────────────────────────────────────────────────────────
static void canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int W = b.size.w;
  int H = b.size.h;

  // Squish factor for the mode-toggle animation (100 = full, 0 = collapsed)
  int sq_pct = (s_flip_phase >= 0 && s_flip_phase < 8)
               ? FLIP_SQ[s_flip_phase] : 100;

  // Background: sky-blue (JP→EN) / warm-orange (EN→JP)
  graphics_context_set_fill_color(ctx, BG_COL());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Content is centred in the screen minus the button rail; the flip
  // animation squishes x about that centre, not the screen centre.
  int cc = (W - RAIL_W) / 2;
  #define SQX(x) (cc + ((x) - cc) * sq_pct / 100)
  #define SQW(w) ((w) * sq_pct / 100)

  // ── RESULT: one big card with the translation ───────────────────────────
  if (s_state == STATE_RESULT && s_translated[0]) {
    draw_status_bar(ctx, W, direction_badge());
    draw_result(ctx, W, H);
    draw_rail(ctx, W, H);
    return;
  }

  if (s_state == STATE_LISTENING) {
    // ── LISTENING: mic, status text, and the animated level meter ─────────
    int mic_cy = STATUS_H + (H - STATUS_H) * 30 / 100;
    draw_mic(ctx, cc, mic_cy, 24);

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx,
      "\xe8\x81\x9e\xe3\x81\x8d\xe5\x8f\x96\xe3\x82\x8a"      // 聞き取り
      "\xe4\xb8\xad\xe2\x80\xa6",                              // 中…
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(4, mic_cy + 34, W - RAIL_W - 8, 34),
      GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    static const int8_t wh[4][13] = {
      { 5, 8,12,16,20,22,20,16,12, 8, 5, 4, 3},
      { 3, 5, 8,14,20,22,20,14, 8, 5, 3, 5, 8},
      { 5, 9,14,18,20,18,14, 9, 5, 4, 6,10,14},
      { 4, 7,12,18,22,20,16,12, 7, 5, 8,12,16},
    };
    int bar_total = 13 * 7 - 3;
    int x0 = cc - bar_total / 2;
    int bar_base = H - 18;
    graphics_context_set_fill_color(ctx, GColorWhite);
    for (int i = 0; i < 13; i++) {
      int h = wh[s_wave_phase][i];
      graphics_fill_rect(ctx, GRect(x0 + i * 7, bar_base - h, 4, h),
                         1, GCornersAll);
    }
  } else {
    // ── HOME: face + prompt ───────────────────────────────────────────────
    if (sq_pct < 100 && s_face_path) {
      // Squish the face outline in place for the flip animation
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

    int prompt_x = SQX(4);
    int prompt_w = SQW(W - RAIL_W - 8);
    if (prompt_w < 4) prompt_w = 4;

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx,
      s_mode_ejp ? "Please speak\nin English"
                 : "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xa7\n"  // 日本語で
                   "\xe8\xa9\xb1\xe3\x81\x97\xe3\x81\xa6\xe4\xb8\x8b"    // 話して下
                   "\xe3\x81\x95\xe3\x81\x84",                            // さい
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(prompt_x, s_face_bot_y + 6, prompt_w, 70),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }

  draw_status_bar(ctx, W, direction_badge());
  draw_rail(ctx, W, H);

  #undef SQX
  #undef SQW
}

// ── Log canvas ────────────────────────────────────────────────────────────
static void log_canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int W = b.size.w;
  int H = b.size.h;

  graphics_context_set_fill_color(ctx, BG_COL());
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Same status bar as every other screen, badged "履歴"
  draw_status_bar(ctx, W, "\xe5\xb1\xa5\xe6\xad\xb4");

  if (s_log_n == 0) {
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, "No history yet",
      fonts_get_system_font(FONT_KEY_GOTHIC_18),
      GRect(10, STATUS_H + 10, W - 20, 26),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    return;
  }

  // Cards show the timestamp plus the translation — the recognised source is
  // mostly kanji, so the readable result is what earns the space.
  int lm       = 6;
  int card_h   = (H - STATUS_H - 18) / 2;
  int card_gap = 6;
  int y        = STATUS_H + 6;

  for (int i = 0; i < s_log_n && y + card_h <= H; i++) {
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(lm, y, W - lm * 2, card_h), 8, GCornersAll);

    graphics_context_set_text_color(ctx, COL_ACCENT);
    graphics_draw_text(ctx, s_log[i].timestamp,
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(lm + 8, y + 3, 60, 22),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    graphics_context_set_text_color(ctx, COL_INK);
    graphics_draw_text(ctx, s_log[i].translated,
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(lm + 8, y + 24, W - lm * 2 - 16, card_h - 28),
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

  // Build face path and all element positions for this screen size
  build_layout(bounds);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_draw);
  layer_add_child(root, s_canvas);

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  snprintf(s_clock_buf, sizeof(s_clock_buf), "%02d:%02d", t->tm_hour, t->tm_min);

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
