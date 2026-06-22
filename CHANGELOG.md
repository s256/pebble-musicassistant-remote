# Changelog

All notable changes to this project follow [Keep a Changelog](https://keepachangelog.com/) and adhere to [Semantic Versioning](https://semver.org/).

## [0.2.0] — 2026-06-22

UX rebuild around Pebble idiom + Quick Play shortcuts.

### Added

- **Right-edge action column** on now-playing — prev / play-pause (cerulean primary) / next. Hand-rolled custom layer (not `ActionBarLayer` — that doesn't support touch on emery and requires bitmap resources). Bound to UP / SELECT / DOWN hardware buttons AND tap-targets on Time 2.
- **Bottom status strip** showing shuffle ON/OFF, repeat OFF/ALL/ONE, speaker + volume %. Tap on shuffle → toggle immediately. Tap on repeat → cycle immediately. Tap on volume cluster → opens volume window.
- **Volume window** — dedicated full-screen control with a large percent readout and chunky bar. UP/DOWN step volume (repeating), SELECT toggles mute, Back returns. No touch on this screen (intentional, per design discussion).
- **Quick Play** — long-press SELECT on now-playing opens a menu of up-to-10 user-curated shortcuts. Tap a row to start playback on the active player via `player_queues/play_media` with `option: "replace"`. Empty-state row prompts the user to add shortcuts in settings.
- Slot persistence on the watch (`persist_write_string` per slot label + URI) so the Quick Play menu works before the first phone sync.
- New AppMessage keys: `QUICK_BEGIN`, `QUICK_END`, `QUICK_ROW_INDEX`, `QUICK_ROW_LABEL`, `QUICK_ROW_URI`. New command `CMD_QUICK_PLAY = 12`.
- **Discover panel** on the settings page — once host + credentials are entered and validated (Test Connection button does its own `POST /auth/login`), the page exposes a debounced search input with Album / Artist / Playlist filter chips, calling `music/search` directly against MA. Tap a result → adds it to the Quick Play slot list. Up to 10 slots with inline label edit, reorder, delete.
- Settings page handles the pypkjs emulator `return_to=<localhost>/close` redirect so it works in `pebble emu-app-config` as well as on the real phone.

### Fixed

- Settings page was reading `body.result` for the search response; Music Assistant returns the raw `SearchResults` dict directly at the top level. Search now matches what's actually in your library.
- Player list rendering had regressed back to `menu_cell_basic_draw` (cramped 36 px rows, packed text). Restored the v0.1.1 design: 44 px rows, explicit 1 px separators, coloured state dot (green / amber / grey) on the left, two clearly-spaced text lines (bold white name, light-grey state label), white-on-cerulean highlight.

### Behaviour notes

- Volume on the now-playing window is no longer on UP/DOWN — those are now transport (prev / next) to match Pebble Music idiom. To change volume, tap the volume cluster in the strip → opens the volume window.
- Header now shows the active player name + chevron; tap to open the player list (replaces the v0.1.1 status-bar tap).

### Internal

- `CONCEPT.md` and `HANDOFF.md` moved to local-only (untracked) — they're working notes, not project docs.
- Build size on emery: 65 KB `.pbw`, ~10 KB RAM, ~121 KB heap free.

## [0.1.1] — 2026-06-22

Touch + icon polish on top of v0.1.0.

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

[0.2.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v0.2.0
[0.1.1]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v0.1.1
[0.1.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v0.1.0
