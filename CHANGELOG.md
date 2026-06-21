# Changelog

All notable changes to this project follow [Keep a Changelog](https://keepachangelog.com/) and adhere to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed

- Transport / shuffle / repeat / speaker icons were rendering as tofu boxes — Pebble's bundled Gothic fonts don't ship the unicode media glyphs we'd used. Replaced them with native-primitive drawings (triangles, rects, arcs).
- Status bar was uncomfortably small for a touch target. Doubled its height to 44 px, made the whole bar tappable (was previously split between bar and volume readout), and added a chevron affordance on the right edge to show it's interactive.
- Shuffle / repeat hit boxes enlarged to match the new 32 px icons and given a subtle border so the inactive state is still visibly tappable.

### Docs

- Trimmed README — removed boilerplate sections.
- Example IP updated to `192.168.80.10` across README and settings page.

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

### Filed for 0.2.0 (post-review)

Security (sentinel review, all non-blocking):

- Warn (don't block) when settings host uses `http://` — credentials and bearer token currently travel in cleartext on plain-HTTP LANs.
- Add CSP `<meta>` to `docs/index.html` for defence-in-depth.
- Re-validate `merged.host` scheme in `webviewclosed` against `^https?://`.
- Serialize `login()` to avoid concurrent re-auth races on parallel 401s.
- Sanitize `pushError` messages before surfacing them on the watch.
- Drop the `host` / `user` query-string prefill on the settings page — keeps them out of phone-browser history.

Performance (iridium review, all post-ship polish; no correctness impact):

- Batch player-list AppMessage chunks (1+16+1 → ~5 messages) or add priority lanes so transport taps don't queue behind a list flood (≈1.8 s worst-case interactive lag).
- Coalesce `player_queues/all` + `players/all` — cache `players/all` with a longer TTL than `player_queues/all`.
- Cache the active player for ~5 s inside `withActive()` to drop the pre-action fetch on each transport tap (cuts a button-press from 6 → ~3 HTTP requests).
- Cancel the 1 Hz interpolation timer while paused / idle.
- De-duplicate `menu_layer_set_selected_index()` calls during drag in the player list.
- Rename / parameterise `clamp32` (it actually clamps to 60).

[0.1.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v0.1.0
