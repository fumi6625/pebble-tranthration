// PebbleKit JS — Speech Translator
// Keys must match appinfo.json appKeys and src/main.c defines.
var KEY_TEXT   = 0;
var KEY_LANG   = 1;
var KEY_RESULT = 2;

var MYMEMORY_BASE = 'https://api.mymemory.translated.net/get';

function buildUrl(text, sourceLang, targetLang) {
  return MYMEMORY_BASE +
         '?q='        + encodeURIComponent(text) +
         '&langpair=' + encodeURIComponent(sourceLang + '|' + targetLang);
}

// Truncate to fit within AppMessage 512-byte buffer (allow header overhead).
function truncate(str) {
  var max = 490;
  if (str.length <= max) { return str; }
  return str.substring(0, max - 3) + '...';
}

function fetchTranslation(text, targetLang, onSuccess, onError) {
  var xhr = new XMLHttpRequest();
  xhr.open('GET', buildUrl(text, 'ja', targetLang), true);
  xhr.timeout = 10000;

  xhr.onload = function() {
    if (xhr.status === 200) {
      try {
        var json = JSON.parse(xhr.responseText);
        if (json.responseStatus === 200 && json.responseData) {
          onSuccess(json.responseData.translatedText);
        } else {
          onError('API error: ' + json.responseStatus);
        }
      } catch (e) {
        onError('JSON parse error');
      }
    } else {
      onError('HTTP ' + xhr.status);
    }
  };
  xhr.onerror   = function() { onError('Network error'); };
  xhr.ontimeout = function() { onError('Timeout');       };
  xhr.send();
}

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  var text = payload[KEY_TEXT];
  var lang = payload[KEY_LANG];

  if (!text || !lang) {
    console.log('SpeechTrans: missing text or lang');
    return;
  }

  console.log('SpeechTrans: "' + text + '" -> ' + lang);

  fetchTranslation(text, lang,
    function(translated) {
      console.log('SpeechTrans: result: ' + translated);

      if (lang === 'en') {
        Pebble.sendAppMessage(
          { 2: truncate(translated) },
          function() { console.log('SpeechTrans: EN sent OK'); },
          function(err) { console.log('SpeechTrans: EN send fail: ' + JSON.stringify(err)); }
        );
      } else if (lang === 'zh') {
        Pebble.showSimpleNotificationOnPebble('中国語 (ZH)', translated);
        console.log('SpeechTrans: ZH notification sent');
      }
    },
    function(errMsg) {
      console.log('SpeechTrans: fetch error: ' + errMsg);
      if (lang === 'en') {
        Pebble.sendAppMessage(
          { 2: 'Error: ' + errMsg },
          function() {},
          function() {}
        );
      } else {
        Pebble.showSimpleNotificationOnPebble('Translate Error', errMsg);
      }
    }
  );
});

Pebble.addEventListener('ready', function() {
  console.log('SpeechTrans JS: ready');
});
