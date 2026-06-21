# Pebble Music Assistant Remote — Concept & Architecture

> **Status:** v0.1.0 MVP scope. Architecture stabilises at v1.0.0.
> **Target platform:** Pebble Time 2 (`emery`, 200×228 colour, touchscreen).
> **Server target:** [Music Assistant](https://www.music-assistant.io) ≥ 2.9 (standalone or Home Assistant add-on).

---

## 1. Goal

A wrist remote for Music Assistant on Pebble Time 2 that:

1. Auto-selects the "currently playing" player from the server's player list.
2. Lets you flick between players with a single tap.
3. Exposes the full transport: play/pause, next/prev, shuffle, repeat, volume up/down/mute.
4. Treats the Time 2 touchscreen as a first-class input — buttons are still wired for parity.
5. Works against Music Assistant when it's a standalone server *or* the Home Assistant add-on.

---

## 2. Why not the existing `pebble-ma`?

The reference watchapp [`vincentezw/pebble-ma`](https://github.com/vincentezw/pebble-ma) is the prior art. It validates the idea but has design constraints we deliberately move past:

| `pebble-ma`                                  | This project                                                       |
| -------------------------------------------- | ------------------------------------------------------------------ |
| Moddable SDK + on-watch JS (Piu)             | Native Pebble C — frees heap, unlocks `TouchService`               |
| WebSocket on the phone                       | REST `/api` with bearer token — simpler, no on-watch socket budget |
| Long-lived token only                        | Built-in auth (username + password) per the user's brief           |
| Single hard-coded "first playing" player     | Active-player list with alphabetical tiebreak + manual override    |
| Button-only                                  | Touch-first, buttons as fallback                                   |
| Plain Clay phone settings page               | Hosted settings page on GitHub Pages                               |

---

## 3. Architecture

```
  ┌─────────────────────────────────────────────┐
  │ Pebble Time 2 (emery)                       │
  │                                             │
  │  src/c/main.c                               │
  │   • now-playing Window                      │
  │   • player-list Window (touch-driven)      │
  │   • TouchService → tap routing             │
  │   • AppMessage in/out                      │
  │                                             │
  └────────────────┬────────────────────────────┘
                   │  AppMessage (binary keyed dict)
  ┌────────────────▼────────────────────────────┐
  │ Phone — PebbleKit JS (src/pkjs/index.js)    │
  │   • POST /auth/login → JWT bearer          │
  │   • POST /api {command, args}               │
  │   • polled state (2 s tick + on-demand)    │
  │   • AppMessage router                       │
  │   • settings via webviewclosed             │
  └────────────────┬────────────────────────────┘
                   │  HTTPS / HTTP REST
  ┌────────────────▼────────────────────────────┐
  │ Music Assistant (standalone or HA add-on)   │
  │   POST /auth/login                          │
  │   POST /api                                 │
  └─────────────────────────────────────────────┘
```

### 3.1 Why REST, not WebSocket

PebbleKit JS *does* expose `WebSocket`, but:

- AppMessage is the bottleneck anyway — bursts > a few hundred bytes are dropped.
- REST keeps the phone process stateless across kills (PebbleKit JS is aggressively suspended on iOS).
- The OpenAPI at `/api-docs/openapi.json` confirms a single `POST /api` envelope handles every command — the same `{command, args}` shape MA uses internally. Future WebSocket upgrade is mechanical.

### 3.2 Auth flow

```text
phone                     server
  │  POST /auth/login      │
  │  {provider_id:"builtin",
  │   credentials:{username,password}}
  │ ─────────────────────► │
  │                        │  validates against built-in user store
  │ ◄───── 200 {token} ─── │
  │                        │
  │  POST /api             │
  │  Authorization: Bearer <token>
  │  {command, args}       │
  │ ─────────────────────► │
```

The token is cached in `localStorage` and refreshed on 401.

### 3.3 Active-player selection

Each poll the phone calls `player_queues/all`. From the returned `PlayerQueue[]`:

1. Filter `available && active`.
2. If any are `state == "playing"` — sort by `display_name`, pick first.
3. Else if any are `state == "paused"` — sort by `display_name`, pick first.
4. Else sort all active by `display_name`, pick first.
5. Else: no player → display "no player" placeholder.

A manual override (user picks from the player list) wins until they switch back to "auto".

### 3.4 Polling cadence

| State                                  | Interval                    |
| -------------------------------------- | --------------------------- |
| Player playing, watch foregrounded     | 2 s                         |
| Player paused / idle, foregrounded     | 5 s                         |
| Optimistic — right after a user action | 250 ms one-shot then resume |
| Watch backgrounded (no AppMessage)     | poll halts                  |

The watch interpolates the elapsed-time counter between polls so the progress bar moves smoothly.

### 3.5 Volume / mute

- `players/cmd/volume_set` takes `{ player_id, volume_level: 0..100 }`.
- `players/cmd/volume_up` / `players/cmd/volume_down` exist server-side — used for ± buttons.
- `players/cmd/volume_mute` takes `{ player_id, muted: bool }`.

### 3.6 Shuffle / repeat

These live on the *queue*, not the player:

- `player_queues/shuffle` `{ queue_id, shuffle_enabled }`
- `player_queues/repeat` `{ queue_id, repeat_mode: "off"|"one"|"all" }`

---

## 4. UI

### 4.1 Now-playing (default window)

```
┌──────────────────────────────┐
│ ♫ Track Title                │  large font, scrolls
│   Artist — Album             │  medium font
│                              │
│        ▶            (state)  │  large state glyph
│   ◀◀   ⏯   ▶▶               │  3 transport buttons (touch + Pebble SELECT)
│   🔀   🔁                    │  shuffle / repeat (touch)
│                              │
│ ────────────────────         │  progress bar
│ 1:23 / 4:56                  │
│                              │
│ Front room          ▮▮▮▮▯ 70 │  status bar — player + volume — TAPPABLE
└──────────────────────────────┘
```

- **Pebble UP / DOWN** → volume up / down (long-press toggles mute).
- **Pebble SELECT** → play/pause.
- **Pebble BACK** → only exits app (system default).
- **Touch in transport row** → play/pause/next/prev.
- **Touch on shuffle/repeat glyphs** → toggle.
- **Tap status bar** → open player list.
- **Vertical swipe on right edge** → volume slider (post-MVP).

### 4.2 Player list

```
┌──────────────────────────────┐
│ Players                  ✕   │
│ ────────────────────────     │
│  ▶  Back garden       AUTO   │  active player, marker
│  ⏸  Bedroom                  │
│  ◼  Kitchen                  │
│  ◼  Living room              │
└──────────────────────────────┘
```

- Whole row is a touch target — tap to select that player.
- Selecting a player flips the override; `AUTO` returns when the user taps the active row twice.
- The list sorts alphabetically — same order as the auto-select tiebreak.

### 4.3 Settings entry

PebbleKit JS catches `showConfiguration` and opens the hosted page (`docs/index.html` on GitHub Pages). User enters host + username + password; on `webviewclosed` the JS persists to `localStorage` and re-auths.

---

## 5. Touchscreen feature catalogue

Mandatory (v0.1.0):

- **Tap status bar** → open player list.
- **Tap transport icons** → play/pause/next/prev.
- **Tap shuffle/repeat glyphs** → cycle.
- **Tap player row** → select active player.

High value, post-MVP (tracked in roadmap):

- **Drag on volume bar** → scrub volume.
- **Drag on progress bar** → seek within track.
- **Long-press track title** → favourite.
- **Swipe down on now-playing** → queue view.
- **Tap album art** (when introduced) → expand cover.

All gestures are derived in software from the raw `TouchService` events (`Touchdown`, `Liftoff`, `PositionUpdate`). The watch maintains a tiny gesture state machine: position + dwell-time + delta classifies tap vs drag vs swipe.

---

## 6. Feature list (MVP, v0.1.0)

- [x] Connection to Music Assistant via REST `POST /api`.
- [x] Built-in auth (username + password → JWT bearer).
- [x] Home Assistant add-on supported (just configure host + path).
- [x] List players, auto-select active, alphabetical tiebreak.
- [x] Now-playing screen: title, artist, album, state glyph, progress bar.
- [x] Controls: play, pause, next, previous.
- [x] Shuffle toggle, repeat cycle (off → all → one).
- [x] Volume: up, down, mute, level indicator.
- [x] Player list screen reached by tap on status bar.
- [x] Touch + button parity.
- [x] Hosted settings page on GitHub Pages.

## 7. Roadmap

| Version | Theme                | Highlights                                                                            |
| ------- | -------------------- | ------------------------------------------------------------------------------------- |
| 0.1.0   | Bootstrap (this)     | MVP feature list above.                                                               |
| 0.2.0   | Touch upgrades       | Volume drag, progress seek, gesture state machine cleanup.                            |
| 0.3.0   | Queue & library      | Queue view (swipe down), recently played, favourite toggle.                           |
| 0.4.0   | Stability            | Exponential reconnect, token refresh, settings validation, error toasts.              |
| 0.5.0   | Polish               | Album art (where palette allows), animations, App Glance for "now playing".           |
| 0.6.0   | Multi-watch          | Companion modes (e.g. Pebble Time Round / `chalk`), shared settings.                  |
| 0.9.0   | Beta hardening       | Field testing, telemetry-free crash log capture, full keyboard nav for settings page. |
| 1.0.0   | First stable release | Rebble appstore submission, frozen API contract.                                      |

## 8. Non-goals (for now)

- On-watch WebSocket client. The phone is fine.
- On-watch library browsing beyond the queue. The Time 2 is a remote, not a player.
- Voice control / dictation.
- Multi-room grouping management — exposed read-only; group editing stays in the MA web UI.
- Cover-art download for every track — bandwidth and memory budget aren't there at v0.1.0.

## 9. Versioning policy

- Semantic versioning.
- 0.x.y — pre-1.0, breaking changes allowed; direct pushes to `main` permitted.
- Each tagged release ships a `pbw` artefact and a CHANGELOG entry.
- After 1.0.0 — feature work on branches, releases via PR, breaking changes only on majors.
