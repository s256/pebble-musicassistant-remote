# pebble-musicassistant-remote

> A Pebble Time 2 remote for [Music Assistant](https://www.music-assistant.io).
> Touch-first. Native C. REST API.

[![Version](https://img.shields.io/badge/version-1.0.0-blue)](./CHANGELOG.md) [![Platform](https://img.shields.io/badge/platform-emery-orange)](https://developer.repebble.com/) [![License](https://img.shields.io/badge/license-MIT-green)](./LICENSE)

A native watchapp for the **Pebble Time 2** (`emery`, 200×228 colour, touchscreen) that turns your wrist into a full Music Assistant remote — transport, volume, player switching, multi-room grouping, and one-tap Quick Play shortcuts to your library.

Works with Music Assistant as a **standalone server** and as a **Home Assistant add-on**.

---

## Features

### Now playing
- Track title, artist, album, playback state, elapsed/total time, scrolling progress bar.
- **Right-edge action column**: previous · play/pause · next — tappable AND bound to the watch's `UP` / `SELECT` / `DOWN` buttons.
- **Bottom status strip** showing the live state of shuffle, repeat, volume. Each zone is tappable:
  - Tap shuffle → toggle immediately.
  - Tap repeat → cycle off → all → one.
  - Tap volume → open the volume control screen.
- Header shows the active player; tap it to switch players.
- Long-press `SELECT` → opens **Quick Play** (see below).

### Players
- Lists every Music Assistant player the server exposes, sorted alphabetically.
- A small coloured dot marks each player's state: green = playing, amber = paused, grey = idle.
- **Active-player auto-selection**: the watch picks the player that's currently *playing*; tie-breaks alphabetically. If none are playing, it picks the most-recently *paused* one.
- Tap any row to override and control that player instead.
- **Grouping** (multi-room): long-press a row to open an action sheet — join another player's group, leave the group, or tear down the whole group. Members of a group render indented under their master, with a `⛓ N` chain badge on the master row.

### Volume
- Dedicated full-screen window with a large percent readout and a chunky bar.
- `UP` / `DOWN` step the volume (repeating on hold).
- `SELECT` toggles mute.
- Reachable by tapping the volume cluster in the bottom strip on the now-playing screen.

### Quick Play
- Up to **10 user-curated shortcuts** to albums, artists, or playlists already in your Music Assistant library.
- Long-press `SELECT` on the now-playing screen to open them.
- Tap a row to start playback on the current player.
- Watch persists the slot list locally so the menu works even before the phone has synced.

### Settings (Discover panel)
- The settings page authenticates against your Music Assistant server directly from your phone's browser.
- Once connected, you get a **Discover panel** with a debounced search box and three filter chips (Album · Artist · Playlist). Tap a result to add it as a Quick Play shortcut.
- Reorder, rename, and remove shortcuts inline. No URI pasting required.

### Under the hood
- Pure native Pebble C. No on-watch JavaScript, no Moddable, no WebSocket on the watch.
- All Music Assistant traffic happens **on the phone** via PebbleKit JS. The watch only sees AppMessage state and forwards user intent.
- REST `POST /api` with the standard `{command, args}` envelope and bearer-token auth (`POST /auth/login` with built-in credentials → JWT, refreshed on 401).
- Polling cadence: 2 s while playing, 5 s while paused/idle, 250 ms one-shot after a user action (so the UI feels instantaneous).
- 1 Hz interpolation tick on the watch so the progress bar moves smoothly between phone polls — gated by `app_focus_service` so the watch can actually sleep when the screen blanks.
- Non-Latin punctuation (smart quotes, em-dashes, etc.) is transliterated to ASCII before being sent to the watch so Pebble's bundled fonts render every track name cleanly.

---

## Requirements

| | |
|---|---|
| Watch | Pebble Time 2 (`emery`) — or the emery emulator from the Rebble SDK |
| Phone | The [rePebble](https://repebble.com/app) mobile app paired to your watch |
| Server | Music Assistant ≥ 2.9 — standalone or Home Assistant add-on |
| Account | A built-in Music Assistant user (username + password) |

---

## Install

### From a release (recommended)

1. Download `pebble-musicassistant-publish.pbw` from the latest [GitHub release](../../releases).
2. Open it from the rePebble mobile app. The watch installs it automatically.

### From source

```bash
# One-time SDK setup
sudo dnf install nodejs SDL2 glib2 pixman zlib                                 # Fedora
sudo apt install nodejs npm libsdl2-2.0-0 libglib2.0-0 libpixman-1-0 zlib1g    # Ubuntu

uv tool install pebble-tool
pebble sdk install latest

git clone git@github.com:s256/pebble-musicassistant-remote.git
cd pebble-musicassistant-remote

# Build for emery
pebble build

# Run in the emulator
pebble install --emulator emery --logs

# Sideload onto a paired watch via the phone
pebble login            # one-time GitHub auth
pebble install --cloudpebble

# Build a clean publish artifact (strips JS source map → no machine-path leaks)
./scripts/make-publish-pbw.sh
# Output: build/pebble-musicassistant-publish.pbw
```

---

## Configure

After install, open **rePebble app → My Watches → Music Assistant → Settings**. The settings page opens in your phone's browser.

| Field | Example | Notes |
|---|---|---|
| **Host** | `http://192.168.80.10:8095` | Music Assistant base URL. Use `https://` if you have TLS. |
| **Username** | `alice` | Built-in Music Assistant user. |
| **Password** | `••••••••` | Stored locally in the rePebble app — never sent to the watch. |

Click **Test connection**. If it succeeds, the **Discover** panel unlocks — search your library and tap items to add them as Quick Play shortcuts. Save when done.

> **Home Assistant add-on:** point Host at the add-on's exposed port (default `8095`) on your HA host (e.g. `http://homeassistant.local:8095`). No ingress path prefix needed — the add-on exposes the same API.
>
> **CORS:** the settings page is served from `https://s256.github.io`, so your Music Assistant server must allow that origin to send `POST /auth/login` and `POST /api`. The MA add-on doesn't ship CORS headers by default; the cleanest fix is a small Traefik / Caddy / nginx middleware in front of MA that injects `Access-Control-Allow-Origin: https://s256.github.io` plus the matching `Allow-Methods` and `Allow-Headers`.

---

## Controls reference

### Now playing

| Action | Touch | Button |
|---|---|---|
| Play / pause | Tap centre action icon (right edge) | `SELECT` short |
| Previous track | Tap top action icon | `UP` short |
| Next track | Tap bottom action icon | `DOWN` short |
| Toggle shuffle | Tap shuffle zone in bottom strip | — |
| Cycle repeat (off → all → one) | Tap repeat zone in bottom strip | — |
| Open volume control | Tap volume zone in bottom strip | — |
| Open player list | Tap the header (player name + chevron) | — |
| Open Quick Play | — | `SELECT` long-press |
| Exit app | — | `BACK` |

### Volume window

| Action | Button |
|---|---|
| Volume up | `UP` (repeats while held) |
| Volume down | `DOWN` (repeats while held) |
| Mute toggle | `SELECT` |
| Back to now playing | `BACK` |

### Players list

| Action | Touch | Button |
|---|---|---|
| Select player | Tap row | `UP`/`DOWN` to highlight, `SELECT` to confirm |
| Group menu | Long-press row | `SELECT` long-press |
| Back | — | `BACK` |

### Group action sheet

| Variant | Rows |
|---|---|
| Player is **solo / ungrouped** | One row per other player — "Join X" (if X is a master) or "Group with X" (if solo). Tap to join. |
| Player is **a group member** | • `⏏ Leave group` — drop out, others keep playing<br>• `＋ Add to this group` — pick another player to add (drill-down)<br>• `⚠ Ungroup all` — tear down the whole group |

### Quick Play

| Action | Button |
|---|---|
| Open | `SELECT` long-press on now playing |
| Scroll | `UP` / `DOWN` |
| Play selected | `SELECT` or tap |
| Back | `BACK` |

---

## Project layout

```
.
├── CHANGELOG.md            human-readable per-version release notes
├── LICENSE                 MIT
├── README.md               this file
├── package.json            Pebble app manifest (UUID, target platforms, message keys, icon)
├── wscript                 waf build orchestration
├── docs/
│   └── index.html          settings page (served as GitHub Pages)
├── resources/
│   ├── icon-25.png         25×25 menu-launcher icon embedded in the .pbw
│   └── icon-source.svg     design source for the icon
├── scripts/
│   └── make-publish-pbw.sh strips JS source map from build/.pbw for clean upload
├── src/
│   ├── c/main.c            native watchapp — windows, layers, touch, AppMessage
│   └── pkjs/index.js       PebbleKit JS — REST client, auth, polling, settings bridge
```

---

## License

MIT — see [LICENSE](./LICENSE).
