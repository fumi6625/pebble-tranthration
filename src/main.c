#include <pebble.h>

#define KEY_TEXT             0
#define KEY_LANG             1
#define KEY_RESULT           2
#define MSG_BUFFER_SIZE      512
#define TRANSLATE_TIMEOUT_MS 20000
#define LOG_MAX              10

typedef struct {
  char original[150];   // Japanese original (up to ~50 chars UTF-8)
  char translated[201]; // Translated result
} LogEntry;

// ── Main window state ──────────────────────────────────────────────────────
static Window           *s_window;
static TextLayer        *s_status_layer;
static TextLayer        *s_main_layer;
static DictationSession *s_dictation;
static AppTimer         *s_watchdog    = NULL;
static char              s_dictated_text[512];
static char              s_translated_text[512];

// ── Translation log ────────────────────────────────────────────────────────
static LogEntry  s_log[LOG_MAX];
static int       s_log_count = 0;

// ── Log window ─────────────────────────────────────────────────────────────
static Window    *s_log_window = NULL;
static MenuLayer *s_log_menu   = NULL;

// ── Forward declarations ───────────────────────────────────────────────────
static void start_dictation(void);

// ── Log management ─────────────────────────────────────────────────────────
static void add_to_log(const char *original, const char *translated) {
  // Shift existing entries so [0] is always the newest
  int n = (s_log_count < LOG_MAX) ? s_log_count : LOG_MAX - 1;
  memmove(&s_log[1], &s_log[0], n * sizeof(LogEntry));
  if (s_log_count < LOG_MAX) { s_log_count++; }
  snprintf(s_log[0].original,   sizeof(s_log[0].original),   "%s", original);
  snprintf(s_log[0].translated, sizeof(s_log[0].translated), "%s", translated);
}

// ── Log window callbacks ───────────────────────────────────────────────────
static uint16_t log_get_num_rows(MenuLayer *ml, uint16_t sec, void *ctx) {
  return (s_log_count == 0) ? 1 : (uint16_t)s_log_count;
}

static int16_t log_get_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return 44;
}

static void log_draw_row(GContext *ctx, const Layer *cell,
                         MenuIndex *idx, void *context) {
  if (s_log_count == 0) {
    menu_cell_basic_draw(ctx, cell, "No history yet", NULL, NULL);
    return;
  }
  // Title: translated text  Subtitle: original Japanese (kana renders; kanji may not)
  menu_cell_basic_draw(ctx, cell,
                       s_log[idx->row].translated,
                       s_log[idx->row].original,
                       NULL);
}

static void log_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  window_stack_pop(true);
}

static void log_window_appear(Window *window) {
  if (s_log_menu) { menu_layer_reload_data(s_log_menu); }
}

static void log_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_log_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_log_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows    = log_get_num_rows,
    .get_cell_height = log_get_cell_height,
    .draw_row        = log_draw_row,
    .select_click    = log_select,
  });
  menu_layer_set_click_config_onto_window(s_log_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_log_menu));
}

static void log_window_unload(Window *window) {
  menu_layer_destroy(s_log_menu);
  s_log_menu = NULL;
}

static void open_log(void) {
  if (!s_log_window) {
    s_log_window = window_create();
    window_set_window_handlers(s_log_window, (WindowHandlers){
      .load   = log_window_load,
      .unload = log_window_unload,
      .appear = log_window_appear,
    });
  }
  window_stack_push(s_log_window, true);
}

// ── Watchdog ───────────────────────────────────────────────────────────────
static void cancel_watchdog(void) {
  if (s_watchdog) { app_timer_cancel(s_watchdog); s_watchdog = NULL; }
}

static void watchdog_callback(void *context) {
  s_watchdog = NULL;
  text_layer_set_text(s_status_layer, "Timeout. SEL=Retry");
}

// ── Dictation ──────────────────────────────────────────────────────────────
static void dictation_session_callback(DictationSession *session,
                                       DictationSessionStatus status,
                                       char *transcription,
                                       void *context) {
  if (status == DictationSessionStatusSuccess) {
    snprintf(s_dictated_text, sizeof(s_dictated_text), "%s", transcription);
    text_layer_set_text(s_status_layer, "UP=EN SEL=Mic DN=Log");
    text_layer_set_text(s_main_layer, s_dictated_text);
  } else {
    static char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "Error: %d\nSEL to retry", (int)status);
    text_layer_set_text(s_status_layer, "Dictation failed");
    text_layer_set_text(s_main_layer, err_buf);
  }
}

// ── AppMessage ─────────────────────────────────────────────────────────────
static void send_translation_request(const char *lang) {
  cancel_watchdog();
  DictionaryIterator *iter;
  AppMessageResult result = app_message_outbox_begin(&iter);
  if (result != APP_MSG_OK) {
    text_layer_set_text(s_status_layer, "Outbox error");
    return;
  }
  dict_write_cstring(iter, KEY_TEXT, s_dictated_text);
  dict_write_cstring(iter, KEY_LANG, lang);
  result = app_message_outbox_send();
  if (result != APP_MSG_OK) {
    text_layer_set_text(s_status_layer, "Send error");
    return;
  }
  s_watchdog = app_timer_register(TRANSLATE_TIMEOUT_MS, watchdog_callback, NULL);
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  cancel_watchdog();
  Tuple *t = dict_find(iter, KEY_RESULT);
  if (!t) { return; }

  const char *result = t->value->cstring;

  // ZH confirmation sentinel — notification was shown on watch, nothing to log
  if (strncmp(result, "[ZH", 3) == 0) {
    text_layer_set_text(s_status_layer, "UP=EN SEL=Mic DN=Log");
    return;
  }

  snprintf(s_translated_text, sizeof(s_translated_text), "%s", result);
  add_to_log(s_dictated_text, s_translated_text);
  text_layer_set_text(s_status_layer, "UP=EN SEL=Mic DN=Log");
  text_layer_set_text(s_main_layer, s_translated_text);
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  cancel_watchdog();
  text_layer_set_text(s_status_layer, "Msg dropped");
}

static void outbox_failed_callback(DictionaryIterator *iter,
                                   AppMessageResult reason, void *context) {
  cancel_watchdog();
  text_layer_set_text(s_status_layer, "Send failed");
}

static void outbox_sent_callback(DictionaryIterator *iter, void *context) {
  text_layer_set_text(s_status_layer, "Waiting for API...");
}

// ── Button handlers ────────────────────────────────────────────────────────
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_dictated_text[0] != '\0') {
    text_layer_set_text(s_status_layer, "Translating EN...");
    text_layer_set_text(s_main_layer, "");
    send_translation_request("en");
  }
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  cancel_watchdog();
  s_dictated_text[0] = '\0';
  s_translated_text[0] = '\0';
  text_layer_set_text(s_main_layer, "");
  start_dictation();
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  open_log();
}

// Long-press DOWN: translate to Chinese (less frequent, kept as secondary action)
static void down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (s_dictated_text[0] != '\0') {
    text_layer_set_text(s_status_layer, "Translating ZH...");
    text_layer_set_text(s_main_layer, "");
    send_translation_request("zh");
  }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP,     up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN,   down_click_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, 700, down_long_click_handler, NULL);
}

// ── Dictation start ────────────────────────────────────────────────────────
static void start_dictation(void) {
  if (!s_dictation) {
    text_layer_set_text(s_status_layer, "No dictation");
    return;
  }
  text_layer_set_text(s_status_layer, "Listening...");
  dictation_session_start(s_dictation);
}

// ── Main window ────────────────────────────────────────────────────────────
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);

  s_status_layer = text_layer_create(GRect(0, 0, bounds.size.w, 30));
  text_layer_set_background_color(s_status_layer, GColorBlack);
  text_layer_set_text_color(s_status_layer, GColorWhite);
  text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
  text_layer_set_text(s_status_layer, "Starting...");
  layer_add_child(root, text_layer_get_layer(s_status_layer));

  s_main_layer = text_layer_create(
      GRect(4, 32, bounds.size.w - 8, bounds.size.h - 34));
  text_layer_set_background_color(s_main_layer, GColorWhite);
  text_layer_set_text_color(s_main_layer, GColorBlack);
  text_layer_set_font(s_main_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_main_layer, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_main_layer, GTextOverflowModeWordWrap);
  text_layer_set_text(s_main_layer, "");
  layer_add_child(root, text_layer_get_layer(s_main_layer));

  s_dictation = dictation_session_create(sizeof(s_dictated_text),
                                         dictation_session_callback, NULL);
  start_dictation();
}

static void window_unload(Window *window) {
  cancel_watchdog();
  dictation_session_destroy(s_dictation);
  text_layer_destroy(s_status_layer);
  text_layer_destroy(s_main_layer);
}

// ── App lifecycle ──────────────────────────────────────────────────────────
static void init(void) {
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);
  app_message_open(MSG_BUFFER_SIZE, MSG_BUFFER_SIZE);

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });
  window_set_click_config_provider(s_window, click_config_provider);
  window_stack_push(s_window, true);
}

static void deinit(void) {
  if (s_log_window) { window_destroy(s_log_window); s_log_window = NULL; }
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
