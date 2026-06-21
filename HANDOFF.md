# Session Handoff — Pebble Music Assistant Remote

> Resume with `/start-work` in the next session.
> Date paused: 2026-06-22. Branch: `main`. Last shipped tag: `v0.1.1`.

---

## 1. Current state

### Where we are

- **v0.1.0** — initial bootstrap. Committed, tagged, GitHub Release with `.pbw` attached.
- **v0.1.1** — touch + icon polish. Tofu glyphs fixed (native primitives), status bar enlarged. **Verified working on the user's real Pebble Time 2.** Tagged and released. This is the **revert target** if v0.2.x regresses.
- **v0.2.0 work-in-progress** — currently uncommitted in the working tree. UI experiment that the user partially rejected; we then redesigned around **proper Pebble idiom** (ActionBarLayer + bottom status strip + long-press Quick Play). **The redesign is documented in `CONCEPT.md` §4 but not yet implemented in code.**

### What's complete

- v0.1.0 bootstrap (repo, README, CONCEPT, CHANGELOG, LICENSE, GH Pages, settings page).
- Working build pipeline (`pebble build`, emulator workflow with `LD_LIBRARY_PATH=/usr/local/lib`).
- Music Assistant REST integration with built-in auth (JWT bearer via `/auth/login`, then `POST /api` with `{command, args}`).
- Active-player auto-selection (playing > paused > anything, alphabetical tiebreak).
- Player list screen, volume screen, transport, shuffle, repeat, volume up/down, mute.
- v0.1.1 icon fix (native-drawn triangles/rects/arcs instead of Unicode glyphs that Pebble's Gothic fonts can't render).
- Player list readability fix (white-on-black, not the previous black-on-black bug).
- **CONCEPT.md updated** to capture the v0.2.0 design the user signed off on.

### What's remaining (v0.2.0 implementation)

1. **Now-playing rebuild around `ActionBarLayer`** — right-edge icon column bound to UP/SELECT/DOWN, with the icons also being touch targets on emery.
2. **Bottom status strip** — generous height (≥36 px, the user explicitly asked for this; the previous version was a 16 px sliver), showing shuffle ON/OFF + repeat OFF/ALL/ONE + speaker icon + volume %. **All three zones tap to act immediately** (not navigate to a sub-screen).
3. **Long-press SELECT on now-playing → Quick Play menu** — MenuLayer of up-to-10 user-defined shortcuts. UP/DOWN scroll, SELECT or tap = `player_queues/play_media` on active player.
4. **Quick Play AppMessage protocol** — new keys: `QUICK_BEGIN`, `QUICK_END`, `QUICK_ROW_INDEX`, `QUICK_ROW_LABEL`, `QUICK_ROW_URI`, plus a new command `CMD_QUICK_PLAY` that takes the URI as `ARG_STR`. PebbleKit JS pushes the slot list on launch and on settings save; the watch caches it in `persist_*` for offline display.
5. **Settings page Discover panel** — after host/credentials validate, the page authenticates against MA itself, exposes a debounced search box with filter chips (Album · Artist · Playlist), tap-to-add results, reorderable up-to-10 slots. Page returns the whole settings + slot list via `pebblejs://close#…` as today.

### Uncommitted changes in the working tree

```
 M CONCEPT.md          ← v0.2.0 design locked in (KEEP)
 M package.json        ← bumped to 0.2.0 (KEEP)
 M src/c/main.c        ← partial v0.2.0 UI experiments (REVERT OR FINISH)
```

### Decision needed at session start

`src/c/main.c` contains UI experiments the user rejected midway: two bottom chips with floating mid-screen shuffle/repeat pills, then matching-pill style. The user's final direction (ActionBarLayer + bottom strip + long-press Quick Play) hasn't been written yet. **Best path: `git checkout -- src/c/main.c` to revert to v0.1.1, then implement the new design fresh.** That's cleaner than picking through the partial state.

Keep `CONCEPT.md` and `package.json` changes — they reflect the agreed direction.

---

## 2. Architecture context

### Repo layout

```
.
├── CONCEPT.md            architecture + roadmap. §4 has the v0.2.0 UI spec, §4.5 has Quick Play
├── CHANGELOG.md          v0.1.0, v0.1.1; "Filed for 0.2.0" section captures security/perf followups
├── README.md             user-facing docs
├── docs/index.html       GitHub Pages settings page (already live at https://s256.github.io/pebble-musicassistant-remote/)
├── LICENSE               MIT
├── .gitignore
├── package.json          Pebble manifest. targetPlatforms=["emery"]. UUID generated, do not change.
├── wscript               waf build
└── src/
    ├── c/main.c          watchapp (~750 lines as of v0.1.1)
    └── pkjs/index.js     PebbleKit JS — REST client, settings handler, AppMessage bridge
```

### Key patterns

- **Phone-side REST, not on-watch WebSocket.** Confirmed by deep-reading the prior-art `vincentezw/pebble-ma`: on-watch WS is impractical (memory). Phone polls `player_queues/all` + `players/all` every 2s (playing) / 5s (idle), pushes deltas via AppMessage.
- **AppMessage envelope is the bottleneck.** Messages are queued in `src/pkjs/index.js` via `enqueue()` + `pump()`. Send queue is FIFO; player-list flood (BEGIN + N rows + END) shares the queue with now-playing updates. (Filed for 0.2.0 as iridium-flagged perf issue — split into priority lanes.)
- **Active-player logic in `pickActiveQueue()`** in `src/pkjs/index.js`. Override (`overridePlayerId`) wins; else playing > paused > available, alphabetical tiebreak.
- **Token cached in localStorage**, re-auth on 401 inside `maCall()`.
- **All UI primitives drawn natively** — no bitmap resources. Pebble's Gothic fonts lack Unicode media glyphs (⏮ ⏸ ⏭ 🔀 ↻) so we draw icons from triangles/rects/arcs. This is the v0.1.1 fix; keep this approach.
- **Settings page → watch** via `pebblejs://close#` + `JSON.stringify` + `encodeURIComponent`. PebbleKit JS `webviewclosed` listener parses and merges (empty password keeps the previous one).
- **Touch is raw `TouchService`** (Touchdown / PositionUpdate / Liftoff). We classify tap vs drag in software with `TAP_SLOP_PX=12` and `TAP_MAX_MS=450`. No SDK gesture API exists; this is correct.

### MA API quick reference (verified against `http://192.168.0.206:8095/api-docs/openapi.json`)

- `POST /auth/login` with `{provider_id:"builtin", credentials:{username,password}}` → `{token}` (JWT bearer).
- `POST /api` with `{message_id, command, args}` + `Authorization: Bearer <token>`.
- Commands used: `players/all`, `player_queues/all`, `player_queues/play_pause`, `player_queues/next`, `player_queues/previous`, `player_queues/shuffle` (`{queue_id, shuffle_enabled}`), `player_queues/repeat` (`{queue_id, repeat_mode}`), `players/cmd/volume_set` (`{player_id, volume_level: 0..100}`), `players/cmd/volume_mute` (`{player_id, muted}`).
- For Quick Play: `player_queues/play_media` with `{queue_id, media: <uri>, option: "replace"}`.
- For library search (Discover panel): TBD — likely `music/search` with filter, or per-type endpoints (`music/albums/search`, `music/artists/search`, `music/playlists/search`). **Confirm against the OpenAPI spec at session start** (it's at `http://192.168.0.206:8095/api-docs/openapi.json`).

### MA enums (from OpenAPI)

- `PlaybackState`: `idle`, `paused`, `playing`, `unknown`.
- `RepeatMode`: `off`, `one`, `all`, `unknown`.
- `Player.volume_level`: integer 0..100, nullable.
- `PlayerQueue.shuffle_enabled`: boolean.

---

## 3. Outstanding items

### Open todos (high-level)

- [ ] Implement v0.2.0 now-playing rebuild (`ActionBarLayer` + bottom strip + long-press Quick Play).
- [ ] Implement Quick Play window (MenuLayer over `s_quick_slots[10]`).
- [ ] Implement Discover panel in `docs/index.html` (settings page does its own MA auth, calls library search, builds slot list).
- [ ] Wire Quick Play AppMessage keys both sides + `CMD_QUICK_PLAY` → `player_queues/play_media`.
- [ ] Persist slots on the watch via `persist_*` so the menu works before the first phone sync after launch.
- [ ] Verify in emery emulator + on the user's real watch.
- [ ] Tag `v0.2.0`, release with `.pbw`.

### Known issues / blockers

- **None blocking.** v0.1.1 is working on the user's watch.
- v0.2.0 partial code in `src/c/main.c` is messy; revert per §1 above.

### Decisions already made (do not re-litigate)

1. Action bar order: **prev (UP) / play-pause (SELECT) / next (DOWN)** — confirmed.
2. Bottom strip taps act **immediately** (no sub-screen). Tap shuffle = toggle now. Tap repeat = cycle now. Tap volume cluster = open volume window.
3. Quick Play entry = **long-press SELECT** on now-playing. (Not a header tab. Not a swipe gesture.)
4. Settings page **Discover** appears once host + credentials work; user does NOT type raw URIs.
5. **No persistent bottom dock / tab bar** — Pebble idiom is Back-button navigation. Don't add Android-style chrome.

### Decisions still open

- Exact MA library-search endpoint for the Discover panel (probably `music/search`, but verify via the OpenAPI dump at session start before coding).
- Maximum URI length for Quick Play slots — current guess 120 chars; confirm AppMessage outbox max is OK with up to ~10 × 160-byte rows during the slot push.
- Bottom-strip visual style: still pill-shape per `ActionBarLayer` aesthetic? Or flatter strip with separator dots? **My recommendation: flatter strip, not pills, so it reads as state-display, not buttons.** Confirm with user when implementation starts.

---

## 4. How to continue (next session)

1. **Read `CONCEPT.md` §4 and §4.5 first.** That's the agreed v0.2.0 design.
2. **Read this file (`HANDOFF.md`) §3 for open decisions.**
3. **Reset the partial UI experiment**:
   ```
   git checkout -- src/c/main.c
   ```
   This brings the C source back to v0.1.1. Keep `package.json` (0.2.0 bump) and `CONCEPT.md` (locked-in design).
4. **Re-fetch the MA OpenAPI spec** to confirm library-search endpoints:
   ```
   curl -sS http://192.168.0.206:8095/api-docs/openapi.json | jq '.paths | keys'
   curl -sS http://192.168.0.206:8095/api-docs/openapi.json > /tmp/ma-spec/openapi.json
   ```
5. **Implement in this order** (each is a self-contained step; build + emulator screenshot between each):
   1. Strip the floating-pill shuffle/repeat row.
   2. Add `ActionBarLayer` for prev / play-pause / next. Verify hardware buttons still work and touch on the icons mirrors them.
   3. Build the bottom status strip (≥ 36 px tall — user was explicit). Three zones: shuffle state, repeat state, volume cluster. Each tap acts immediately or opens volume window.
   4. Add Quick Play window (`MenuLayer`). Wire long-press SELECT → `window_stack_push`.
   5. Wire AppMessage keys + `CMD_QUICK_PLAY`. Bench-test with `--int 10000=<cmd> --string ...` from CLI.
   6. Extend `docs/index.html` with the Discover panel. Validate auth flow against the live MA server.
   7. PebbleKit JS: push slot list to watch on `ready` and on `webviewclosed`. Implement `player_queues/play_media` dispatch.
   8. Persist slots on the watch (`persist_write_string` per slot).
   9. Verify on emulator, then on real watch.
   10. Commit, push, tag `v0.2.0`, release with `.pbw`.
6. **Show the user a screenshot** at each milestone — they iterate visually.

### Commands you'll need

```bash
# Build
/home/snoe/.local/bin/pebble build

# Emulator (libbz2 quirk — see Gotchas)
LD_LIBRARY_PATH=/usr/local/lib /home/snoe/.local/bin/pebble install --emulator emery
LD_LIBRARY_PATH=/usr/local/lib /home/snoe/.local/bin/pebble screenshot --emulator emery /tmp/shot.png

# Kill stuck emulator if needed
pkill -9 qemu-pebble; pkill -9 pypkjs

# MA API quick check
curl -sS http://192.168.0.206:8095/api-docs/openapi.json | jq '.paths | keys'

# Build artefact lives at:
build/pebble-musicassistant.pbw
```

---

## 5. Gotchas (so we don't repeat mistakes)

1. **Unicode glyphs render as tofu boxes.** Pebble's Gothic fonts don't ship media-control code points (`⏮ ⏸ ⏭ 🔀 ↻ 🔊`). v0.1.0 used them and they were unreadable on hardware. **Always draw icons natively via `graphics_fill_rect` / `graphics_draw_line` / `fill_triangle()` helper.** All the icons we need are already implemented in `src/c/main.c` (`icon_play`, `icon_pause`, `icon_prev`, `icon_next`, `icon_shuffle`, `icon_repeat`, `icon_speaker`) — reuse them.
2. **Emulator needs `libbz2.so.1.0` symlink.** Fedora ships `libbz2.so.1.0.8` but not `.1.0`. Use `LD_LIBRARY_PATH=/usr/local/lib` prefix (symlink already exists there). Without this, `qemu-pebble` fails silently and `pebble install --emulator` times out.
3. **`pebble send-app-message` only sends from phone-side context.** With no `pypkjs` running, AppMessages sent via CLI hit the watch but the JS event listener treats them as `appmessage` events and routes them as watch→phone commands. For mocking now-playing state in the emulator, easier to just authenticate against a real MA instance.
4. **Clang/clangd LSP shows phantom errors** about `pebble.h` and unknown types — they appear as IDE diagnostics but `pebble build` (which uses `arm-none-eabi-gcc`) compiles fine. **Ignore the LSP diagnostics; trust the actual build.**
5. **Branch guard hook is disabled for this repo** (`.claude/branch-guard.off`). User explicitly allowed direct pushes to `main` until 1.0.0.
6. **Don't use `git add -A` or `git add .`** — global rule in `~/.claude/CLAUDE.md`. Always stage specific files. Especially because `build/`, `.claude/`, `CLAUDE.md` must never enter git (see `.gitignore`).
7. **`tap` is the only gesture we trust.** Drag/swipe classification is in `CONCEPT.md` §5 as 0.2.0+ post-MVP. Don't try to add gestures during the v0.2.0 rebuild — keep it tap-only.
8. **No bottom dock / tab bar.** This was the user's explicit correction: Pebble apps use Back-button navigation, not persistent navigation chrome.
9. **`vincentezw/pebble-ma`** is the reference impl but it uses Moddable SDK + on-watch JS + WebSocket. We deliberately chose native C + REST. Don't be tempted to copy its architecture.
10. **GitHub Pages site is live** at `https://s256.github.io/pebble-musicassistant-remote/` — extend `docs/index.html` carefully; mistakes are immediately public. Use a `?preview=1` branch query or test locally first.
11. **The user has an MA instance at `http://192.168.0.206:8095`** with a built-in user account. The OpenAPI spec there is the ground truth — fetch it on every session, don't trust cached schemas.
12. **The user prefers iterative visual checks.** Show screenshots after each meaningful UI change rather than batch-implementing then revealing.

---

## Working state summary

| File | State | Action |
|------|-------|--------|
| `CONCEPT.md` | Modified (v0.2.0 design locked in) | **Keep** |
| `package.json` | Modified (bumped 0.1.0 → 0.2.0) | **Keep** |
| `src/c/main.c` | Modified (partial mid-redesign experiments) | **Revert with `git checkout -- src/c/main.c`** |
| `src/pkjs/index.js` | Unchanged from v0.1.1 | — |
| `docs/index.html` | Unchanged from v0.1.1 | Extend in step 6 above |

When resuming, the first thing to do is reset `src/c/main.c` so we have a clean base to rebuild from. Everything we need to do next is in `CONCEPT.md` §4–§4.5.
