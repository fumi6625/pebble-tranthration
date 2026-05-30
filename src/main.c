#include <pebble.h>

#define KEY_TEXT   0
#define KEY_LANG   1
#define KEY_RESULT 2

#define MSG_BUFFER_SIZE 512
#define TRANSLATE_TIMEOUT_MS 20000

static Window           *s_window;
static TextLayer        *s_status_layer;
static TextLayer        *s_main_layer;
static DictationSession *s_dictation;
static AppTimer         *s_watchdog = NULL;

static char s_dictated_text[512];
static char s_translated_text[512];

static void start_dictation(void);

// Cancel watchdog timer if running
static void cancel_watchdog(void) {
  if (s_watchdog) {
    app_timer_cancel(s_watchdog);
    s_watchdog = NULL;
  }
}

static void watchdog_callback(void *context) {
  s_watchdog = NULL;
  text_layer_set_text(s_status_layer, "Timeout. SEL=Retry");
}

static void dictation_session_callback(DictationSession *session,
                                       DictationSessionStatus status,
                                       char *transcription,
                                       void *context) {
  if (status == DictationSessionStatusSuccess) {
    snprintf(s_dictated_text, sizeof(s_dictated_text), "%s", transcription);
    text_layer_set_text(s_status_layer, "UP=EN  DN=ZH  SEL=Redo");
    text_layer_set_text(s_main_layer, s_dictated_text);
  } else {
    static char err_buf[64];
    snprintf(err_buf, sizeof(err_buf), "Error: %d\nSELECT to retry", (int)status);
    text_layer_set_text(s_status_layer, "Dictation failed");
    text_layer_set_text(s_main_layer, err_buf);
  }
}

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
  // Start watchdog: if no response in 20s, show timeout
  s_watchdog = app_timer_register(TRANSLATE_TIMEOUT_MS, watchdog_callback, NULL);
}

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
  if (s_dictated_text[0] != '\0') {
    text_layer_set_text(s_status_layer, "Sending ZH...");
    text_layer_set_text(s_main_layer, "");
    send_translation_request("zh");
  }
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP,     up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN,   down_click_handler);
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
  cancel_watchdog();
  Tuple *result_tuple = dict_find(iter, KEY_RESULT);
  if (result_tuple) {
    snprintf(s_translated_text, sizeof(s_translated_text),
             "%s", result_tuple->value->cstring);
    text_layer_set_text(s_status_layer, "English:");
    text_layer_set_text(s_main_layer, s_translated_text);
  }
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
  // Message reached phone: update status to show we are waiting for API response
  text_layer_set_text(s_status_layer, "Waiting for API...");
}

static void start_dictation(void) {
  if (!s_dictation) {
    text_layer_set_text(s_status_layer, "No dictation");
    return;
  }
  text_layer_set_text(s_status_layer, "Listening...");
  dictation_session_start(s_dictation);
}

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
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
