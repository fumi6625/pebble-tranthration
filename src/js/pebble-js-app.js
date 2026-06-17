// PebbleKit JS — Speech Translator (JP↔EN)
var KEY_TEXT   = 0;
var KEY_LANG   = 1;
var KEY_RESULT = 2;

function sendToWatch(text) {
  var MAX = 490;
  var safe = (text.length <= MAX) ? text : text.substring(0, MAX - 3) + '...';
  console.log('SpeechTrans: send: ' + safe);
  Pebble.sendAppMessage(
    { 'KEY_RESULT': safe },
    function() { console.log('SpeechTrans: ack ok'); },
    function(e) { console.log('SpeechTrans: ack fail ' + JSON.stringify(e)); }
  );
}

// langpair examples: "ja|en" (JP→EN) or "en|ja" (EN→JP)
function translate(text, langpair, onResult) {
  var parts = (langpair || 'ja|en').split('|');
  var sl = parts[0] || 'ja';
  var tl = parts[1] || 'en';
  var url = 'https://translate.googleapis.com/translate_a/single' +
            '?client=gtx&sl=' + sl + '&tl=' + tl +
            '&dt=t&q=' + encodeURIComponent(text);
  console.log('SpeechTrans: GET ' + url);

  var xhr = new XMLHttpRequest();
  xhr.open('GET', url, true);
  xhr.onload = function(e) {
    console.log('SpeechTrans: readyState=' + this.readyState + ' status=' + this.status);
    if (this.readyState == 4) {
      if (this.status == 200) {
        try {
          var data = JSON.parse(this.responseText);
          var out = '';
          for (var i = 0; i < data[0].length; i++) {
            if (data[0][i] && data[0][i][0]) { out += data[0][i][0]; }
          }
          onResult(out || '(empty)');
        } catch (err) {
          onResult('Parse err: ' + err);
        }
      } else {
        onResult('HTTP ' + this.status);
      }
    }
  };
  xhr.onerror = function() {
    console.log('SpeechTrans: onerror');
    onResult('Net error');
  };
  xhr.send(null);
}

Pebble.addEventListener('appmessage', function(e) {
  console.log('SpeechTrans: appmessage ' + JSON.stringify(e.payload));

  var text = (e.payload['KEY_TEXT'] !== undefined) ? e.payload['KEY_TEXT'] : e.payload[KEY_TEXT];
  var langpair = (e.payload['KEY_LANG'] !== undefined)
    ? e.payload['KEY_LANG']
    : (e.payload[KEY_LANG] || 'ja|en');

  console.log('SpeechTrans: text="' + text + '" langpair="' + langpair + '"');

  if (!text) {
    sendToWatch('Error: no text');
    return;
  }

  translate(text, langpair, function(result) {
    console.log('SpeechTrans: result="' + result + '"');
    sendToWatch(result);
  });
});

Pebble.addEventListener('ready', function() {
  console.log('SpeechTrans JS: ready');
  sendToWatch('JS OK');
});
