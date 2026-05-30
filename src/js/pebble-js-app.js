// PebbleKit JS — Speech Translator
var KEY_TEXT   = 0;
var KEY_LANG   = 1;
var KEY_RESULT = 2;

function sendToWatch(text) {
  var MAX = 490;
  var safe = (text.length <= MAX) ? text : text.substring(0, MAX - 3) + '...';
  console.log('SpeechTrans: sendToWatch: ' + safe);
  Pebble.sendAppMessage(
    { 'KEY_RESULT': safe },
    function() { console.log('SpeechTrans: ACK OK'); },
    function(e) { console.log('SpeechTrans: ACK FAIL: ' + JSON.stringify(e)); }
  );
}

function translate(text) {
  var url = 'https://translate.googleapis.com/translate_a/single' +
            '?client=gtx&sl=ja&tl=en&dt=t&q=' + encodeURIComponent(text);
  console.log('SpeechTrans: GET ' + url);

  try {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, false);  // synchronous
    xhr.send(null);
    console.log('SpeechTrans: status=' + xhr.status + ' len=' + xhr.responseText.length);

    if (xhr.status === 200) {
      var data = JSON.parse(xhr.responseText);
      var out = '';
      for (var i = 0; i < data[0].length; i++) {
        if (data[0][i] && data[0][i][0]) { out += data[0][i][0]; }
      }
      return out || '(empty result)';
    }
    return 'HTTP error: ' + xhr.status;
  } catch (e) {
    console.log('SpeechTrans: exception: ' + e);
    return 'Error: ' + e;
  }
}

Pebble.addEventListener('appmessage', function(e) {
  console.log('SpeechTrans: appmessage: ' + JSON.stringify(e.payload));

  var text = e.payload['KEY_TEXT'] !== undefined ? e.payload['KEY_TEXT'] : e.payload[KEY_TEXT];
  var lang = e.payload['KEY_LANG'] !== undefined ? e.payload['KEY_LANG'] : e.payload[KEY_LANG];

  console.log('SpeechTrans: text="' + text + '" lang="' + lang + '"');

  if (!text) {
    sendToWatch('Error: no text');
    return;
  }

  var result = translate(text);
  console.log('SpeechTrans: result="' + result + '"');
  sendToWatch(result);
});

// Fires when JS is loaded and connected to the watch.
// Sends a test message so the watch can display "JS OK" — confirms the full
// JS-to-watch communication path is working before the user tries to translate.
Pebble.addEventListener('ready', function() {
  console.log('SpeechTrans JS: ready');
  sendToWatch('JS OK');
});
