#include <pebble.h>

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

static AppState          s_state = STATE_HOME;
static char              s_dictated_text[512];
static char              s_translated_text[512];
static char              s_clock_text[8];
static LogEntry          s_log[LOG_MAX];
static int               s_log_count = 0;
static AppTimer         *s_watchdog   = NULL;
static AppTimer         *s_wave_timer = NULL;
static int               s_wave_phase = 0;
static Window           *s_window      = NULL;
static Layer            *s_canvas      = NULL;
static TextLayer        *s_clock_layer = NULL;
static Window           *s_log_window  = NULL;
static Layer            *s_log_canvas  = NULL;
static DictationSession *s_dictation   = NULL;

// ── Face GPath ────────────────────────────────────────────────────────────
// SVG path (viewBox 0 0 120 120, strokeWidth 7, facing left):
//   M70 22 C90 26 98 44 97 61 C96 78 88 92 70 98
//   C58 101 48 100 43 93 L40 84 L47 73 L32 62 L47 53
//   C49 45 47 37 53 31 C58 25 63 22 70 22 Z
// Bezier curves sampled at t=0,0.2,0.4,0.6,0.8,1.0
// Scale 0.84, offset (+18, +37) → face 55px wide, 65px tall
// Center on Pebble: x≈72, y≈87
static GPoint s_face_pts[] = {
  {77, 55},  // M70,22  — top of head
  {86, 59},  // C1 t=0.2  (≈81,26)
  {92, 65},  // C1 t=0.4  (≈88,33)
  {97, 71},  // C1 t=0.6  (≈94,41)
  {99, 80},  // C1 t=0.8  (≈96,51)
  {99, 88},  // C1 t=1.0  (97,61)  — rightmost, back of head
  {99, 97},  // C2 t=0.2  (≈96,71)
  {95, 104}, // C2 t=0.4  (≈92,80)
  {91, 110}, // C2 t=0.6  (≈87,87)
  {85, 115}, // C2 t=0.8  (≈80,93)
  {77, 119}, // C2 t=1.0  (70,98)  — bottom right
  {63, 120}, // C3 t=0.5  (≈54,99) — chin bottom
  {54, 115}, // C3 t=1.0  (43,93)  — chin left
  {52, 108}, // L40,84   — jaw indentation
  {57, 98},  // L47,73
  {45, 89},  // L32,62   — mouth indent, leftmost point
  {57, 82},  // L47,53
  {59, 71},  // C4 t=0.5  (≈49,41)
  {63, 63},  // C4 t=1.0  (53,31)  — upper back
  {69, 57},  // C5 t=0.5  (≈61,24)
              // path auto-closes to {77,55}
};
static const GPathInfo s_face_info = {
  .num_points = 20,
  .points     = s_face_pts,
};
static GPath *s_face_path = NULL;

// Eye:   SVG (62,48) → Pebble (70, 77),  r≈3
// Mouth: SVG (42,78) → Pebble (53,103), outer r=6 yellow, inner r=4 blue
// sw1:   SVG M22,78 L15,78  → Pebble (36,103)-(31,103)
// sw2:   SVG M18,70 Q9,75 9,86 → (33,96)-(27,101)-(26,109)
// sw3:   SVG M15,62 Q4,71 5,93 → (31,89)-(24,99)-(22,115)

static void start_dictation(void);
static void send_translation_request(void);

static void cancel_watchdog(void) {
  if (s_watchdog) { app_timer_cancel(s_watchdog); s_watchdog = NULL; }
}
static void stop_wave(void) {
  if (s_wave_timer) { app_timer_cancel(s_wave_timer); s_wave_timer = NULL; }
}
static void canvas_dirty(void) { if (s_canvas) layer_mark_dirty(s_canvas); }

static void add_to_log(const char *orig, const char *trans) {
  int n = (s_log_count < LOG_MAX) ? s_log_count : LOG_MAX - 1;
  memmove(&s_log[1], &s_log[0], n * sizeof(LogEntry));
  if (s_log_count < LOG_MAX) s_log_count++;
  snprintf(s_log[0].original,   sizeof(s_log[0].original),   "%.149s", orig);
  snprintf(s_log[0].translated, sizeof(s_log[0].translated), "%.200s", trans);
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  snprintf(s_log[0].timestamp, sizeof(s_log[0].timestamp),
           "%02d:%02d", t->tm_hour, t->tm_min);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  snprintf(s_clock_text, sizeof(s_clock_text),
           "%02d:%02d", tick_time->tm_hour, tick_time->tm_min);
  if (s_clock_layer) text_layer_set_text(s_clock_layer, s_clock_text);
}

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

// ── Draw face (outline + eye + mouth + sound waves) ───────────────────────
static void draw_face(GContext *ctx) {
  if (!s_face_path) return;

#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorYellow);
  graphics_context_set_fill_color(ctx,   GColorYellow);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx,   GColorBlack);
#endif

  // Draw outline 4× offset for ~2px stroke weight
  int ox, oy;
  for (ox = 0; ox <= 1; ox++) {
    for (oy = 0; oy <= 1; oy++) {
      gpath_move_to(s_face_path, GPoint(ox, oy));
      gpath_draw_outline(ctx, s_face_path);
    }
  }
  gpath_move_to(s_face_path, GPoint(0, 0));

  // Eye: filled circle
  graphics_fill_circle(ctx, GPoint(70, 77), 3);

  // Mouth: yellow outer then blue inner
  graphics_fill_circle(ctx, GPoint(53, 103), 6);
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(0, 85, 170));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_circle(ctx, GPoint(53, 103), 3);

  // Sound waves (visible when listening)
  if (s_state != STATE_LISTENING) return;
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorYellow);
  graphics_context_set_fill_color(ctx,   GColorYellow);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
  // sw1: horizontal line at mouth level
  graphics_draw_line(ctx, GPoint(36, 103), GPoint(31, 103));
  // sw2: arc curving down-left (Q9,75 9,86 in SVG)
  graphics_draw_line(ctx, GPoint(33, 96), GPoint(27, 101));
  graphics_draw_line(ctx, GPoint(27, 101), GPoint(26, 109));
  // sw3: wider arc (Q4,71 5,93 in SVG)
  graphics_draw_line(ctx, GPoint(31, 89), GPoint(24, 99));
  graphics_draw_line(ctx, GPoint(24, 99), GPoint(22, 115));
}

// ── Main canvas ───────────────────────────────────────────────────────────
static void canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  // Background
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(85, 170, 255));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Face
  draw_face(ctx);

  // ── "JP→EN" title (centered, below clock) ────────────────────────────
  // U+2192 → = \xe2\x86\x92
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx, "JP \xe2\x86\x92 EN",
                     fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(0, 17, b.size.w - 16, 30),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // ── Prompt / listening label ──────────────────────────────────────────
  if (s_state == STATE_LISTENING) {
    // "聞き取り中…"
    graphics_draw_text(ctx,
      "\xe8\x81\x9e\xe3\x81\x8d\xe5\x8f\x96\xe3\x82\x8a"
      "\xe4\xb8\xad\xe2\x80\xa6",
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(4, 124, b.size.w - 22, 24),
      GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    // Wave bars (13 bars, animated)
    static const int8_t wh[4][13] = {
      { 5, 8,12,16,20,22,20,16,12, 8, 5, 4, 3},
      { 3, 5, 8,14,20,22,20,14, 8, 5, 3, 5, 8},
      { 5, 9,14,18,20,18,14, 9, 5, 4, 6,10,14},
      { 4, 7,12,18,22,20,16,12, 7, 5, 8,12,16},
    };
    int x0 = (b.size.w - 88) / 2;  // 13*(4+3)-3=88
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorWhite);
#else
    graphics_context_set_fill_color(ctx, GColorBlack);
#endif
    for (int i = 0; i < 13; i++) {
      int h = wh[s_wave_phase][i];
      graphics_fill_rect(ctx,
        GRect(x0 + i * 7, b.size.h - 18 - h, 4, h), 1, GCornersAll);
    }
  } else {
    // HOME: "日本語で話しかけてください。"
    // RESULT: show translated text
    const char *body = (s_state == STATE_RESULT && s_translated_text[0])
      ? s_translated_text
      : "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xa7"
        "\xe8\xa9\xb1\xe3\x81\x97\xe3\x81\x8b\xe3\x81\x91"
        "\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95"
        "\xe3\x81\x84\xe3\x80\x82";
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorWhite);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, body,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(4, 124, b.size.w - 22, 40),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }

  // ── Right-edge labels ─────────────────────────────────────────────────
  // REC (red) at middle button / STOP when listening
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx,
    (s_state == STATE_LISTENING) ? GColorWhite : GColorRed);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx,
    (s_state == STATE_LISTENING) ? "STP" : "REC",
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
    GRect(b.size.w - 24, b.size.h / 2 - 8, 23, 16),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);

  // LOG at down-button level
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorBlack);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx, "LOG",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(b.size.w - 24, b.size.h - 34, 23, 16),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

// ── Log canvas ────────────────────────────────────────────────────────────
static void log_canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(85, 170, 255));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Header bar
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(0, 85, 170));
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, 28), 0, GCornerNone);

#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorBlack);
#else
  graphics_context_set_text_color(ctx, GColorWhite);
#endif
  graphics_draw_text(ctx, "LOG",
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(8, 3, 50, 22), GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  // "直近10件" (split at \x91 to avoid hex-continuation)
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorFromRGB(170, 220, 255));
#else
  graphics_context_set_text_color(ctx, GColorWhite);
#endif
  graphics_draw_text(ctx,
    "\xe7\x9b\xb4\xe8\xbf\x91" "10\xe4\xbb\xb6",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(b.size.w - 58, 7, 54, 16),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);

  if (s_log_count == 0) {
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorWhite);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, "No history yet",
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(8, 38, b.size.w - 16, 20),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    return;
  }

  int y = 32;
  for (int i = 0; i < s_log_count && y < b.size.h - 4; i++) {
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_context_set_stroke_color(ctx, GColorFromRGB(170, 200, 255));
#else
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_fill_rect(ctx, GRect(6, y, b.size.w - 12, 52), 4, GCornersAll);

#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorFromRGB(0, 85, 170));
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, s_log[i].timestamp,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(10, y + 2, 44, 14), GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, s_log[i].original,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(10, y + 16, b.size.w - 20, 14),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);

#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorYellow);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, s_log[i].translated,
      fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(10, y + 30, b.size.w - 20, 16),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    y += 58;
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
    snprintf(s_dictated_text, sizeof(s_dictated_text), "%s", transcription);
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
  char safe[200];
  snprintf(safe, sizeof(safe), "%s", s_dictated_text);
  dict_write_cstring(iter, KEY_TEXT, safe);
  dict_write_cstring(iter, KEY_LANG, "en");
  if (app_message_outbox_send() != APP_MSG_OK) return;
  s_watchdog = app_timer_register(TRANSLATE_TIMEOUT_MS, watchdog_cb, NULL);
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  cancel_watchdog();
  Tuple *t = dict_find(iter, KEY_RESULT);
  if (!t) return;
  const char *r = t->value->cstring;
  if (strncmp(r, "JS OK", 5) == 0) return;
  snprintf(s_translated_text, sizeof(s_translated_text), "%s", r);
  add_to_log(s_dictated_text, s_translated_text);
  s_state = STATE_RESULT;
  canvas_dirty();
}
static void inbox_dropped(AppMessageResult reason, void *ctx) { cancel_watchdog(); }
static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) { cancel_watchdog(); }
static void outbox_sent(DictionaryIterator *iter, void *ctx) {}

// ── Buttons ───────────────────────────────────────────────────────────────
static void btn_up(ClickRecognizerRef r, void *ctx) {
  if (s_dictated_text[0] && s_state != STATE_LISTENING)
    send_translation_request();
}
static void btn_select(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_LISTENING) {
    dictation_session_stop(s_dictation);
    stop_wave();
    s_state = STATE_HOME;
    canvas_dirty();
  } else {
    start_dictation();
  }
}
static void btn_down(ClickRecognizerRef r, void *ctx) {
  if (s_log_window) window_stack_push(s_log_window, true);
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
  s_log_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_log_canvas, log_canvas_draw);
  layer_add_child(root, s_log_canvas);
}
static void log_appear(Window *w) { if (s_log_canvas) layer_mark_dirty(s_log_canvas); }
static void log_unload(Window *w) {
  if (s_log_canvas) { layer_destroy(s_log_canvas); s_log_canvas = NULL; }
}
static void log_back(ClickRecognizerRef r, void *ctx) { window_stack_pop(true); }
static void log_clicks(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, log_back);
  window_single_click_subscribe(BUTTON_ID_BACK,   log_back);
}

// ── Main window ───────────────────────────────────────────────────────────
static void main_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect bounds = layer_get_bounds(root);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_draw);
  layer_add_child(root, s_canvas);

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  snprintf(s_clock_text, sizeof(s_clock_text), "%02d:%02d", t->tm_hour, t->tm_min);
  s_clock_layer = text_layer_create(GRect(bounds.size.w - 48, 1, 46, 14));
  text_layer_set_background_color(s_clock_layer, GColorClear);
#ifdef PBL_COLOR
  text_layer_set_text_color(s_clock_layer, GColorWhite);
#else
  text_layer_set_text_color(s_clock_layer, GColorBlack);
#endif
  text_layer_set_font(s_clock_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text_alignment(s_clock_layer, GTextAlignmentRight);
  text_layer_set_text(s_clock_layer, s_clock_text);
  layer_add_child(root, text_layer_get_layer(s_clock_layer));

  s_face_path = gpath_create(&s_face_info);
  s_dictation = dictation_session_create(sizeof(s_dictated_text), dictation_cb, NULL);
}

static void main_unload(Window *w) {
  cancel_watchdog();
  stop_wave();
  if (s_face_path)   { gpath_destroy(s_face_path);            s_face_path   = NULL; }
  if (s_dictation)   { dictation_session_destroy(s_dictation); s_dictation   = NULL; }
  if (s_clock_layer) { text_layer_destroy(s_clock_layer);      s_clock_layer = NULL; }
  if (s_canvas)      { layer_destroy(s_canvas);                s_canvas      = NULL; }
}

// ── App lifecycle ─────────────────────────────────────────────────────────
static void init(void) {
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_register_outbox_sent(outbox_sent);
  app_message_open(MSG_BUFFER_SIZE, MSG_BUFFER_SIZE);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  s_log_window = window_create();
  window_set_window_handlers(s_log_window, (WindowHandlers){
    .load = log_load, .appear = log_appear, .unload = log_unload });
  window_set_click_config_provider(s_log_window, log_clicks);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = main_load, .unload = main_unload });
  window_set_click_config_provider(s_window, click_provider);
  window_stack_push(s_window, true);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  if (s_log_window) { window_destroy(s_log_window); s_log_window = NULL; }
  if (s_window)     { window_destroy(s_window);     s_window     = NULL; }
}

int main(void) { init(); app_event_loop(); deinit(); return 0; }
