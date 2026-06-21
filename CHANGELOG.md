# Changelog

All notable changes to this project follow [Keep a Changelog](https://keepachangelog.com/) and adhere to [Semantic Versioning](https://semver.org/).

## [0.1.0] — 2026-06-21

Initial bootstrap.

### Added

- Native C watchapp targeting Pebble Time 2 (`emery`).
- PebbleKit JS client speaking Music Assistant REST (`POST /api`) with built-in auth (`POST /auth/login`).
- Now-playing screen: title / artist / album, playback state, progress bar, status bar showing player name and volume.
- Player list screen reached by tapping the status bar; rows are touch targets.
- Transport controls: play, pause, next, previous.
- Volume: up, down, mute.
- Shuffle toggle and repeat cycle (off → all → one) at the queue level.
- Active-player auto-selection: prefers `playing`, then `paused`, alphabetical tiebreak.
- Touch input via Pebble `TouchService` plus full Pebble button parity.
- Hosted settings page (`docs/index.html`) served via GitHub Pages.
- Token caching with re-auth on 401.

### Known limitations

- No album art yet — bandwidth budget and Time 2 palette work still outstanding.
- No on-watch queue browsing; the watch is a remote, not a player.
- No volume-drag gesture yet (planned for 0.2.0).
- WebSocket / push events not used; the phone polls every 2–5 s depending on state.

[0.1.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v0.1.0
