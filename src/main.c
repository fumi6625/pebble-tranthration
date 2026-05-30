#include <pebble.h>

#define KEY_TEXT             0
#define KEY_LANG             1
#define KEY_RESULT           2
#define MSG_BUFFER_SIZE      512
#define TRANSLATE_TIMEOUT_MS 20000
#define LOG_MAX              10
#define WAVE_TIMER_MS        120

typedef enum {
  STATE_HOME,
  STATE_LISTENING,
  STATE_RESULT,
} AppState;

typedef struct {
  char original[150];
  char translated[201];
  char timestamp[12];
} LogEntry;

// ── Global state ──────────────────────────────────────────────────────────
static AppState          s_state          = STATE_HOME;
static char              s_dictated_text[512];
static char              s_translated_text[512];
static char              s_clock_text[10];

// ── Log ───────────────────────────────────────────────────────────────────
static LogEntry  s_log[LOG_MAX];
static int       s_log_count = 0;

// ── Timers ────────────────────────────────────────────────────────────────
static AppTimer *s_watchdog   = NULL;
static AppTimer *s_wave_timer = NULL;
static int       s_wave_phase = 0;

// ── Main window ───────────────────────────────────────────────────────────
static Window    *s_window      = NULL;
static Layer     *s_canvas      = NULL;
static TextLayer *s_clock_layer = NULL;

// ── Log window ────────────────────────────────────────────────────────────
static Window    *s_log_window = NULL;
static Layer     *s_log_canvas = NULL;
static int        s_log_scroll = 0;

// ── Forward declarations ──────────────────────────────────────────────────
static void start_dictation(void);
static void send_translation_request(void);

// ── DictationSession ──────────────────────────────────────────────────────
static DictationSession *s_dictation = NULL;

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

// ── Log management ────────────────────────────────────────────────────────
static void add_to_log(const char *original, const char *translated) {
  int n = (s_log_count < LOG_MAX) ? s_log_count : LOG_MAX - 1;
  memmove(&s_log[1], &s_log[0], n * sizeof(LogEntry));
  if (s_log_count < LOG_MAX) { s_log_count++; }
  snprintf(s_log[0].original,   sizeof(s_log[0].original),   "%s", original);
  snprintf(s_log[0].translated, sizeof(s_log[0].translated), "%s", translated);

  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  snprintf(s_log[0].timestamp, sizeof(s_log[0].timestamp),
           "%02d:%02d", t->tm_hour, t->tm_min);
}

// ── Clock tick ────────────────────────────────────────────────────────────
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  snprintf(s_clock_text, sizeof(s_clock_text),
           "%02d:%02d", tick_time->tm_hour, tick_time->tm_min);
  if (s_clock_layer) { text_layer_set_text(s_clock_layer, s_clock_text); }
}

// ── Wave animation ────────────────────────────────────────────────────────
static void wave_timer_callback(void *ctx) {
  s_wave_phase = (s_wave_phase + 1) % 4;
  canvas_mark_dirty();
  if (s_state == STATE_LISTENING) {
    s_wave_timer = app_timer_register(WAVE_TIMER_MS, wave_timer_callback, NULL);
  } else {
    s_wave_timer = NULL;
  }
}

static void start_wave(void) {
  stop_wave();
  s_wave_phase = 0;
  s_wave_timer = app_timer_register(WAVE_TIMER_MS, wave_timer_callback, NULL);
}

// ── Drawing helpers ───────────────────────────────────────────────────────

// Draw a simplified face profile (C-shape, facing left) in yellow
static void draw_face(GContext *ctx, GPoint center, int radius) {
#ifdef PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorFromRGB(255, 213, 0));
  graphics_context_set_fill_color(ctx,   GColorFromRGB(255, 213, 0));
#else
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_fill_color(ctx,   GColorBlack);
#endif
  graphics_context_set_stroke_width(ctx, 3);

  // Head circle outline (C-shape: draw most of circle, leave opening for mouth area)
  // Use full circle then overdraw the gap
  graphics_draw_circle(ctx, center, radius);

  // Eye: small filled circle, upper-left area of face
  GPoint eye = { center.x - radius / 3, center.y - radius / 3 };
  graphics_fill_circle(ctx, eye, 2);

  // Mouth: small arc opening (simulate with a small filled circle)
  GPoint mouth = { center.x - radius / 2, center.y + radius / 5 };
  graphics_fill_circle(ctx, mouth, 3);

  // Sound wave lines to the left of face (3 arcs approximated as curved lines)
  // Only draw when listening
  if (s_state == STATE_LISTENING) {
#ifdef PBL_COLOR
    graphics_context_set_stroke_color(ctx, GColorFromRGB(255, 255, 255));
#else
    graphics_context_set_stroke_color(ctx, GColorBlack);
#endif
    graphics_context_set_stroke_width(ctx, 2);
    for (int i = 1; i <= 3; i++) {
      int ox = center.x - radius - 4 - i * 8;
      int wh = i * 8;
      GRect arc_rect = GRect(ox - wh / 2, center.y - wh / 2, wh, wh);
      graphics_draw_arc(ctx, arc_rect, GOvalScaleModeFitCircle,
                        DEG_TO_TRIGANGLE(-60), DEG_TO_TRIGANGLE(60));
    }
  }
}

// Draw animated wave bars at the bottom (listening indicator)
static void draw_wave_bars(GContext *ctx, GRect bounds) {
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(255, 255, 255));
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif

  static const int8_t heights[4][5] = {
    {4, 10, 18, 10, 4},
    {6, 16, 10, 16, 6},
    {4, 10, 18, 10, 4},
    {8, 14,  8, 14, 8},
  };

  int total_w = 5 * 6 + 4 * 4;  // 5 bars (6px) + 4 gaps (4px)
  int x_start = (bounds.size.w - total_w) / 2;
  int y_base  = bounds.size.h - 22;

  for (int i = 0; i < 5; i++) {
    int h = heights[s_wave_phase][i];
    int x = x_start + i * (6 + 4);
    graphics_fill_rect(ctx, GRect(x, y_base - h, 6, h), 0, GCornerNone);
  }
}

// ── Main canvas draw ──────────────────────────────────────────────────────
static void canvas_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Background
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(85, 170, 255));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // "JP→EN" top-left
#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorBlack);
#endif
  graphics_draw_text(ctx, "JP\xe2\x86\x92" "EN",  // JP→EN (UTF-8 arrow)
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(4, 2, 60, 20),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  // Face (center-left area)
  GPoint face_center = { bounds.size.w / 2 - 8, bounds.size.h / 2 - 10 };
  draw_face(ctx, face_center, 28);

  if (s_state == STATE_HOME || s_state == STATE_RESULT) {
    // Prompt or result text
    const char *body = (s_state == STATE_RESULT && s_translated_text[0])
                       ? s_translated_text
                       : "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xa7\xe8\xa9\xb1\xe3\x81\x97\xe3\x81\x8b\xe3\x81\x91\xe3\x81\xa6\xe3\x81\x8f\xe3\x81\xa0\xe3\x81\x95\xe3\x81\x84\xe3\x80\x82";
    // "日本語で話しかけてください。"
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorWhite);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, body,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(4, bounds.size.h - 52, bounds.size.w - 18, 46),
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);

    // "REC" label on right edge (middle)
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorRed);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, "REC",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(bounds.size.w - 20, bounds.size.h / 2 - 10, 20, 20),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);

    // "LOG" label on right edge (bottom)
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorWhite);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, "LOG",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(bounds.size.w - 22, bounds.size.h - 28, 22, 20),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);

  } else if (s_state == STATE_LISTENING) {
    // Listening state text
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorWhite);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    // "聞き取り中…" in UTF-8
    graphics_draw_text(ctx,
                       "\xe8\x81\x9e\xe3\x81\x8d\xe5\x8f\x96\xe3\x82\x8a\xe4\xb8\xad\xe2\x80\xa6",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(4, bounds.size.h - 60, bounds.size.w - 18, 30),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    draw_wave_bars(ctx, bounds);

    // "STOP" label on right edge (middle)
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorRed);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, "STP",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(bounds.size.w - 22, bounds.size.h / 2 - 10, 22, 20),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
}

// ── Log window drawing ────────────────────────────────────────────────────
static void log_canvas_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Orange background
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(255, 128, 0));
#else
  graphics_context_set_fill_color(ctx, GColorWhite);
#endif
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Header
#ifdef PBL_COLOR
  graphics_context_set_fill_color(ctx, GColorFromRGB(200, 80, 0));
#else
  graphics_context_set_fill_color(ctx, GColorBlack);
#endif
  graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, 26), 0, GCornerNone);

#ifdef PBL_COLOR
  graphics_context_set_text_color(ctx, GColorWhite);
#else
  graphics_context_set_text_color(ctx, GColorWhite);
#endif
  // "LOG  直近10件" header
  graphics_draw_text(ctx, "LOG  \xe7\x9b\xb4\xe8\xbf\x9110\xe4\xbb\xb6",
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(4, 2, bounds.size.w - 8, 22),
                     GTextOverflowModeFill, GTextAlignmentLeft, NULL);

  if (s_log_count == 0) {
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorWhite);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, "No history yet",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(8, 40, bounds.size.w - 16, 30),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);
    return;
  }

  int y = 30;
  for (int i = 0; i < s_log_count && y < bounds.size.h; i++) {
    // Card background
#ifdef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorFromRGB(255, 255, 220));
#else
    graphics_context_set_fill_color(ctx, GColorWhite);
#endif
    graphics_fill_rect(ctx, GRect(4, y, bounds.size.w - 8, 54), 3, GCornersAll);

    // Timestamp
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorFromRGB(100, 60, 0));
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, s_log[i].timestamp,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(8, y + 2, 50, 16),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // Japanese original (truncated if needed)
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorFromRGB(80, 50, 0));
#else
    graphics_context_set_text_color(ctx, GColorDarkGray);
#endif
    graphics_draw_text(ctx, s_log[i].original,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(8, y + 18, bounds.size.w - 16, 16),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    // English translation
#ifdef PBL_COLOR
    graphics_context_set_text_color(ctx, GColorBlack);
#else
    graphics_context_set_text_color(ctx, GColorBlack);
#endif
    graphics_draw_text(ctx, s_log[i].translated,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(8, y + 34, bounds.size.w - 16, 18),
                       GTextOverflowModeFill, GTextAlignmentLeft, NULL);

    y += 60;
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
                                       char *transcription,
                                       void *context) {
  stop_wave();
  if (status == DictationSessionStatusSuccess) {
    snprintf(s_dictated_text, sizeof(s_dictated_text), "%s", transcription);
    // Immediately send translation request
    s_state = STATE_HOME;
    canvas_mark_dirty();
    send_translation_request();
  } else {
    s_state = STATE_HOME;
    s_translated_text[0] = '\0';
    canvas_mark_dirty();
  }
}

// ── AppMessage ────────────────────────────────────────────────────────────
static void send_translation_request(void) {
  cancel_watchdog();

  char safe_text[300];
  snprintf(safe_text, sizeof(safe_text), "%s", s_dictated_text);

  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK) { return; }
  dict_write_cstring(iter, KEY_TEXT, safe_text);
  dict_write_cstring(iter, KEY_LANG, "en");
  result = app_message_outbox_send();
  if (result != APP_MSG_OK) { return; }
  s_watchdog = app_timer_register(TRANSLATE_TIMEOUT_MS, watchdog_callback, NULL);
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  cancel_watchdog();
  Tuple *t = dict_find(iter, KEY_RESULT);
  if (!t) { return; }

  const char *result = t->value->cstring;

  if (strncmp(result, "JS OK", 5) == 0) {
    return;
  }

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

static void outbox_sent_callback(DictionaryIterator *iter, void *context) {
  // Translation request sent; waiting for response
}

// ── Button handlers ───────────────────────────────────────────────────────
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  // UP: re-translate last dictated text if available
  if (s_dictated_text[0] != '\0' && s_state != STATE_LISTENING) {
    send_translation_request();
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_state == STATE_LISTENING) {
    // Stop dictation
    dictation_session_stop(s_dictation);
    stop_wave();
    s_state = STATE_HOME;
    canvas_mark_dirty();
  } else {
    start_dictation();
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_log_window) { window_stack_push(s_log_window, true); }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP,     up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN,   down_click_handler);
}

// ── Dictation start ───────────────────────────────────────────────────────
static void start_dictation(void) {
  if (!s_dictation) { return; }
  s_state = STATE_LISTENING;
  canvas_mark_dirty();
  start_wave();
  dictation_session_start(s_dictation);
}

// ── Log window ────────────────────────────────────────────────────────────
static void log_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_log_canvas = layer_create(bounds);
  layer_set_update_proc(s_log_canvas, log_canvas_draw);
  layer_add_child(root, s_log_canvas);
}

static void log_window_appear(Window *window) {
  if (s_log_canvas) { layer_mark_dirty(s_log_canvas); }
}

static void log_window_unload(Window *window) {
  if (s_log_canvas) { layer_destroy(s_log_canvas); s_log_canvas = NULL; }
}

static void log_select_click(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop(true);
}

static void log_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, log_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK,   log_select_click);
}

// ── Main window ───────────────────────────────────────────────────────────
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_canvas = layer_create(bounds);
  layer_set_update_proc(s_canvas, canvas_draw);
  layer_add_child(root, s_canvas);

  // Clock TextLayer (top-right)
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  snprintf(s_clock_text, sizeof(s_clock_text), "%02d:%02d", t->tm_hour, t->tm_min);
  s_clock_layer = text_layer_create(GRect(bounds.size.w - 46, 2, 44, 20));
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

  s_dictation = dictation_session_create(sizeof(s_dictated_text),
                                         dictation_session_callback, NULL);
}

static void window_unload(Window *window) {
  cancel_watchdog();
  stop_wave();
  if (s_dictation) { dictation_session_destroy(s_dictation); s_dictation = NULL; }
  if (s_clock_layer) { text_layer_destroy(s_clock_layer); s_clock_layer = NULL; }
  if (s_canvas) { layer_destroy(s_canvas); s_canvas = NULL; }
}

// ── App lifecycle ─────────────────────────────────────────────────────────
static void init(void) {
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);
  app_message_open(MSG_BUFFER_SIZE, MSG_BUFFER_SIZE);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  // Log window
  s_log_window = window_create();
  window_set_window_handlers(s_log_window, (WindowHandlers){
    .load   = log_window_load,
    .appear = log_window_appear,
    .unload = log_window_unload,
  });
  window_set_click_config_provider(s_log_window, log_click_config);

  // Main window
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
