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
  GROUP_VOLUME_UP:    16,
  GROUP_VOLUME_DOWN:  17,
  GROUP_MUTE_TOGGLE:  18,
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
//
// Two auth modes (set by the settings page, stored in `settings.authMode`):
//   - "builtin"        — username + password, silent re-auth on 401.
//   - "homeassistant"  — OAuth flow lives entirely in the settings page;
//                        on 401 we surface an inline error and stop polling
//                        until the user reopens settings.
// `settings.authMode` defaults to "builtin" for backward compatibility with
// pre-v1.1 saved settings.

function authMode() {
  return (settings && settings.authMode) || 'builtin';
}

// builtin-mode silent login.  Re-runs username+password against
// /auth/login and refreshes the cached token in place.  Errors out
// without retrying for HA mode — those sessions can only be restarted
// by the user reopening settings.
function login(cb) {
  if (!settings || !settings.host) {
    return cb(new Error('Settings not configured'));
  }
  if (authMode() === 'homeassistant') {
    return cb(new Error('Sign in expired — open Settings'));
  }
  if (!settings.username || !settings.password) {
    return cb(new Error('Settings not configured'));
  }
  console.log('[ma] login as', settings.username);
  http('POST', settings.host + '/auth/login', {
    provider_id: 'builtin',
    credentials: { username: settings.username, password: settings.password },
  }, null, function (err, status, body) {
    if (err) return cb(err);
    if (status !== 200 || !body) {
      return cb(new Error('Auth failed (' + status + ')'));
    }
    // The builtin envelope uses `token`; the HA envelope (handled in the
    // settings page) uses `access_token`.  We accept either defensively in
    // case the server normalises one day.
    var tok = body.token || body.access_token;
    if (!tok) return cb(new Error('Auth failed (no token in response)'));
    token = tok;
    saveToken(token);
    console.log('[ma] login ok');
    cb(null);
  });
}

// Validate the cached token via auth/me.  On success the response is the
// User record — we cache it on the settings blob so handlers can read it.
// On failure (most often 401), we report the cause; the caller decides
// whether to silently re-auth (builtin) or push an error (HA).
function authMe(cb) {
  if (!settings || !settings.host || !token) {
    return cb(new Error('Not authenticated'));
  }
  http('POST', settings.host + '/api',
    { message_id: String(msgId++), command: 'auth/me', args: {} },
    { Authorization: 'Bearer ' + token },
    function (err, status, body) {
      if (err) return cb(err);
      if (status === 401) return cb(new Error('Token invalid'));
      if (status < 200 || status >= 300) return cb(new Error('HTTP ' + status));
      // /api commands return either { result: <User> } or the raw user dict
      // depending on the MA build.  Both shapes are tolerated.
      var user = (body && body.result) || body || null;
      if (user) {
        settings.user = user;
        saveSettings(settings);
      }
      cb(null, user);
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

// Pick the active queue + the control player id.  Override wins; else
// playing > paused > anything available, alphabetical tiebreak by display_name.
//
// Returns { queue, controlPlayerId }:
//   - queue:           the PlayerQueue we READ state from (title, artist,
//                      progress, shuffle, repeat).  When the user has overridden
//                      to a synced MEMBER, we surface the MASTER's queue here
//                      because the member's own queue is inactive (members
//                      share the master's queue server-side).
//   - controlPlayerId: the player_id we WRITE to (volume / mute / group cmds
//                      and ST_PLAYER_NAME header readout).  Always the user's
//                      pick, even when we promoted a master for read state.
function pickActiveContext(queues, players) {
  if (!queues || queues.length === 0) return null;

  var byName = function (a, b) { return a.display_name.localeCompare(b.display_name); };
  var avail = queues.filter(function (q) { return q.available && q.active; });
  var availFallback = queues.filter(function (q) { return q.available; });

  // Resolve override.  Two sub-cases:
  //   1. Override is in `avail` already (solo / master) → use it directly,
  //      controlPlayerId === queue.queue_id.
  //   2. Override is a synced member — its own queue is active=false, so we
  //      look the player up and follow synced_to to the master's queue, but
  //      keep the member as controlPlayerId.
  if (overridePlayerId) {
    var direct = avail.find(function (q) { return q.queue_id === overridePlayerId; });
    if (direct) return { queue: direct, controlPlayerId: direct.queue_id };

    var pmap = playersById(players);
    var memberPlayer = pmap[overridePlayerId];
    if (memberPlayer && memberPlayer.synced_to) {
      var masterId = memberPlayer.synced_to;
      var masterQueue = avail.find(function (q) { return q.queue_id === masterId; })
                     || availFallback.find(function (q) { return q.queue_id === masterId; });
      if (masterQueue) return { queue: masterQueue, controlPlayerId: overridePlayerId };
    }

    // Override is also in availFallback (e.g. master that's available but
    // inactive) — accept it as-is.
    var fallbackDirect = availFallback.find(function (q) { return q.queue_id === overridePlayerId; });
    if (fallbackDirect) return { queue: fallbackDirect, controlPlayerId: overridePlayerId };

    // Override stale; drop it and fall through.
    overridePlayerId = null;
  }

  if (avail.length === 0) avail = availFallback;
  if (avail.length === 0) return null;

  var playing = avail.filter(function (q) { return q.state === 'playing'; }).sort(byName);
  if (playing.length) return { queue: playing[0], controlPlayerId: playing[0].queue_id };
  var paused  = avail.filter(function (q) { return q.state === 'paused' ; }).sort(byName);
  if (paused.length)  return { queue: paused[0],  controlPlayerId: paused[0].queue_id  };
  var first = avail.slice().sort(byName)[0];
  return { queue: first, controlPlayerId: first.queue_id };
}

// Derive group context for one player.  Returns
//   { isInGroup, masterId, members } — masterId is null when solo.
function groupCtxFor(player, players) {
  if (!player) return { isInGroup: false, masterId: null, members: [] };
  if (player.synced_to) {
    // Synced member — master is its synced_to.
    var pmap = playersById(players);
    var master = pmap[player.synced_to];
    var members = (master && Array.isArray(master.group_members)) ? master.group_members : [];
    return { isInGroup: true, masterId: player.synced_to, members: members };
  }
  if (Array.isArray(player.group_members) && player.group_members.length > 0) {
    return { isInGroup: true, masterId: player.player_id, members: player.group_members };
  }
  return { isInGroup: false, masterId: null, members: [] };
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

function pushNowPlaying(ctx, players) {
  if (!ctx || !ctx.queue) {
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
      ST_CONTROL_PLAYER_ID: '',
      ST_GROUP_VOLUME: -1,
      ST_GROUP_MUTED:  0,
    });
    return;
  }
  var queue = ctx.queue;
  var controlPlayerId = ctx.controlPlayerId;
  var item = queue.current_item || {};
  var media = item.media_item || {};
  var title = item.name || media.name || 'Unknown';
  var artist = '';
  if (media.artists && media.artists.length) artist = media.artists[0].name || '';
  else if (item.artist) artist = item.artist;
  var album = (media.album && media.album.name) || item.album || '';

  // Volume / mute come from the CONTROL player record (the user's pick),
  // not the master.  The header name also follows controlPlayerId.
  var pmap = playersById(players);
  var controlPlayer = pmap[controlPlayerId];
  var volume = controlPlayer && controlPlayer.volume_level != null ? controlPlayer.volume_level : 0;
  var muted  = controlPlayer && controlPlayer.volume_muted ? 1 : 0;

  // Header readout — prefer the control player's display_name (= user's pick).
  // Fall back to the queue's display_name when the player record is missing.
  var headerName = (controlPlayer && controlPlayer.display_name)
      || queue.display_name
      || '';

  // Group context drives the two-row volume window.  Read group volume from
  // the master player's record (group_volume is mirrored across members but
  // we trust the source — group commands target the master anyway).
  var grp = groupCtxFor(controlPlayer, players);
  var groupVolume = -1;
  var groupMuted = 0;
  if (grp.isInGroup && grp.masterId) {
    var masterPlayer = pmap[grp.masterId];
    if (masterPlayer && masterPlayer.group_volume != null) {
      groupVolume = masterPlayer.group_volume;
      groupMuted  = masterPlayer.group_volume_muted ? 1 : 0;
    }
  }

  // Signature for dedup.  elapsed_s is deliberately omitted — the watch
  // interpolates it locally between pushes.  controlPlayerId, header name,
  // group volume/muted ARE included so a user switch (override) or a group
  // volume change pushes through.
  var sig = [queue.queue_id, controlPlayerId, headerName, queue.state, title, artist, album,
             volume, muted, queue.shuffle_enabled ? 1 : 0, queue.repeat_mode,
             groupVolume, groupMuted].join('|');
  if (sig === lastSentSig) return;
  lastSentSig = sig;

  enqueue({
    ST_OK: 1,
    ST_PLAYER_NAME:       clamp32(headerName),
    ST_TITLE:             clamp32(title),
    ST_ARTIST:            clamp32(artist),
    ST_ALBUM:             clamp32(album),
    ST_STATE:             maStateToEnum(queue.state),
    ST_VOLUME:            volume,
    ST_MUTED:             muted,
    ST_SHUFFLE:           queue.shuffle_enabled ? 1 : 0,
    ST_REPEAT:            REPEAT[queue.repeat_mode] != null ? REPEAT[queue.repeat_mode] : REPEAT.off,
    ST_ELAPSED:           Math.floor(queue.elapsed_time || 0),
    ST_DURATION:          Math.floor((item && item.duration) || (media && media.duration) || 0),
    ST_CONTROL_PLAYER_ID: clamp32(controlPlayerId || ''),
    ST_GROUP_VOLUME:      groupVolume,
    ST_GROUP_MUTED:       groupMuted,
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
      var ctx = pickActiveContext(queues, players);
      pushNowPlaying(ctx, players);
      cb && cb(null, queues, players, ctx);
    });
  });
}

function schedulePoll(ms) {
  if (pollTimer) clearTimeout(pollTimer);
  pollTimer = setTimeout(function () {
    pollOnce(function (err, queues, players, ctx) {
      var queue = ctx && ctx.queue;
      var next = (queue && queue.state === 'playing') ? POLL_MS_PLAYING : POLL_MS_IDLE;
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
//
// Resolves the active read-queue AND the control player id for an outbound
// command.  Both are forwarded to the callback:
//
//   queue     = the PlayerQueue to read state from / target with queue cmds
//               (play_pause, next, previous, shuffle, repeat, play_media).
//   controlId = the player_id to target with player cmds (volume_*, *_mute).
//   player    = the Player record for controlId (gives current volume/mute).
//   players   = the full /players list (callers that need a master lookup).
function withActive(cb) {
  maCall('player_queues/all', {}, function (err, qres) {
    if (err) { pushError(err.message); return; }
    var queues = (qres && qres.result) || qres || [];
    // We also fetch /players to get current volume for ± commands AND to let
    // pickActiveContext resolve overridden members via their synced_to.
    maCall('players/all', {}, function (err2, pres) {
      if (err2) { pushError(err2.message); return; }
      var players = (pres && pres.result) || pres || [];
      var ctx = pickActiveContext(queues, players);
      if (!ctx) { pushError('No active player'); return; }
      var player = players.find(function (p) { return p.player_id === ctx.controlPlayerId; });
      cb(ctx.queue, ctx.controlPlayerId, player, players);
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
      withActive(function (queue) {
        maCall('player_queues/play_pause', { queue_id: queue.queue_id }, function (err) {
          if (err) pushError(err.message);
          quickRepoll();
        });
      });
      break;

    case CMD.NEXT:
      withActive(function (queue) {
        maCall('player_queues/next', { queue_id: queue.queue_id }, function (err) {
          if (err) pushError(err.message);
          quickRepoll();
        });
      });
      break;

    case CMD.PREVIOUS:
      withActive(function (queue) {
        maCall('player_queues/previous', { queue_id: queue.queue_id }, function (err) {
          if (err) pushError(err.message);
          quickRepoll();
        });
      });
      break;

    case CMD.VOLUME_UP:
    case CMD.VOLUME_DOWN:
      // Player command — target the CONTROL player (the user's pick), not the
      // queue.  This is the v1.1 bugfix: when overridden to a synced member,
      // queue.queue_id is the master, but volume must hit the member.
      withActive(function (queue, controlId, player) {
        var current = (player && player.volume_level != null) ? player.volume_level : 50;
        var target = current + (cmd === CMD.VOLUME_UP ? VOLUME_STEP : -VOLUME_STEP);
        if (target < 0) target = 0;
        if (target > 100) target = 100;
        maCall('players/cmd/volume_set',
          { player_id: controlId, volume_level: target },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.MUTE_TOGGLE:
      withActive(function (queue, controlId, player) {
        var muted = !(player && player.volume_muted);
        maCall('players/cmd/volume_mute',
          { player_id: controlId, muted: muted },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.SHUFFLE_TOGGLE:
      withActive(function (queue) {
        // Queue command — shuffle_enabled is on the queue.
        var next = !queue.shuffle_enabled;
        maCall('player_queues/shuffle',
          { queue_id: queue.queue_id, shuffle_enabled: next },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.REPEAT_CYCLE:
      withActive(function (queue) {
        var cur = queue.repeat_mode || 'off';
        var nextMode = cur === 'off' ? 'all' : cur === 'all' ? 'one' : 'off';
        maCall('player_queues/repeat',
          { queue_id: queue.queue_id, repeat_mode: nextMode },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.QUICK_PLAY:
      withActive(function (queue) {
        var uri = payload.ARG_STR;
        if (!uri) { pushError('Empty quick-play URI'); return; }
        maCall('player_queues/play_media',
          { queue_id: queue.queue_id, media: uri, option: 'replace' },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    // ─── Group volume (v1.1) ────────────────────────────────────────────
    // Targets the MASTER player resolved from the control player's synced_to
    // (or controlId itself when it's already the master / a non-sync group
    // master).  group_volume / group_volume_mute commands always take the
    // master's player_id.
    case CMD.GROUP_VOLUME_UP:
    case CMD.GROUP_VOLUME_DOWN:
      withActive(function (queue, controlId, player, players) {
        var pmap = playersById(players);
        var ctrl = pmap[controlId];
        var masterId = (ctrl && ctrl.synced_to) ? ctrl.synced_to : controlId;
        var master = pmap[masterId];
        var current = (master && master.group_volume != null) ? master.group_volume : 50;
        var target = current + (cmd === CMD.GROUP_VOLUME_UP ? VOLUME_STEP : -VOLUME_STEP);
        if (target < 0) target = 0;
        if (target > 100) target = 100;
        maCall('players/cmd/group_volume',
          { player_id: masterId, volume_level: target },
          function (err) { if (err) pushError(err.message); quickRepoll(); });
      });
      break;

    case CMD.GROUP_MUTE_TOGGLE:
      withActive(function (queue, controlId, player, players) {
        var pmap = playersById(players);
        var ctrl = pmap[controlId];
        var masterId = (ctrl && ctrl.synced_to) ? ctrl.synced_to : controlId;
        var master = pmap[masterId];
        var current = !!(master && master.group_volume_muted);
        maCall('players/cmd/group_volume_mute',
          { player_id: masterId, muted: !current },
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
  // Pull the cached HA token (or builtin token) out of settings if the
  // settings page populated it on its last save.  Token in localStorage
  // takes precedence for the builtin re-auth path.
  if (!token && settings.accessToken) {
    token = settings.accessToken;
    saveToken(token);
  }
  if (token) {
    // Probe the cached token first.  On success we know the session is
    // still valid and start polling immediately; otherwise branch on
    // authMode and either silently re-auth (builtin) or surface a
    // "Sign in expired" toast (homeassistant).
    authMe(function (err, user) {
      if (!err) {
        console.log('[ma] auth/me ok as', user && (user.display_name || user.username));
        schedulePoll(POLL_MS_AFTER_ACTION);
        return;
      }
      console.log('[ma] auth/me failed:', err.message);
      if (authMode() === 'homeassistant') {
        token = null; saveToken(null);
        pushError('Sign in expired — open Settings');
        return;
      }
      // builtin — try a silent re-auth.
      token = null; saveToken(null);
      login(function (lerr) {
        if (lerr) {
          pushError('Sign in expired — open Settings');
          return;
        }
        schedulePoll(POLL_MS_AFTER_ACTION);
      });
    });
  } else {
    // No cached token — let maCall trigger login on first use.
    schedulePoll(POLL_MS_AFTER_ACTION);
  }
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  if (p.CMD != null) handleWatchCmd(p.CMD, p);
});

Pebble.addEventListener('showConfiguration', function () {
  var cur = loadSettings() || {};
  var slotsJson = JSON.stringify(Array.isArray(cur.slots) ? cur.slots : []);
  var userJson = cur.user ? JSON.stringify(cur.user) : '';
  var qs = 'host='        + encodeURIComponent(cur.host || '')
         + '&user='       + encodeURIComponent(cur.username || '')
         + '&hasPass='    + (cur.password ? '1' : '0')
         + '&slots='      + encodeURIComponent(slotsJson)
         + '&authMode='   + encodeURIComponent(cur.authMode || 'builtin')
         + '&hasToken='   + (cur.accessToken ? '1' : '0')
         + '&userBlob='   + encodeURIComponent(userJson);
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
  var mode = (next.authMode === 'homeassistant') ? 'homeassistant' : 'builtin';
  var merged = {
    host:        (next.host || '').replace(/\/+$/, ''),
    username:    next.username || '',
    password:    next.password || prev.password || '',
    slots:       slots,
    authMode:    mode,
    accessToken: next.accessToken || null,
    user:        next.user || null,
  };
  saveSettings(merged);
  settings = merged;
  // Token rotation: if the page handed us a fresh token (either builtin
  // re-auth or a completed HA OAuth roundtrip), adopt it; otherwise clear
  // the cached one so the next maCall triggers a fresh login flow.
  if (merged.accessToken) {
    token = merged.accessToken;
    saveToken(token);
  } else {
    saveToken(null); token = null;
  }
  pushQuickSlots();
  console.log('[ma] settings saved (' + slots.length + ' quick slots, mode=' + mode + ')');
  schedulePoll(POLL_MS_AFTER_ACTION);
});
