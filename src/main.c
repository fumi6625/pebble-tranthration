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

// ── State ─────────────────────────────────────────────────────────────────
static AppState          s_state = STATE_HOME;
static char              s_dictated_text[512];
static char              s_translated_text[512];
static char              s_clock_text[8];
static LogEntry          s_log[LOG_MAX];
static int               s_log_count = 0;

// ── Timers ────────────────────────────────────────────────────────────────
static AppTimer         *s_watchdog   = NULL;
static AppTimer         *s_wave_timer = NULL;
static int               s_wave_phase = 0;

// ── Windows / Layers ──────────────────────────────────────────────────────
static Window           *s_window      = NULL;
static Layer            *s_canvas      = NULL;
static TextLayer        *s_clock_layer = NULL;
static Window           *s_log_window  = NULL;
static Layer            *s_log_canvas  = NULL;
static DictationSession *s_dictation   = NULL;

// ── Face GPath ────────────────────────────────────────────────────────────
// Approximated from SVG: M70 22 C90 26 98 44 97 61 C96 78 88 92 70 98
//   C58 101 48 100 43 93 L40 84 L47 73 L32 62 L47 53
//   C49 45 47 37 53 31 C58 25 63 22 70 22 Z  (viewBox 0 0 120 120)
// Scale 0.60, offset (+23, +35) → face center ≈ (62, 72), h≈46px
static GPoint s_face_points[] = {
  {65, 48}, {72, 51}, {76, 55}, {79, 60}, {81, 66},
  {81, 72}, {81, 78}, {78, 83}, {75, 87}, {71, 91},
  {65, 94}, {55, 94}, {49, 91},
  {47, 85}, {51, 79}, {42, 72}, {51, 67},
  {52, 60}, {55, 54}, {60, 49}, {65, 48},
};
static const GPathInfo s_face_path_info = {
  .num_points = 21,
  .points = s_face_points,
};
static GPath *s_face_path = NULL;

// ── Forward declarations ──────────────────────────────────────────────────
static void start_dictation(void);
static void send_translation_request(void);

// ── Helpers ───────────────────────────────────────────────────────────────
static void cancel_watchdog(void) {
  if (s_watchdog) { app_timer_cancel(s_watchdog); s_watchdog = NULL; }
}
static void stop_wave(void) {
  if (s_wave_timer) { app_timer_cancel(s_wave_timer); s_wave_timer = NULL; }
}
static void canvas_mark_dirty(void) {
  if (s_canvas) { layer_mark_dirty(s_canvas); }
}

// ── Log ───────────────────────────────────────────────────────────────────
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

// ── Clock ─────────────────────────────────────────────────────────────────
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  snprintf(s_clock_text, sizeof(s_clock_text),
           "%02d:%02d", tick_time->tm_hour, tick_time->tm_min);
  if (s_clock_layer) text_layer_set_text(s_clock_layer, s_clock_text);
}

// ── Wave animation ────────────────────────────────────────────────────────
static void wave_timer_callback(void *ctx) {
  s_wave_phase = (s_wave_phase + 1) % 4;
  canvas_mark_dirty();
  s_wave_timer = (s_state == STATE_LISTENING)
    ? app_timer_register(WAVE_TIMER_MS, wave_timer_callback, NULL)
    : NULL;
}
static void start_wave(void) {
  stop_wave();
  s_wave_phase = 0;
  s_wave_timer = app_timer_register(WAVE_TIMER_MS, wave_timer_callback, NULL);
}

// ── Canvas draw ───────────────────────────────────────────────────────────
static void canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  // Background
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(85, 170, 255));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // ── Face outline (yellow on color, black on B&W) ──────────────────────
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorYellow);
  graphics_context_set_fill_color(ctx,   GColorYellow);
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx,   GColorBlack);
#endif
  if (s_face_path) {
    gpath_move_to(s_face_path, GPoint(0, 0));
    gpath_draw_outline(ctx, s_face_path);
    gpath_move_to(s_face_path, GPoint(1, 0));
    gpath_draw_outline(ctx, s_face_path);
    gpath_move_to(s_face_path, GPoint(0, 1));
    gpath_draw_outline(ctx, s_face_path);
  }

  // Eye at SVG (62,48) → Pebble (60, 64)
  graphics_fill_circle(ctx, GPoint(60, 64), 3);

  // Mouth at SVG (42,78) → Pebble (48, 82) — filled ellipse approx
  graphics_fill_circle(ctx, GPoint(48, 82), 4);
#ifdef PBL_COLOR
  // Dark blue fill inside to give "open mouth" effect
  graphics_context_set_fill_color(ctx, GColorFromRGB(0, 85, 170));
  graphics_fill_circle(ctx, GPoint(48, 82), 2);
#endif

  // Sound waves when listening (3 lines to the left of mouth)
  if (s_state == STATE_LISTENING) {
#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorYellow);
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    // sw1: short horizontal line at mouth level
    graphics_draw_line(ctx, GPoint(39, 82), GPoint(34, 82));
    // sw2: slight arc downward (2 segments)
    graphics_draw_line(ctx, GPoint(35, 77), GPoint(31, 81));
    graphics_draw_line(ctx, GPoint(31, 81), GPoint(30, 88));
    // sw3: longer arc (2 segments)
    graphics_draw_line(ctx, GPoint(32, 72), GPoint(26, 78));
    graphics_draw_line(ctx, GPoint(26, 78), GPoint(25, 94));
  }

  // ── "JP→EN" title, centered ───────────────────────────────────────────
  // Arrow U+2192 = \xe2\x86\x92
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx, "JP\xe2\x86\x92" "EN",
                     fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, 14, b.size.w - 16, 32),
                     GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  // ── Prompt / result / listening label ─────────────────────────────────
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  if (s_state == STATE_LISTENING) {
    // "聞き取り中…"
    graphics_draw_text(ctx,
      "\xe8\x81\x9e\xe3\x81\x8d\xe5\x8f\x96\xe3\x82\x8a\xe4\xb8\xad"
      "\xe2\x80\xa6",
      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
      GRect(4, 100, b.size.w - 22, 24),
      GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    // Wave bars (13 bars)
    static const int8_t wh[4][13] = {
      { 5, 8,12,16,20,22,20,16,12, 8, 5, 4, 3},
      { 3, 5, 8,14,20,22,20,14, 8, 5, 3, 5, 8},
      { 5, 9,14,18,20,18,14, 9, 5, 4, 6,10,14},
      { 4, 7,12,18,22,20,16,12, 7, 5, 8,12,16},
    };
    int x0 = (b.size.w - 13 * 7 + 3) / 2;
    for (int i = 0; i < 13; i++) {
      int h = wh[s_wave_phase][i];
#ifdef PBL_COLOR
      graphics_context_set_fill_color(ctx, GColorWhite);
#else
      graphics_context_set_fill_color(ctx, GColorBlack);
#endif
      graphics_fill_rect(ctx,
        GRect(x0 + i * 7, b.size.h - 16 - h, 4, h),
        1, GCornersAll);
    }
  } else {
    const char *body = (s_state == STATE_RESULT && s_translated_text[0])
      ? s_translated_text
      // "日本語で話しかけてください。"
      : "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xa7"
        "\xe8\xa9\xb1\xe3\x81\x97\xe3\x81\x8b\xe3\x81\x91"
        "\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95"
        "\xe3\x81\x84\xe3\x80\x82";
    graphics_draw_text(ctx, body,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(4, 100, b.size.w - 22, 62),
      GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }

  // ── Right-edge labels ─────────────────────────────────────────────────
  // REC (red) / STOP (white) at middle button height
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx,
    (s_state == STATE_LISTENING) ? GColorWhite : GColorRed);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx,
    (s_state == STATE_LISTENING) ? "STP" : "REC",
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
    GRect(b.size.w - 24, b.size.h / 2 - 9, 23, 16),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);

  // LOG (dark) at down button height
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorBlack);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx, "LOG",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(b.size.w - 24, b.size.h - 32, 23, 16),
    GTextOverflowModeFill, GTextAlignmentRight, NULL);
}

// ── Log canvas draw ───────────────────────────────────────────────────────
static void log_canvas_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);

  // Same sky-blue background as home
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(85, 170, 255));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  // Header bar (dark blue)
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
    GRect(8, 4, 48, 20), GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  // "直近10件" — split at \x91 to avoid hex-continuation error
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
    // Card
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorFromRGB(255, 255, 255));
    graphics_context_set_stroke_color(ctx, GColorFromRGB(170, 200, 255));
#else
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_fill_rect(ctx, GRect(6, y, b.size.w - 12, 52), 4, GCornersAll);

    // Timestamp
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorFromRGB(0, 85, 170));
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, s_log[i].timestamp,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(10, y + 2, 44, 14),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // Japanese original
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorBlack);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, s_log[i].original,
      fonts_get_system_font(FONT_KEY_GOTHIC_14),
      GRect(10, y + 16, b.size.w - 20, 14),
      GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // English translation (yellow on color)
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
static void watchdog_callback(void *context) {
  s_watchdog = NULL;
  stop_wave();
  s_state = STATE_HOME;
  canvas_mark_dirty();
}

// ── Dictation ─────────────────────────────────────────────────────────────
static void dictation_session_callback(DictationSession *session,
                                       DictationSessionStatus status,
                                       char *transcription, void *context) {
  stop_wave();
  if (status == DictationSessionStatusSuccess) {
    snprintf(s_dictated_text, sizeof(s_dictated_text), "%s", transcription);
    s_state = STATE_HOME;
    canvas_mark_dirty();
    send_translation_request();
  } else {
    s_state = STATE_HOME;
    canvas_mark_dirty();
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
  s_watchdog = app_timer_register(TRANSLATE_TIMEOUT_MS, watchdog_callback, NULL);
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  cancel_watchdog();
  Tuple *t = dict_find(iter, KEY_RESULT);
  if (!t) return;
  const char *result = t->value->cstring;
  if (strncmp(result, "JS OK", 5) == 0) return;
  snprintf(s_translated_text, sizeof(s_translated_text), "%s", result);
  add_to_log(s_dictated_text, s_translated_text);
  s_state = STATE_RESULT;
  canvas_mark_dirty();
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  cancel_watchdog();
}
static void outbox_failed_callback(DictionaryIterator *iter,
                                   AppMessageResult reason, void *context) {
  cancel_watchdog();
}
static void outbox_sent_callback(DictionaryIterator *iter, void *context) {}

// ── Buttons ───────────────────────────────────────────────────────────────
static void up_click_handler(ClickRecognizerRef r, void *ctx) {
  if (s_dictated_text[0] && s_state != STATE_LISTENING)
    send_translation_request();
}
static void select_click_handler(ClickRecognizerRef r, void *ctx) {
  if (s_state == STATE_LISTENING) {
    dictation_session_stop(s_dictation);
    stop_wave();
    s_state = STATE_HOME;
    canvas_mark_dirty();
  } else {
    start_dictation();
  }
}
static void down_click_handler(ClickRecognizerRef r, void *ctx) {
  if (s_log_window) window_stack_push(s_log_window, true);
}
static void click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP,     up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN,   down_click_handler);
}

// ── Dictation start ───────────────────────────────────────────────────────
static void start_dictation(void) {
  if (!s_dictation) return;
  s_state = STATE_LISTENING;
  canvas_mark_dirty();
  start_wave();
  dictation_session_start(s_dictation);
}

// ── Log window ────────────────────────────────────────────────────────────
static void log_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_log_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_log_canvas, log_canvas_draw);
  layer_add_child(root, s_log_canvas);
}
static void log_window_appear(Window *w) {
  if (s_log_canvas) layer_mark_dirty(s_log_canvas);
}
static void log_window_unload(Window *w) {
  if (s_log_canvas) { layer_destroy(s_log_canvas); s_log_canvas = NULL; }
}
static void log_back_click(ClickRecognizerRef r, void *ctx) {
  window_stack_pop(true);
}
static void log_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, log_back_click);
  window_single_click_subscribe(BUTTON_ID_BACK,   log_back_click);
}

// ── Main window ───────────────────────────────────────────────────────────
static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect bounds = layer_get_bounds(root);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_draw);
  layer_add_child(root, s_canvas);

  // Clock (top-right, above face)
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

  s_face_path = gpath_create(&s_face_path_info);

  s_dictation = dictation_session_create(sizeof(s_dictated_text),
                                         dictation_session_callback, NULL);
}

static void window_unload(Window *w) {
  cancel_watchdog();
  stop_wave();
  if (s_face_path)   { gpath_destroy(s_face_path);            s_face_path   = NULL; }
  if (s_dictation)   { dictation_session_destroy(s_dictation); s_dictation   = NULL; }
  if (s_clock_layer) { text_layer_destroy(s_clock_layer);      s_clock_layer = NULL; }
  if (s_canvas)      { layer_destroy(s_canvas);                s_canvas      = NULL; }
}

// ── App lifecycle ─────────────────────────────────────────────────────────
static void init(void) {
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);
  app_message_open(MSG_BUFFER_SIZE, MSG_BUFFER_SIZE);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  s_log_window = window_create();
  window_set_window_handlers(s_log_window, (WindowHandlers){
    .load   = log_window_load,
    .appear = log_window_appear,
    .unload = log_window_unload,
  });
  window_set_click_config_provider(s_log_window, log_click_config);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  if (s_log_window) { window_destroy(s_log_window); s_log_window = NULL; }
  if (s_window)     { window_destroy(s_window);     s_window     = NULL; }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
