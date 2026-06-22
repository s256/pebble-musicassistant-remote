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
  GROUP:           13,
  UNGROUP:         14,
  UNGROUP_ALL:     15,
};

// Row flag bits — must match main.c.
var ROW_FLAG_MASTER = 0x01;
var ROW_FLAG_MEMBER = 0x02;

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

// Pebble's bundled Gothic fonts cover ASCII + Latin-1 supplement only.
// Anything outside that range — smart quotes (U+2018/2019), em-dash (U+2014),
// hyphen-minus alternatives (U+2010/2011), ellipsis (U+2026), CJK, etc. —
// renders as a tofu rectangle on-watch.  We map the common offenders to ASCII
// equivalents and fall back to '?' for everything else above Latin-1.
//
// Apply to EVERY string we send to the watch before clamping.
var PUNCT_MAP = {
  '‘': "'", '’': "'", '‚': "'", '‛': "'",  // ‘ ’ ‚ ‛
  '“': '"', '”': '"', '„': '"', '‟': '"',  // “ ” „ ‟
  '‐': '-', '‑': '-', '‒': '-', '–': '-', '—': '-', '―': '-',  // ‐ ‑ ‒ – — ―
  '…': '...', '•': '*',                              // … •
  ' ': ' ', ' ': ' ', ' ': ' ', ' ': ' ',  // various spaces
  '«': '"', '»': '"',                                // « »
  '′': "'", '″': '"',                                // ′ ″
  '​': '',  '‌': '',  '‍': '',  '﻿': '',   // zero-width
};

function pebbleSafe(s) {
  if (!s) return '';
  var out = '';
  for (var i = 0; i < s.length; i++) {
    var c = s.charAt(i);
    var code = s.charCodeAt(i);
    if (code < 0x80) {
      out += c;                       // ASCII passes through unchanged
    } else if (PUNCT_MAP[c] != null) {
      out += PUNCT_MAP[c];            // known punctuation transliteration
    } else if (code < 0x100) {
      out += c;                       // Latin-1 supplement (à, ñ, ü, etc.)
    } else {
      out += '?';                     // everything else — keep the slot but mark unrenderable
    }
  }
  return out;
}

function clamp32(s) {
  s = pebbleSafe(s);
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

// Build a quick lookup from player_id -> player record for group derivation.
function playersById(players) {
  var map = {};
  (players || []).forEach(function (p) { if (p && p.player_id) map[p.player_id] = p; });
  return map;
}

// Derive grouping state for one player.  Returns { syncedTo, groupCount, flags }.
//   - master:  group_members.length > 0 AND NOT itself synced to anyone
//   - member:  synced_to is set
//   - solo:    neither
function deriveGroupState(player) {
  if (!player) return { syncedTo: '', groupCount: 0, flags: 0 };
  var syncedTo = player.synced_to || '';
  var members  = Array.isArray(player.group_members) ? player.group_members : [];
  var isMember = !!syncedTo;
  var isMaster = !isMember && members.length > 0;
  var flags = (isMaster ? ROW_FLAG_MASTER : 0) | (isMember ? ROW_FLAG_MEMBER : 0);
  return {
    syncedTo:   syncedTo,
    groupCount: isMaster ? members.length : 0,
    flags:      flags,
  };
}

function pushPlayerList(queues, players) {
  // Order rules:
  //  1. Each master is immediately followed by its members (so the indented
  //     rail in the watch UI visually connects to the right player).
  //  2. Solo players interleave alphabetically AMONG the masters (treating
  //     a master's display_name as the cluster's sort key).
  //  3. Members inside a cluster also sort alphabetically.
  //
  // Without this, members render under whatever row sits above them in a
  // flat alphabetical sort — Living Room synced to Office could appear
  // indented under Kitchen, etc.
  var pmap = playersById(players);
  var byName = function (a, b) { return a.display_name.localeCompare(b.display_name); };

  var rows = (queues || []).filter(function (q) { return q.available; });

  // Resolve every row up-front so we can group by master.
  var resolved = rows.map(function (q) {
    return { queue: q, grp: deriveGroupState(pmap[q.queue_id]) };
  });

  // Index by queue_id for member lookup.
  var byId = {};
  resolved.forEach(function (r) { byId[r.queue.queue_id] = r; });

  // Walk all rows, pull masters + solos into the top-level cluster key list,
  // and stash members under their master's id.
  var clusters = [];           // [{ master: resolvedRow, members: [resolvedRow,...] }]
  var solos    = [];           // resolvedRow[]
  var memberBuckets = {};      // masterId -> [resolvedRow,...]

  resolved.forEach(function (r) {
    var isMaster = (r.grp.flags & ROW_FLAG_MASTER) !== 0;
    var isMember = (r.grp.flags & ROW_FLAG_MEMBER) !== 0;
    if (isMaster) {
      clusters.push({ master: r, members: [] });
      memberBuckets[r.queue.queue_id] = memberBuckets[r.queue.queue_id] || [];
    } else if (isMember) {
      var key = r.grp.syncedTo;
      memberBuckets[key] = memberBuckets[key] || [];
      memberBuckets[key].push(r);
    } else {
      solos.push(r);
    }
  });

  // Attach members to their clusters.  If a "member" reports a synced_to that
  // isn't in our visible list (rare — server hiccup), treat it as solo so it
  // still renders.
  clusters.forEach(function (c) {
    var bucket = memberBuckets[c.master.queue.queue_id] || [];
    c.members = bucket.slice().sort(function (a, b) {
      return a.queue.display_name.localeCompare(b.queue.display_name);
    });
  });
  Object.keys(memberBuckets).forEach(function (k) {
    if (!byId[k] || !(byId[k].grp.flags & ROW_FLAG_MASTER)) {
      // orphaned member — fall back to solo
      memberBuckets[k].forEach(function (r) { solos.push(r); });
    }
  });

  // Interleave clusters + solos by name.
  var top = clusters.map(function (c) { return { kind: 'cluster', name: c.master.queue.display_name, cluster: c }; })
        .concat(solos.map(function (r) { return { kind: 'solo', name: r.queue.display_name, row: r }; }));
  top.sort(function (a, b) { return a.name.localeCompare(b.name); });

  // Flatten back to a row sequence.
  var ordered = [];
  top.forEach(function (entry) {
    if (entry.kind === 'cluster') {
      ordered.push(entry.cluster.master);
      entry.cluster.members.forEach(function (m) { ordered.push(m); });
    } else {
      ordered.push(entry.row);
    }
  });
  ordered = ordered.slice(0, 16);

  enqueue({ PLAYERS_BEGIN: 1 });
  ordered.forEach(function (r, i) {
    enqueue({
      PLAYER_ROW_INDEX:       i,
      PLAYER_ROW_ID:          r.queue.queue_id,
      PLAYER_ROW_NAME:        clamp32(r.queue.display_name),
      PLAYER_ROW_STATE:       maStateToEnum(r.queue.state),
      PLAYER_ROW_SYNCED_TO:   clamp32(r.grp.syncedTo),
      PLAYER_ROW_GROUP_COUNT: r.grp.groupCount,
      PLAYER_ROW_FLAGS:       r.grp.flags,
    });
  });
  enqueue({ PLAYERS_END: 1 });
}

function pushError(msg) {
  console.log('[ma] error:', msg);
  enqueue({ ST_ERROR: clamp32(msg) });
}

function clampStr(s, n) {
  s = pebbleSafe(s);
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

// Pull a fresh snapshot of queues + players and emit a fresh player-list to
// the watch.  Called on CMD_REQUEST_PLAYERS (when the user opens the list)
// and after every grouping change so the visible rows reflect the new
// topology without the user having to back out and re-enter the list.
function refreshPlayersList() {
  maCall('player_queues/all', {}, function (err, qres) {
    if (err) { pushError(err.message); return; }
    var queues = (qres && qres.result) || qres || [];
    maCall('players/all', {}, function (perr, pres) {
      if (perr) { pushError(perr.message); return; }
      var players = (pres && pres.result) || pres || [];
      pushPlayerList(queues, players);
    });
  });
}

// Called after any group / ungroup / ungroup_many command settles.  MA
// applies the membership change asynchronously, so we give it a short beat
// before refreshing both the now-playing snapshot and the player list.
function afterGroupingChange() {
  setTimeout(function () {
    refreshPlayersList();      // updates s_players[] on the watch in-place
    quickRepoll();             // refreshes now-playing in case master changed
  }, 350);
}

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
      refreshPlayersList();
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

    case CMD.GROUP: {
      var src = payload.ARG_PLAYER_ID;
      var tgt = payload.ARG_TARGET_PLAYER_ID;
      if (!src || !tgt) { pushError('Group: missing player ids'); break; }
      maCall('players/cmd/group',
        { player_id: src, target_player: tgt },
        function (err) {
          if (err) pushError(err.message);
          // MA needs a beat to apply the change before the next snapshot
          // reflects it; the player list on the watch has just popped to
          // the front so refresh both the now-playing AND the list.
          afterGroupingChange();
        });
      break;
    }

    case CMD.UNGROUP: {
      var pid = payload.ARG_PLAYER_ID;
      if (!pid) { pushError('Ungroup: missing player id'); break; }
      maCall('players/cmd/ungroup',
        { player_id: pid },
        function (err) {
          if (err) pushError(err.message);
          afterGroupingChange();
        });
      break;
    }

    case CMD.UNGROUP_ALL: {
      var idsCsv = payload.ARG_PLAYER_IDS || '';
      var ids = idsCsv.split(',').filter(function (s) { return s.length > 0; });
      if (ids.length === 0) { pushError('Ungroup all: empty id list'); break; }
      maCall('players/cmd/ungroup_many',
        { player_ids: ids },
        function (err) {
          if (err) pushError(err.message);
          afterGroupingChange();
        });
      break;
    }

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
