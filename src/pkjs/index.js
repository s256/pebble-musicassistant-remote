/*
 * pebble-musicassistant-remote — PebbleKit JS bridge
 *
 * Runs on the phone inside the Pebble mobile app.  Talks REST to a Music
 * Assistant server, talks AppMessage to the watch.
 *
 * Settings (host / username / password) are entered on the GitHub Pages
 * configuration page; we persist them in localStorage and re-auth as needed.
 */

/* eslint-disable no-var, prefer-const, prefer-template, no-console */

// ─── Constants ─────────────────────────────────────────────────────────────

var CMD = {
  REQUEST_STATE:   1,
  REQUEST_PLAYERS: 2,
  SELECT_PLAYER:   3,
  PLAY_PAUSE:      4,
  NEXT:            5,
  PREVIOUS:        6,
  VOLUME_UP:       7,
  VOLUME_DOWN:     8,
  MUTE_TOGGLE:     9,
  SHUFFLE_TOGGLE:  10,
  REPEAT_CYCLE:    11,
  QUICK_PLAY:      12,
};

var MAX_QUICK_SLOTS = 10;
var MAX_QUICK_LABEL = 40;
var MAX_QUICK_URI   = 120;

var STATE = { UNKNOWN: 0, IDLE: 1, PAUSED: 2, PLAYING: 3 };
var REPEAT = { off: 0, one: 1, all: 2 };

var POLL_MS_PLAYING = 2000;
var POLL_MS_IDLE    = 5000;
var POLL_MS_AFTER_ACTION = 250;
var VOLUME_STEP = 5;

var CONFIG_URL = 'https://s256.github.io/pebble-musicassistant-remote/';

// ─── Settings ─────────────────────────────────────────────────────────────

function loadSettings() {
  try {
    var raw = localStorage.getItem('ma.settings');
    if (!raw) return null;
    return JSON.parse(raw);
  } catch (e) { return null; }
}

function saveSettings(s) {
  localStorage.setItem('ma.settings', JSON.stringify(s));
}

function loadToken() {
  return localStorage.getItem('ma.token') || null;
}
function saveToken(t) {
  if (t) localStorage.setItem('ma.token', t);
  else   localStorage.removeItem('ma.token');
}

// ─── Runtime state ────────────────────────────────────────────────────────

var settings        = loadSettings();
var token           = loadToken();
var pollTimer       = null;
var overridePlayerId = null;
var lastSentSig     = '';
var sendQueue       = [];
var sending         = false;

// ─── HTTP helper ──────────────────────────────────────────────────────────

function http(method, url, body, headers, cb) {
  var xhr = new XMLHttpRequest();
  xhr.open(method, url, true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  if (headers) {
    Object.keys(headers).forEach(function (k) { xhr.setRequestHeader(k, headers[k]); });
  }
  xhr.timeout = 10000;
  xhr.onload = function () {
    var parsed = null;
    try { parsed = xhr.responseText ? JSON.parse(xhr.responseText) : null; } catch (e) {}
    cb(null, xhr.status, parsed);
  };
  xhr.onerror   = function () { cb(new Error('network'), 0, null); };
  xhr.ontimeout = function () { cb(new Error('timeout'), 0, null); };
  xhr.send(body ? JSON.stringify(body) : null);
}

// ─── Auth ─────────────────────────────────────────────────────────────────

function login(cb) {
  if (!settings || !settings.host || !settings.username || !settings.password) {
    return cb(new Error('Settings not configured'));
  }
  console.log('[ma] login as', settings.username);
  http('POST', settings.host + '/auth/login', {
    provider_id: 'builtin',
    credentials: { username: settings.username, password: settings.password },
  }, null, function (err, status, body) {
    if (err) return cb(err);
    if (status !== 200 || !body || !body.token) {
      return cb(new Error('Auth failed (' + status + ')'));
    }
    token = body.token;
    saveToken(token);
    console.log('[ma] login ok');
    cb(null);
  });
}

// ─── MA command envelope ──────────────────────────────────────────────────

var msgId = 1;
function maCall(command, args, cb) {
  if (!settings || !settings.host) return cb(new Error('Settings not configured'));
  var doCall = function () {
    http('POST', settings.host + '/api',
      { message_id: String(msgId++), command: command, args: args || {} },
      { Authorization: 'Bearer ' + token },
      function (err, status, body) {
        if (err) return cb(err);
        if (status === 401) {
          console.log('[ma] 401, re-auth');
          token = null; saveToken(null);
          return login(function (lerr) {
            if (lerr) return cb(lerr);
            doCall();
          });
        }
        if (status < 200 || status >= 300) return cb(new Error('HTTP ' + status));
        cb(null, body);
      });
  };
  if (!token) login(function (err) { if (err) return cb(err); doCall(); });
  else doCall();
}

// ─── Player selection ─────────────────────────────────────────────────────

// Pick the active player to render on the watch.  Override wins; else
// playing > paused > anything available, alphabetical tiebreak by display_name.
function pickActiveQueue(queues) {
  if (!queues || queues.length === 0) return null;
  var avail = queues.filter(function (q) { return q.available && q.active; });
  if (avail.length === 0) avail = queues.filter(function (q) { return q.available; });
  if (avail.length === 0) return null;

  if (overridePlayerId) {
    var ov = avail.find(function (q) { return q.queue_id === overridePlayerId; });
    if (ov) return ov;
    // Override stale; drop it.
    overridePlayerId = null;
  }

  var byName = function (a, b) { return a.display_name.localeCompare(b.display_name); };
  var playing = avail.filter(function (q) { return q.state === 'playing'; }).sort(byName);
  if (playing.length) return playing[0];
  var paused  = avail.filter(function (q) { return q.state === 'paused' ; }).sort(byName);
  if (paused.length)  return paused[0];
  return avail.slice().sort(byName)[0];
}

function maStateToEnum(s) {
  return s === 'playing' ? STATE.PLAYING :
         s === 'paused'  ? STATE.PAUSED  :
         s === 'idle'    ? STATE.IDLE    : STATE.UNKNOWN;
}

// ─── AppMessage send queue ────────────────────────────────────────────────

function enqueue(msg) {
  sendQueue.push(msg);
  pump();
}

function pump() {
  if (sending || sendQueue.length === 0) return;
  sending = true;
  var msg = sendQueue.shift();
  Pebble.sendAppMessage(msg,
    function () { sending = false; pump(); },
    function (e) {
      console.log('[ma] sendAppMessage failed:', JSON.stringify(e));
      sending = false; pump();
    });
}

function clamp32(s) {
  if (!s) return '';
  if (s.length <= 60) return s;
  return s.substring(0, 60);
}

function pushNowPlaying(queue, players) {
  if (!queue) {
    enqueue({
      ST_OK: 1,
      ST_PLAYER_NAME: 'No player',
      ST_TITLE: 'Nothing playing',
      ST_ARTIST: '',
      ST_ALBUM: '',
      ST_STATE: STATE.IDLE,
      ST_VOLUME: 0, ST_MUTED: 0,
      ST_SHUFFLE: 0, ST_REPEAT: REPEAT.off,
      ST_ELAPSED: 0, ST_DURATION: 0,
    });
    return;
  }
  var item = queue.current_item || {};
  var media = item.media_item || {};
  var title = item.name || media.name || 'Unknown';
  var artist = '';
  if (media.artists && media.artists.length) artist = media.artists[0].name || '';
  else if (item.artist) artist = item.artist;
  var album = (media.album && media.album.name) || item.album || '';

  // The player record gives us volume / mute.
  var player = (players || []).find(function (p) { return p.player_id === queue.queue_id; });
  var volume = player && player.volume_level != null ? player.volume_level : 0;
  var muted  = player && player.volume_muted ? 1 : 0;

  // Signature for dedup.  elapsed_s is deliberately omitted — the watch
  // interpolates it locally between pushes, so sending it every poll would
  // defeat the dedup intent.  album IS included since it can change without
  // title/artist changing on compilation albums.
  var sig = [queue.queue_id, queue.state, title, artist, album, volume, muted,
             queue.shuffle_enabled ? 1 : 0, queue.repeat_mode].join('|');
  if (sig === lastSentSig) return;
  lastSentSig = sig;

  enqueue({
    ST_OK: 1,
    ST_PLAYER_NAME: clamp32(queue.display_name),
    ST_TITLE:       clamp32(title),
    ST_ARTIST:      clamp32(artist),
    ST_ALBUM:       clamp32(album),
    ST_STATE:       maStateToEnum(queue.state),
    ST_VOLUME:      volume,
    ST_MUTED:       muted,
    ST_SHUFFLE:     queue.shuffle_enabled ? 1 : 0,
    ST_REPEAT:      REPEAT[queue.repeat_mode] != null ? REPEAT[queue.repeat_mode] : REPEAT.off,
    ST_ELAPSED:     Math.floor(queue.elapsed_time || 0),
    ST_DURATION:    Math.floor((item && item.duration) || (media && media.duration) || 0),
  });
}

function pushPlayerList(queues) {
  // Sort alphabetically by display_name — same rule as the auto-picker.
  var list = (queues || []).slice()
    .filter(function (q) { return q.available; })
    .sort(function (a, b) { return a.display_name.localeCompare(b.display_name); })
    .slice(0, 16);

  enqueue({ PLAYERS_BEGIN: 1 });
  list.forEach(function (q, i) {
    enqueue({
      PLAYER_ROW_INDEX: i,
      PLAYER_ROW_ID:    q.queue_id,
      PLAYER_ROW_NAME:  clamp32(q.display_name),
      PLAYER_ROW_STATE: maStateToEnum(q.state),
    });
  });
  enqueue({ PLAYERS_END: 1 });
}

function pushError(msg) {
  console.log('[ma] error:', msg);
  enqueue({ ST_ERROR: clamp32(msg) });
}

function clampStr(s, n) {
  if (!s) return '';
  if (s.length <= n) return s;
  return s.substring(0, n);
}

function getSlots() {
  if (settings && Array.isArray(settings.slots)) return settings.slots;
  return [];
}

function pushQuickSlots() {
  var slots = getSlots().slice(0, MAX_QUICK_SLOTS);
  enqueue({ QUICK_BEGIN: 1 });
  slots.forEach(function (s, i) {
    if (!s || !s.uri) return;
    enqueue({
      QUICK_ROW_INDEX: i,
      QUICK_ROW_LABEL: clampStr(s.label || s.uri, MAX_QUICK_LABEL),
      QUICK_ROW_URI:   clampStr(s.uri, MAX_QUICK_URI),
    });
  });
  enqueue({ QUICK_END: 1 });
}

// ─── Poll ─────────────────────────────────────────────────────────────────

function pollOnce(cb) {
  maCall('player_queues/all', {}, function (err, qres) {
    if (err) {
      pushError(err.message || String(err));
      return cb && cb(err);
    }
    var queues = (qres && qres.result) || qres || [];
    maCall('players/all', {}, function (err2, pres) {
      if (err2) {
        pushError(err2.message || String(err2));
        return cb && cb(err2);
      }
      var players = (pres && pres.result) || pres || [];
      var active = pickActiveQueue(queues);
      pushNowPlaying(active, players);
      cb && cb(null, queues, players, active);
    });
  });
}

function schedulePoll(ms) {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = setTimeout(function () {
    pollOnce(function (err, queues, players, active) {
      var next = (active && active.state === 'playing') ? POLL_MS_PLAYING : POLL_MS_IDLE;
      schedulePoll(next);
    });
  }, ms);
}

function quickRepoll() { schedulePoll(POLL_MS_AFTER_ACTION); }

// ─── Active-player resolution for outbound commands ───────────────────────

function withActive(cb) {
  maCall('player_queues/all', {}, function (err, qres) {
    if (err) { pushError(err.message); return; }
    var queues = (qres && qres.result) || qres || [];
    var active = pickActiveQueue(queues);
    if (!active) { pushError('No active player'); return; }
    // We also fetch /players to get current volume for ± commands.
    maCall('players/all', {}, function (err2, pres) {
      if (err2) { pushError(err2.message); return; }
      var players = (pres && pres.result) || pres || [];
      var player  = players.find(function (p) { return p.player_id === active.queue_id; });
      cb(active, player);
    });
  });
}

// ─── Watch → MA dispatch ──────────────────────────────────────────────────

function handleWatchCmd(cmd, payload) {
  switch (cmd) {
    case CMD.REQUEST_STATE:
      pollOnce(); break;

    case CMD.REQUEST_PLAYERS:
      maCall('player_queues/all', {}, function (err, qres) {
        if (err) { pushError(err.message); return; }
        var queues = (qres && qres.result) || qres || [];
        pushPlayerList(queues);
      });
      break;

    case CMD.SELECT_PLAYER:
      overridePlayerId = payload.ARG_PLAYER_ID || null;
      console.log('[ma] override player ->', overridePlayerId);
      quickRepoll();
      break;

    case CMD.PLAY_PAUSE:
      withActive(function (active) {
        maCall('player_queues/play_pause', { queue_id: active.queue_id }, function (err) {
          if (err) pushError(err.message);
          quickRepoll();
        });
      });
      break;

    case CMD.NEXT:
      withActive(function (active) {
        maCall('player_queues/next', { queue_id: active.queue_id }, function (err) {
          if (err) pushError(err.message);
          quickRepoll();
        });
      });
      break;

    case CMD.PREVIOUS:
      withActive(function (active) {
        maCall('player_queues/previous', { queue_id: active.queue_id }, function (err) {
          if (err) pushError(err.message);
          quickRepoll();
        });
      });
      break;

    case CMD.VOLUME_UP:
    case CMD.VOLUME_DOWN:
      withActive(function (active, player) {
        var current = (player && player.volume_level != null) ? player.volume_level : 50;
        var target = current + (cmd === CMD.VOLUME_UP ? VOLUME_STEP : -VOLUME_STEP);
        if (target < 0) target = 0;
        if (target > 100) target = 100;
        maCall('players/cmd/volume_set',
          { player_id: active.queue_id, volume_level: target },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.MUTE_TOGGLE:
      withActive(function (active, player) {
        var muted = !(player && player.volume_muted);
        maCall('players/cmd/volume_mute',
          { player_id: active.queue_id, muted: muted },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.SHUFFLE_TOGGLE:
      withActive(function (active) {
        // active here is a queue, so shuffle_enabled is on it.
        var next = !active.shuffle_enabled;
        maCall('player_queues/shuffle',
          { queue_id: active.queue_id, shuffle_enabled: next },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.REPEAT_CYCLE:
      withActive(function (active) {
        var cur = active.repeat_mode || 'off';
        var nextMode = cur === 'off' ? 'all' : cur === 'all' ? 'one' : 'off';
        maCall('player_queues/repeat',
          { queue_id: active.queue_id, repeat_mode: nextMode },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.QUICK_PLAY:
      withActive(function (active) {
        var uri = payload.ARG_STR;
        if (!uri) { pushError('Empty quick-play URI'); return; }
        maCall('player_queues/play_media',
          { queue_id: active.queue_id, media: uri, option: 'replace' },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    default:
      console.log('[ma] unknown cmd', cmd);
  }
}

// ─── Pebble glue ──────────────────────────────────────────────────────────

Pebble.addEventListener('ready', function () {
  console.log('[ma] PebbleKit JS ready');
  settings = loadSettings();
  // Always push the cached slot list so Quick Play works even before the
  // first poll (and even when the watch's persist is empty).
  pushQuickSlots();
  if (!settings || !settings.host) {
    pushError('Configure host in settings');
    return;
  }
  schedulePoll(POLL_MS_AFTER_ACTION);
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  if (p.CMD != null) handleWatchCmd(p.CMD, p);
});

Pebble.addEventListener('showConfiguration', function () {
  var cur = loadSettings() || {};
  var slotsJson = JSON.stringify(Array.isArray(cur.slots) ? cur.slots : []);
  var qs = 'host='     + encodeURIComponent(cur.host || '')
         + '&user='    + encodeURIComponent(cur.username || '')
         + '&hasPass=' + (cur.password ? '1' : '0')
         + '&slots='   + encodeURIComponent(slotsJson);
  Pebble.openURL(CONFIG_URL + '?' + qs);
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var next;
  try { next = JSON.parse(decodeURIComponent(e.response)); }
  catch (err) { console.log('[ma] settings parse error', err); return; }

  // Merge — if user left password blank, keep the existing one. Slots are
  // overwritten when the page sends them (the page sends the complete list).
  var prev = loadSettings() || {};
  var slots = Array.isArray(next.slots) ? next.slots.slice(0, MAX_QUICK_SLOTS) : (prev.slots || []);
  slots = slots
    .filter(function (s) { return s && s.uri; })
    .map(function (s) {
      return {
        label: clampStr(String(s.label || s.uri), MAX_QUICK_LABEL),
        uri:   clampStr(String(s.uri), MAX_QUICK_URI),
      };
    });
  var merged = {
    host:     (next.host || '').replace(/\/+$/, ''),
    username:  next.username || '',
    password:  next.password || prev.password || '',
    slots:     slots,
  };
  saveSettings(merged);
  settings = merged;
  saveToken(null); token = null;
  pushQuickSlots();
  console.log('[ma] settings saved (' + slots.length + ' quick slots)');
  schedulePoll(POLL_MS_AFTER_ACTION);
});
