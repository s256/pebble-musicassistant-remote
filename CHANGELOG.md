# Changelog

All notable changes to this project follow [Keep a Changelog](https://keepachangelog.com/) and adhere to [Semantic Versioning](https://semver.org/).

## [1.2.0] — 2026-06-22

Two now-playing screen improvements you asked for after a real-watch testing pass.

### Added

- **Marquee scrolling for the title and album lines.** When either text overflows the visible band, the line pauses 3 s at the start, sweeps right-to-left at 50 px/s, snaps back, and loops. Static when the text fits. Driven by a dedicated 25 fps timer that is gated by `app_focus_service` and the now-playing window's appear/disappear handlers, so it sleeps with the watch and pauses while a sub-window is on top.
- **Optimistic feedback on transport toggles.** Tapping play/pause, shuffle, or repeat now flips the on-screen icon the instant the touch is registered — no more 300 ms wait for the next phone poll to confirm. The next snapshot from the server overwrites if it disagrees, which is rare.

### Changed

- The second line of the now-playing screen now shows just the **album**. The artist still lives on line 1 (Music Assistant typically formats `current_item.name` as `Artist - Title`), so duplicating it on line 2 was wasted space.

### Internal

- New file-scope marquee state machine + helpers in `src/c/main.c`. Phase counter is a private monotonic ms accumulator — Pebble's `time_ms(NULL, NULL)` doesn't return a monotonic value the way the name suggests, which silently broke an earlier iteration.
- Font-key comparison in `marquee_set_text` is now by value (`strcmp`), not pointer — Pebble's font macros are string literals that the linker doesn't always merge, so the pointer compare was producing false "text changed" results and resetting scroll on every paint.
- The right-edge action column now paints a black mask over its band before drawing the rounded pills, so scrolling title text can't bleed into the gaps between the prev/play/next icons.

## [1.1.0] — 2026-06-22

Three deliverables: a bug fix you reported on the watch, a group-volume control, and Home Assistant authentication.

### Added

- **Two-row volume window for groups.** When the active control player is part of a sync-group, the volume window now shows two rows — group overall on top, this-player only underneath. Tap a row to focus it; the focused row gets a cerulean accent strip and brighter title. Hardware buttons always change the focused row's volume (`UP` / `DOWN` step ±5, `SELECT` toggles mute). Solo players keep the single-row volume window unchanged.
  - Group writes go via `players/cmd/group_volume` + `players/cmd/group_volume_mute` on the master.
  - Individual writes go via `players/cmd/volume_set` + `players/cmd/volume_mute` on the chosen player.
- **Home Assistant authentication.** The settings page now offers a radio between **Music Assistant** (today's username + password) and **Home Assistant** (OAuth via Music Assistant's `homeassistant` provider — only works when MA is running as the HA add-on). Both options are always visible; pick the one that fits your setup.
  - HA flow: clicking **Sign in with Home Assistant** calls `auth/authorization_url` on MA, redirects the browser to HA, lets MA's server-side `/auth/callback` exchange the code, then bounces back to the settings page with the bearer token ready to use.
  - Signed-in badge with display name + role + "Sign out" button after either auth path succeeds. Sign out revokes the token server-side (`auth/logout`) and clears the local state.
  - Token validation on app start via `auth/me`. Builtin mode silently re-auths on 401 using stored credentials. HA mode surfaces "Sign in expired — open Settings" and waits for the user.

### Fixed

- **Selecting a non-master member in the player list now actually controls that member.** Previously, picking Den (synced to Office) snapped the now-playing screen back to Office and you couldn't tweak Den's volume. The phone-side bridge now tracks two separate ids: `activeQueueId` (the master's queue, where playback state lives) and `controlPlayerId` (the user's literal pick, where volume / mute commands go). The now-playing header now shows the chosen member's name.

### Internal

- New AppMessage keys: `ST_CONTROL_PLAYER_ID`, `ST_GROUP_VOLUME`, `ST_GROUP_MUTED`. New commands `CMD_GROUP_VOLUME_UP = 16`, `CMD_GROUP_VOLUME_DOWN = 17`, `CMD_GROUP_MUTE_TOGGLE = 18`. Existing ids unchanged.
- Settings page schema gains `authMode: "builtin" | "homeassistant"` and `accessToken` / `user` blobs. Backwards compatible with v1.0 settings (defaults to `"builtin"`).

## [1.0.1] — 2026-06-22

Two grouping bugs reported from real-watch use.

### Fixed

- **Member rows now indent under their actual master** instead of whichever player happened to sort above them alphabetically. The player list is now built phone-side as a sequence of master-then-members clusters interleaved with solo players (clusters sort by master name; members inside a cluster sort by their own name). Previously a member could appear visually nested under an unrelated player two rows above.
- **Player list refreshes after a grouping change.** `CMD_GROUP` / `CMD_UNGROUP` / `CMD_UNGROUP_ALL` now trigger an explicit `pushPlayerList` (in addition to the now-playing repoll), with a 350 ms delay so Music Assistant has a beat to apply the change. Previously the watch popped back to a stale list and the user had to back out and re-enter to see the new topology.

## [1.0.0] — 2026-06-22

First stable release. Feature-complete for the v1 remit; the watchapp is daily-driver ready.

### Added

- **Menu launcher icon** — a 25 × 25 native-PNG speaker silhouette + sound waves, distinct from Pebble Music's note glyph and `pebble-ma`'s cassette. Embedded in the `.pbw` as a `menuIcon` published-media resource.
- **Multi-room grouping** (was 0.3.0):
  - Players list now renders group topology — master rows carry a `⛓ N` chain badge, members render indented under their master with a `↳ synced` subtitle, solo rows are unchanged.
  - Long-press a row (or `SELECT` long-press) opens an action sheet:
    - Solo player → "Join / Group with" rows for every compatible other player.
    - Group member → `⏏ Leave group`, `＋ Add to this group`, `⚠ Ungroup all`.
  - New AppMessage keys: `ARG_TARGET_PLAYER_ID`, `ARG_PLAYER_IDS`, `PLAYER_ROW_SYNCED_TO`, `PLAYER_ROW_GROUP_COUNT`, `PLAYER_ROW_FLAGS`. New commands `CMD_GROUP`, `CMD_UNGROUP`, `CMD_UNGROUP_ALL` → `players/cmd/group` · `players/cmd/ungroup` · `players/cmd/ungroup_many`.

### Fixed

- **Unicode rendering on the watch** — track names containing smart quotes (`’`), em-dashes (`—`), unicode hyphens (`‐`), ellipses (`…`) and similar punctuation were rendering as tofu boxes because Pebble's bundled Gothic fonts only cover ASCII + Latin-1. The phone-side bridge now transliterates the common punctuation Unicode block to ASCII equivalents before sending strings to the watch, and falls back to `?` for anything else outside Latin-1.
- **Blank-screen / watchdog kill after extended inactivity** — the 1 Hz interpolation tick was holding the app awake during screen-off, eventually tripping the system watchdog and killing the app silently. Now gated by `app_focus_service` (paused when the screen blanks) and by window `.appear` / `.disappear` (paused when a sub-window is on top of now-playing).

### Repo hygiene

- **`scripts/make-publish-pbw.sh`** — strips the bundled JS source map from the build output for upload. The source map embeds the SDK install path of whoever built the artifact (i.e. a username); the map isn't needed at runtime. The release asset name is `pebble-musicassistant-publish.pbw`.
- **CONCEPT.md and HANDOFF.md** kept untracked (already excluded in `.gitignore`).

### Stubbed for a future release

- `＋ Add to this group` in the group action sheet currently logs and pops — it needs a sub-sheet drilling into ungrouped candidates that calls `players/cmd/set_members`. Captured in `src/c/main.c` next to the call site.

## [0.3.0] — 2026-06-22

Working draft toward the v1 grouping feature. Folded into `1.0.0` for the first stable release — same code, just an additional version stamp during development. See above.



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

[1.2.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v1.2.0
[1.1.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v1.1.0
[1.0.1]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v1.0.1
[1.0.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v1.0.0
[0.3.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v1.0.0
[0.2.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v0.2.0
[0.1.1]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v0.1.1
[0.1.0]: https://github.com/s256/pebble-musicassistant-remote/releases/tag/v0.1.0
