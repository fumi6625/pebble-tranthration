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

// Truncate to fit within AppMessage 512-byte buffer.
function truncate(str) {
  var max = 490;
  if (str.length <= max) { return str; }
  return str.substring(0, max - 3) + '...';
}

function sendToWatch(text) {
  console.log('SpeechTrans: sending to watch: ' + text);
  Pebble.sendAppMessage(
    { 'KEY_RESULT': truncate(text) },
    function() { console.log('SpeechTrans: watch send OK'); },
    function(e) { console.log('SpeechTrans: watch send FAIL: ' + JSON.stringify(e)); }
  );
}

function fetchTranslation(text, targetLang, onSuccess, onError) {
  var url = buildUrl(text, 'ja', targetLang);
  console.log('SpeechTrans: XHR GET ' + url);

  var xhr = new XMLHttpRequest();
  // Use onreadystatechange for compatibility with all PebbleKit JS versions
  xhr.onreadystatechange = function() {
    if (xhr.readyState !== 4) { return; }
    console.log('SpeechTrans: XHR status=' + xhr.status + ' len=' + xhr.responseText.length);
    if (xhr.status === 200) {
      try {
        var json = JSON.parse(xhr.responseText);
        if (json.responseStatus === 200 && json.responseData) {
          onSuccess(json.responseData.translatedText);
        } else {
          onError('API err: ' + json.responseStatus);
        }
      } catch (e) {
        onError('JSON parse error');
      }
    } else {
      onError('HTTP ' + xhr.status);
    }
  };
  xhr.open('GET', url, true);
  xhr.send();
}

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  console.log('SpeechTrans: appmessage payload=' + JSON.stringify(payload));

  var text = payload[KEY_TEXT];
  var lang = payload[KEY_LANG];

  if (!text || !lang) {
    console.log('SpeechTrans: missing text or lang');
    return;
  }

  console.log('SpeechTrans: translate "' + text + '" -> ' + lang);

  fetchTranslation(text, lang,
    function(translated) {
      console.log('SpeechTrans: translated: ' + translated);
      if (lang === 'en') {
        sendToWatch(translated);
      } else if (lang === 'zh') {
        Pebble.showSimpleNotificationOnPebble('中国語 (ZH)', translated);
        console.log('SpeechTrans: ZH notification sent');
        // Also confirm to watch that ZH is done
        sendToWatch('[ZH sent to notif]');
      }
    },
    function(errMsg) {
      console.log('SpeechTrans: error: ' + errMsg);
      sendToWatch('Error: ' + errMsg);
    }
  );
});

Pebble.addEventListener('ready', function() {
  console.log('SpeechTrans JS: ready');
});
