# Pebble Music Assistant Remote — Concept & Architecture

> **Status:** v0.1.1 shipped — touch + icon polish on v0.1.0. v0.2.0 is the
> in-progress redesign captured in §4 and §5.  Architecture stabilises at v1.0.0.
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

## 4. UI (v0.2.0 design — finalised, not yet implemented)

The home screen is rebuilt around **Pebble idiom**, not mobile-app convention.
Three guiding rules that emerged from real Pebble UX research, not the early
mockups:

1. **The hardware Back button is the universal "up" gesture.** No persistent
   docks, no global tab bars. Nest sub-screens via `window_stack_push`, return
   via Back. Every first-party Pebble app does this.
2. **The right edge is for controls.** Pebble's `ActionBarLayer` is the
   canonical place for the three hardware buttons (UP / SELECT / DOWN) to be
   mirrored on-screen — and on Time 2, those icons are also touch targets.
   Stop drawing custom floating pills; use the idiom.
3. **State at the bottom, controls on the right.** A wide, readable bottom
   strip shows *what's currently true* (shuffle, repeat, volume). Glanceable.
   It's tappable — taps act immediately (no extra sub-screen) — but it's not
   visually a "button row".

### 4.1 Now-playing

```
┌───────────────────────────────┐
│ Front room               ▸   │  header — player name + chevron (tap = list)
├──────────────────────────┬────┤
│                          │ ◀◀ │  ← UP = prev   (tap mirrors button)
│ Track Title              │    │
│ Artist — Album           ├────┤
│                          │ ▶  │  ← SELECT = play/pause
│                          │    │  (long-press = Quick Play menu)
│ ──────────────────       ├────┤
│ 1:23 / 4:56              │ ▶▶ │  ← DOWN = next
│                          │    │
├──────────────────────────┴────┤
│  🔀 ON     ↻ ALL     🔈 65%  │  bottom strip — state + tap targets
└───────────────────────────────┘
```

- **Right-edge action bar** (≈ 30 px wide) holds prev / play-pause / next.
  Bound to UP / SELECT / DOWN and tap-bound on Time 2. Pebble Music itself
  uses this layout — it's instantly familiar.
- **Bottom status strip** (≥ ~36 px tall — generous, not 16 px sliver):
  - **Shuffle icon + ON/OFF text** — tap toggles immediately.
  - **Repeat icon + OFF/ALL/ONE text** — tap cycles immediately.
  - **Speaker icon + percentage** — tap opens the existing volume window.
  - Plenty of vertical room for legible text alongside the icons; reads as
    "current state of playback", not as "menu of buttons".
- **Header** is the player name + chevron — tap opens the players list.
- **Long-press SELECT** opens the **Quick Play** menu (see §4.5).

#### Button mapping on the now-playing window

| Button | Short press | Long press |
|---|---|---|
| UP | Previous track | — |
| SELECT | Play / pause | **Open Quick Play** |
| DOWN | Next track | — |
| BACK | Exit app | — |

> Volume is no longer on UP/DOWN here — those are transport, matching Pebble
> Music. To change volume, tap the volume cluster in the strip → opens the
> volume window where UP/DOWN step volume and SELECT mutes. Back returns.

### 4.2 Players list (unchanged from v0.1.1 — works well)

```
┌──────────────────────────────┐
│ Players                      │
├──────────────────────────────┤
│ ● Back garden                │  ← green dot = playing
│   Playing                    │
├──────────────────────────────┤
│ ● Bedroom                    │  ← amber dot = paused
│   Paused                     │
├──────────────────────────────┤
│ ● Kitchen                    │  ← grey dot = idle
│   Idle                       │
└──────────────────────────────┘
```

- Tap a row, or use UP / DOWN + SELECT, to set that player as active.
- All row text white on black; highlight is white-on-cerulean.
- Back returns to now-playing.

> Future: **long-press a row → group / ungroup menu** (post-MVP, captured
> for the roadmap but not in 0.2.0 scope).

### 4.3 Volume window (unchanged from v0.2.0 work-in-progress)

```
┌──────────────────────────────┐
│ Volume                       │
│                              │
│                              │
│           65 %               │  big readout
│                              │
│   🔈 ░░░░░░░░░░             │  chunky bar
│                              │
│   UP / DOWN  •  SELECT mutes │
└──────────────────────────────┘
```

- No touch. UP / DOWN step volume (repeating); SELECT toggles mute; Back returns.

### 4.4 Settings entry

PebbleKit JS catches `showConfiguration` and opens the hosted page
(`docs/index.html` on GitHub Pages). User enters host + username + password;
on `webviewclosed` the JS persists to `localStorage` and re-auths.

The same page also hosts the **Quick Play picker** (see §4.5).

### 4.5 Quick Play (new in v0.2.0)

A small, user-curated set of one-tap shortcuts to start playback of an album,
artist, or playlist that already exists in Music Assistant.

**How users open it**: long-press SELECT on the now-playing window.

```
   now-playing
     ↓ long-press SELECT
┌──────────────────────────────┐
│ Quick Play                   │
├──────────────────────────────┤
│ ▶ Morning Jazz               │
│ ▶ Workout                    │
│ ▶ The Beatles                │
│ ▶ Daily mix                  │
└──────────────────────────────┘
```

- MenuLayer; UP / DOWN scroll, SELECT or tap = play that item on the current
  active player. Back returns.
- Empty state: "Add shortcuts in Settings."

#### Configuration

Quick-play entries are configured on the **settings page** (`docs/index.html`).

Up to **10 slots**, each `{ label, uri }` where `uri` is whatever Music Assistant
accepts in `player_queues/play_media` (e.g. `library://playlist/42`,
`library://album/…`, `spotify://album/…`).

The settings page does NOT make the user type raw URIs. Once host + credentials
are filled in and validated, the page gains a **Discover** panel that calls
Music Assistant's library APIs directly from the browser:

- One search box (debounced) → MA `music/search` or equivalent.
- Filter chips: **Album · Artist · Playlist**.
- Tap a result → adds it to the slot list as a new shortcut with a
  pre-filled label (editable).
- Drag-and-drop / arrow buttons to reorder the up-to-10 slots.
- "Save" returns the whole settings blob (including the slot list) to the
  watch via `pebblejs://close#...` just like today.

##### Auth flow inside the settings page (new)

The settings page now performs its own POST to `<host>/auth/login` to obtain
a short-lived bearer token, which it uses for the Discover panel's MA calls.
The token never leaves the browser tab. If auth fails, the page shows an
inline error and disables Discover, but still lets the user Save host / user /
password changes.

##### AppMessage contract for slots

Existing `messageKeys` are extended with:

```
QUICK_BEGIN, QUICK_END                     // marker
QUICK_ROW_INDEX (int)                       // 0..9
QUICK_ROW_LABEL (str)                       // up to ~40 chars
QUICK_ROW_URI   (str)                       // up to ~120 chars
```

PebbleKit JS pushes the slot list on settings save and on app launch; the
watch caches it in `persist_*` so the menu is available offline.

When the user picks a slot, the watch sends `CMD_QUICK_PLAY` with the URI as
`ARG_STR`; the phone calls `player_queues/play_media` with `{ queue_id,
media: uri, option: "replace" }`.

---

## 5. Touchscreen feature catalogue

Mandatory in v0.2.0:

- **Tap header** → open player list.
- **Tap action-bar icons** (right edge) → prev / play-pause / next.
- **Tap shuffle in bottom strip** → toggle immediately.
- **Tap repeat in bottom strip** → cycle immediately.
- **Tap volume cluster in bottom strip** → open volume window.
- **Tap player row in player list** → select active player.
- **Tap Quick Play row** → start playback on active player.

High value, post-MVP (tracked in roadmap):

- **Long-press player row** → group / ungroup menu.
- **Drag on volume bar (in volume window)** → scrub volume.
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

| Version | Theme                  | Highlights                                                                                                                                                                                                                       |
| ------- | ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0.1.0   | Bootstrap              | MVP feature list above.                                                                                                                                                                                                          |
| 0.1.1   | Touch + icon polish    | Native-primitive icons (no more tofu boxes), enlarged status-bar tap target. **Verified working — revert target if 0.2.x regresses.**                                                                                            |
| 0.2.0   | **UX rebuild + Quick Play** | ActionBarLayer on the right edge, generous bottom status strip, immediate-toggle shuffle/repeat, **long-press SELECT → Quick Play menu**, Discover panel on the settings page (search MA library, build up to 10 shortcuts). |
| 0.3.0   | Grouping               | Long-press player row → group / ungroup, group volume readout.                                                                                                                                                                   |
| 0.4.0   | Queue & library deeper | Queue view (swipe down), recently played, favourite toggle.                                                                                                                                                                      |
| 0.5.0   | Stability              | Exponential reconnect, token refresh, settings validation, error toasts. Also the 0.2.0 "filed for 0.2.0" punchlist (see CHANGELOG).                                                                                             |
| 0.6.0   | Polish                 | Album art (where palette allows), animations, App Glance for "now playing".                                                                                                                                                      |
| 0.7.0   | Multi-watch            | Companion modes (e.g. Pebble Time Round / `chalk`), shared settings.                                                                                                                                                             |
| 0.9.0   | Beta hardening         | Field testing, telemetry-free crash log capture, full keyboard nav for settings page.                                                                                                                                            |
| 1.0.0   | First stable release   | Rebble appstore submission, frozen API contract.                                                                                                                                                                                 |

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
