// PebbleKit JS — Speech Translator
var KEY_TEXT   = 0;
var KEY_LANG   = 1;
var KEY_RESULT = 2;

function truncate(str) {
  var max = 490;
  if (str.length <= max) { return str; }
  return str.substring(0, max - 3) + '...';
}

function sendToWatch(text) {
  console.log('SpeechTrans: sending: ' + text);
  Pebble.sendAppMessage(
    { 'KEY_RESULT': truncate(text) },
    function() { console.log('SpeechTrans: sent OK'); },
    function(e) { console.log('SpeechTrans: sent FAIL: ' + JSON.stringify(e)); }
  );
}

// Synchronous XHR — avoids async callback issues in older PebbleKit JS runtimes.
// Google Translate unofficial endpoint (no API key, widely used by Pebble apps).
function translate(text, targetLang) {
  var langCode = (targetLang === 'zh') ? 'zh-CN' : 'en';
  var url = 'https://translate.googleapis.com/translate_a/single' +
            '?client=gtx&sl=ja&tl=' + langCode +
            '&dt=t&q=' + encodeURIComponent(text);
  console.log('SpeechTrans: GET ' + url);

  try {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, false);  // false = synchronous: blocks until response
    xhr.send();
    console.log('SpeechTrans: http=' + xhr.status + ' bodyLen=' + xhr.responseText.length);

    if (xhr.status === 200) {
      // Response shape: [[["translated","original",...], ...], null, "ja", ...]
      var data = JSON.parse(xhr.responseText);
      var segments = data[0];
      var out = '';
      for (var i = 0; i < segments.length; i++) {
        if (segments[i] && segments[i][0]) { out += segments[i][0]; }
      }
      return out || '(empty)';
    } else {
      return 'Error: HTTP ' + xhr.status;
    }
  } catch (e) {
    console.log('SpeechTrans: exception: ' + e);
    return 'Error: ' + e;
  }
}

Pebble.addEventListener('appmessage', function(e) {
  var payload = e.payload;
  console.log('SpeechTrans: appmessage ' + JSON.stringify(payload));

  var text = payload[KEY_TEXT];
  var lang = payload[KEY_LANG];

  if (!text || !lang) {
    console.log('SpeechTrans: bad payload');
    return;
  }

  console.log('SpeechTrans: "' + text + '" -> ' + lang);

  var result = translate(text, lang);
  console.log('SpeechTrans: result: ' + result);

  if (lang === 'en') {
    sendToWatch(result);
  } else {
    Pebble.showSimpleNotificationOnPebble('中国語 (ZH)', result);
    sendToWatch('[ZH 通知に送信済]');
  }
});

Pebble.addEventListener('ready', function() {
  console.log('SpeechTrans JS: ready');
});
