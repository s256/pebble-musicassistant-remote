# pebble-musicassistant-remote

> A Pebble Time 2 remote for [Music Assistant](https://www.music-assistant.io).
> Touch-first. Native C. REST API. Built-in auth.

[![Version](https://img.shields.io/badge/version-0.1.0-blue)](./CHANGELOG.md) [![Platform](https://img.shields.io/badge/platform-emery-orange)](https://developer.repebble.com/) [![License](https://img.shields.io/badge/license-MIT-green)](./LICENSE)

---

## What it is

A native watchapp for the **Pebble Time 2** (`emery`, 200×228 colour, touchscreen) that controls a [Music Assistant](https://www.music-assistant.io) instance over its REST API.

- Auto-selects the player that's currently playing.
- Multiple active players → picks the first one alphabetically.
- Tap the status bar to switch player.
- Play / pause / next / previous / shuffle / repeat.
- Volume up / down / mute.
- Works with Music Assistant as a **standalone server** and as a **Home Assistant add-on**.

See **[CONCEPT.md](./CONCEPT.md)** for architecture, design decisions, and the full roadmap.

---

## Requirements

- Pebble Time 2 (or Pebble Time 2 emulator).
- The new Pebble mobile app (rePebble) paired to your watch.
- A Music Assistant server reachable from the phone — `2.9.0+` recommended.
- A built-in user account on Music Assistant (username + password).

---

## Install

### From a release

1. Grab the latest `.pbw` from [Releases](../../releases).
2. Open it from the Pebble mobile app.

### From source

```bash
# One-time SDK setup (Linux)
sudo dnf install nodejs SDL2 glib2 pixman zlib  # Fedora
# or
sudo apt install nodejs npm libsdl2-2.0-0 libglib2.0-0 libpixman-1-0 zlib1g libsndio7.0  # Ubuntu

uv tool install pebble-tool
pebble sdk install latest

# Build this project
git clone git@github.com:s256/pebble-musicassistant-remote.git
cd pebble-musicassistant-remote
pebble build

# Run in the emulator
pebble install --emulator emery --logs

# Sideload via the phone app
pebble login            # GitHub auth
pebble install --cloudpebble
```

The compiled bundle lands at `build/pebble-musicassistant.pbw`.

---

## Configure

After install, open **Pebble app → My Watches → Music Assistant → Settings**. You'll get the hosted settings page.

Fields:

| Field        | Example                  | Notes                                                            |
| ------------ | ------------------------ | ---------------------------------------------------------------- |
| **Host**     | `http://192.168.80.10:8095` | Music Assistant base URL. Use `https://` if your server has TLS. |
| **Username** | `alice`                  | Built-in MA account.                                             |
| **Password** | `••••••••`               | Stored locally on the phone — never on the watch.                |

> Running MA as a **Home Assistant add-on**? Point Host to the add-on's port (default `8095`) on your HA host, e.g. `http://homeassistant.local:8095`. No `/api/hassio_ingress/...` prefix needed — the add-on exposes the same API.

---

## Controls

| Action                            | Touch (Time 2)                  | Buttons                         |
| --------------------------------- | ------------------------------- | ------------------------------- |
| Play / pause                      | Tap centre transport icon       | SELECT (short)                  |
| Next track                        | Tap right transport icon        | —                               |
| Previous track                    | Tap left transport icon         | —                               |
| Volume up                         | Tap volume + (or drag, post-MVP) | UP (short)                      |
| Volume down                       | Tap volume −                    | DOWN (short)                    |
| Mute toggle                       | Tap volume readout              | UP long-press                   |
| Shuffle toggle                    | Tap shuffle glyph               | —                               |
| Repeat cycle (off → all → one)    | Tap repeat glyph                | —                               |
| Open player list                  | **Tap status bar**              | —                               |
| Pick a player                     | Tap the player row              | UP/DOWN to scroll, SELECT       |

---

## Project layout

```
.
├── CONCEPT.md          # architecture, roadmap, design rationale
├── CHANGELOG.md        # human-readable per-version notes
├── README.md           # this file
├── LICENSE             # MIT
├── package.json        # Pebble app manifest (UUID, targets, message keys)
├── wscript             # waf build orchestration
├── src/
│   ├── c/main.c        # native watchapp — windows, layers, touch, AppMessage
│   └── pkjs/index.js   # PebbleKit JS — REST client, auth, polling, settings bridge
└── docs/               # GitHub Pages source for the settings page
    └── index.html
```

---

## License

MIT — see [LICENSE](./LICENSE).
