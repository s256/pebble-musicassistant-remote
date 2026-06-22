/*
 * pebble-musicassistant-remote — v0.3.0
 *
 * Native Pebble Time 2 (emery) watchapp. All Music Assistant traffic happens
 * on the phone (src/pkjs/index.js); we just render state and forward user
 * intent over AppMessage.
 *
 * Windows on the stack:
 *
 *   1. Now-playing — default. Header (tap → player list), title/meta/progress,
 *      right-edge action column (prev / play-pause / next), bottom status
 *      strip (shuffle / repeat / volume).
 *   2. Player list — touch-driven row picker (MenuLayer + raw TouchService).
 *   3. Volume — UP/DOWN steps volume, SELECT mutes, Back returns.
 *   4. Quick Play — MenuLayer of up-to-10 user-defined shortcuts; opened on
 *      long-press SELECT from now-playing.
 */

#include <pebble.h>

// ─── Logging ────────────────────────────────────────────────────────────────

#define LOG_TAG "ma"
#define LOGI(fmt, ...) APP_LOG(APP_LOG_LEVEL_INFO,  "[" LOG_TAG "] " fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) APP_LOG(APP_LOG_LEVEL_ERROR, "[" LOG_TAG "] " fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) APP_LOG(APP_LOG_LEVEL_DEBUG, "[" LOG_TAG "] " fmt, ##__VA_ARGS__)

// ─── Layout (emery: 200×228) ────────────────────────────────────────────────

#define SCREEN_W 200
#define SCREEN_H 228

#define HEADER_H     28
#define HEADER_Y     0

#define STRIP_H      40
#define STRIP_Y      (SCREEN_H - STRIP_H)

#define ACTION_W     34
#define ACTION_X     (SCREEN_W - ACTION_W)
#define ACTION_Y     HEADER_H
#define ACTION_H     (STRIP_Y - ACTION_Y)
#define ACTION_PILL_H ((ACTION_H) / 3)

#define CONTENT_X    0
#define CONTENT_W    (SCREEN_W - ACTION_W)
#define CONTENT_Y    HEADER_H
#define CONTENT_H    (STRIP_Y - CONTENT_Y)

#define TITLE_TOP    (CONTENT_Y + 4)
#define TITLE_H      30
#define META_TOP     (TITLE_TOP + TITLE_H + 2)
#define META_H       20
#define PROGRESS_TOP (STRIP_Y - 36)
#define PROGRESS_BAR_H 4
#define TIME_TOP     (PROGRESS_TOP + 8)

#define STRIP_ZONE_W ((SCREEN_W) / 3)

// ─── Touch gesture state ───────────────────────────────────────────────────

#define TAP_SLOP_PX 12
#define TAP_MAX_MS  450

typedef enum {
  TG_IDLE,
  TG_TRACKING,
} TouchPhase;

typedef struct {
  TouchPhase phase;
  int16_t    start_x, start_y;
  int16_t    last_x,  last_y;
  uint32_t   start_ms;
} TouchState;

static TouchState s_touch;

// ─── Player + playback state mirrored from phone ──────────────────────────

typedef enum {
  PS_UNKNOWN = 0,
  PS_IDLE,
  PS_PAUSED,
  PS_PLAYING,
} PlaybackState;

typedef enum {
  RM_OFF = 0,
  RM_ONE,
  RM_ALL,
} RepeatMode;

#define MAX_PLAYER_NAME 32
#define MAX_TRACK_TEXT  64
#define MAX_PLAYERS     16
#define MAX_PLAYER_ID   48

// Row flag bits — must match index.js ROW_FLAG_*.
#define ROW_FLAG_MASTER 0x01
#define ROW_FLAG_MEMBER 0x02

#define MAX_QUICK_SLOTS  10
#define MAX_QUICK_LABEL  41
#define MAX_QUICK_URI    121

// Persist keys for slots. Even index = label, odd = URI.
#define PERSIST_QUICK_BASE 0x100
#define PERSIST_QUICK_COUNT 0x0FF

typedef struct {
  char          id[MAX_PLAYER_ID];
  char          name[MAX_PLAYER_NAME];
  PlaybackState state;
  char          synced_to[MAX_PLAYER_ID]; // master player_id, "" if not synced
  uint8_t       group_count;              // members in our group (0 if not master)
  uint8_t       flags;                    // ROW_FLAG_* bitmask
} PlayerRow;

typedef struct {
  char label[MAX_QUICK_LABEL];
  char uri[MAX_QUICK_URI];
} QuickSlot;

static struct {
  char          player_name[MAX_PLAYER_NAME];
  char          title[MAX_TRACK_TEXT];
  char          artist[MAX_TRACK_TEXT];
  char          album[MAX_TRACK_TEXT];
  PlaybackState state;
  uint8_t       volume;
  bool          muted;
  bool          shuffle;
  RepeatMode    repeat;
  uint32_t      elapsed_s;
  uint32_t      duration_s;
  bool          connected;
  char          last_error[MAX_TRACK_TEXT];
  // v1.1: control player + group volume mirror.  control_player_id is
  // typed wide enough for any MA player_id (RINCON_* tokens are ~17 chars
  // but MA accepts arbitrary strings).  group_volume == -1 sentinel means
  // "not in a group" — the volume window falls back to its single-row layout.
  char          control_player_id[MAX_PLAYER_ID];
  int16_t       group_volume;
  bool          group_muted;
} s_now = {
  .player_name = "—",
  .title       = "Not connected",
  .state       = PS_UNKNOWN,
  .group_volume = -1,
};

static AppTimer *s_tick_timer;
#define TICK_MS 1000

static PlayerRow s_players[MAX_PLAYERS];
static int       s_players_count;

static QuickSlot s_quick_slots[MAX_QUICK_SLOTS];
static int       s_quick_count;
static int       s_quick_recv_count;

// ─── Outbound command codes (mirrors src/pkjs/index.js CMD.*) ─────────────

enum {
  CMD_REQUEST_STATE   = 1,
  CMD_REQUEST_PLAYERS = 2,
  CMD_SELECT_PLAYER   = 3,
  CMD_PLAY_PAUSE      = 4,
  CMD_NEXT            = 5,
  CMD_PREVIOUS        = 6,
  CMD_VOLUME_UP       = 7,
  CMD_VOLUME_DOWN     = 8,
  CMD_MUTE_TOGGLE     = 9,
  CMD_SHUFFLE_TOGGLE  = 10,
  CMD_REPEAT_CYCLE    = 11,
  CMD_QUICK_PLAY      = 12,
  CMD_GROUP           = 13,
  CMD_UNGROUP         = 14,
  CMD_UNGROUP_ALL     = 15,
  CMD_GROUP_VOLUME_UP   = 16,
  CMD_GROUP_VOLUME_DOWN = 17,
  CMD_GROUP_MUTE_TOGGLE = 18,
  // Emulator-only: open the volume window directly so screenshot capture
  // doesn't need touch simulation (no `emu-touch` exists).  Never sent by
  // the JS bridge in real-watch use — same gate as CMD_DEBUG_SEED.
  CMD_DEBUG_OPEN_VOL  = 98,
  // Emulator-only: populate s_players with a canonical mock group setup.
  CMD_DEBUG_SEED      = 99,
};

// ─── Windows ──────────────────────────────────────────────────────────────

static Window     *s_now_window;
static Layer      *s_now_root_layer;
static Window     *s_players_window;
static MenuLayer  *s_players_menu;
static Window     *s_volume_window;
static Layer      *s_volume_root_layer;
static Window     *s_quick_window;
static MenuLayer  *s_quick_menu;
static Window     *s_groupsheet_window;
static MenuLayer  *s_groupsheet_menu;

// The player the action sheet operates on — captured at long-press time.
// Index into s_players[]; -1 = no sheet open.
static int        s_groupsheet_target = -1;

// Long-press detection for the players list — tracks finger position so we can
// resolve the row under the finger at fire time.
#define LONG_PRESS_MS  500
static AppTimer  *s_pl_long_timer;
static int16_t   s_pl_long_x, s_pl_long_y;

// ─── Icon drawing (native primitives) ─────────────────────────────────────

static void fill_triangle(GContext *ctx, GPoint a, GPoint b, GPoint c) {
  GPathInfo info = (GPathInfo){ .num_points = 3, .points = (GPoint[]){a, b, c} };
  GPath *path = gpath_create(&info);
  gpath_draw_filled(ctx, path);
  gpath_destroy(path);
}

static void icon_play(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  int s  = 12;
  fill_triangle(ctx,
                GPoint(cx - s/2, cy - s),
                GPoint(cx - s/2, cy + s),
                GPoint(cx + s,   cy));
}

static void icon_pause(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  graphics_fill_rect(ctx, GRect(cx - 7, cy - 10, 4, 20), 1, GCornersAll);
  graphics_fill_rect(ctx, GRect(cx + 3, cy - 10, 4, 20), 1, GCornersAll);
}

static void icon_next(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  int s  = 8;
  fill_triangle(ctx, GPoint(cx - 10, cy - s), GPoint(cx - 10, cy + s), GPoint(cx - 2, cy));
  fill_triangle(ctx, GPoint(cx - 2,  cy - s), GPoint(cx - 2,  cy + s), GPoint(cx + 6, cy));
  graphics_fill_rect(ctx, GRect(cx + 6, cy - s, 3, s * 2), 0, GCornerNone);
}

static void icon_prev(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  int s  = 8;
  graphics_fill_rect(ctx, GRect(cx - 11, cy - s, 3, s * 2), 0, GCornerNone);
  fill_triangle(ctx, GPoint(cx + 8, cy - s), GPoint(cx + 8, cy + s), GPoint(cx,     cy));
  fill_triangle(ctx, GPoint(cx,     cy - s), GPoint(cx,     cy + s), GPoint(cx - 8, cy));
}

static void icon_shuffle(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 2);
  int x = rect.origin.x + 2;
  int y = rect.origin.y + rect.size.h / 2;
  int w = rect.size.w - 4;
  graphics_draw_line(ctx, GPoint(x,        y - 5), GPoint(x + w/2,  y - 5));
  graphics_draw_line(ctx, GPoint(x + w/2,  y - 5), GPoint(x + w,    y + 5));
  graphics_draw_line(ctx, GPoint(x,        y + 5), GPoint(x + w/2,  y + 5));
  graphics_draw_line(ctx, GPoint(x + w/2,  y + 5), GPoint(x + w,    y - 5));
  graphics_context_set_fill_color(ctx, col);
  fill_triangle(ctx, GPoint(x + w, y + 5), GPoint(x + w - 5, y + 1), GPoint(x + w - 5, y + 9));
  fill_triangle(ctx, GPoint(x + w, y - 5), GPoint(x + w - 5, y - 1), GPoint(x + w - 5, y - 9));
}

static void icon_repeat(GContext *ctx, GRect rect, GColor col, RepeatMode mode) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 2);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  graphics_draw_line(ctx, GPoint(cx - 9, cy - 6), GPoint(cx + 7,  cy - 6));
  graphics_draw_line(ctx, GPoint(cx + 7, cy - 6), GPoint(cx + 7,  cy + 6));
  graphics_draw_line(ctx, GPoint(cx + 7, cy + 6), GPoint(cx - 9,  cy + 6));
  graphics_draw_line(ctx, GPoint(cx - 9, cy + 6), GPoint(cx - 9,  cy));
  graphics_context_set_fill_color(ctx, col);
  fill_triangle(ctx, GPoint(cx + 11, cy - 6), GPoint(cx + 5, cy - 10), GPoint(cx + 5, cy - 2));
  if (mode == RM_ONE) {
    graphics_context_set_text_color(ctx, col);
    graphics_draw_text(ctx, "1", fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(cx - 4, cy - 8, 8, 16),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
  }
}

static void icon_speaker(GContext *ctx, GRect rect, GColor col, bool muted) {
  graphics_context_set_fill_color(ctx, col);
  int x = rect.origin.x;
  int cy = rect.origin.y + rect.size.h / 2;
  graphics_fill_rect(ctx, GRect(x + 1, cy - 5, 4, 10), 0, GCornerNone);
  fill_triangle(ctx, GPoint(x + 5, cy - 5), GPoint(x + 5, cy + 5), GPoint(x + 12, cy + 8));
  fill_triangle(ctx, GPoint(x + 5, cy - 5), GPoint(x + 12, cy + 8), GPoint(x + 12, cy - 8));
  if (muted) {
    graphics_context_set_stroke_color(ctx, col);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(x + 14, cy - 6), GPoint(x + 22, cy + 6));
    graphics_draw_line(ctx, GPoint(x + 22, cy - 6), GPoint(x + 14, cy + 6));
  }
}

static void icon_chevron_right(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 2);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  graphics_draw_line(ctx, GPoint(cx - 3, cy - 5), GPoint(cx + 2, cy));
  graphics_draw_line(ctx, GPoint(cx + 2, cy),     GPoint(cx - 3, cy + 5));
}

// Two linked rounded rects — used as the "group" glyph in the chain badge.
// Width 12 px, height 8 px centred in `rect`.
static void icon_chain(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  // Left ring at cx-4, right ring at cx+1; they overlap in the middle by 2 px.
  graphics_draw_round_rect(ctx, GRect(cx - 7, cy - 3, 8, 6), 2);
  graphics_draw_round_rect(ctx, GRect(cx - 1, cy - 3, 8, 6), 2);
}

// Eject: an upward triangle with a horizontal bar underneath.
static void icon_eject(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  fill_triangle(ctx,
                GPoint(cx - 6, cy + 1),
                GPoint(cx + 6, cy + 1),
                GPoint(cx,     cy - 7));
  graphics_fill_rect(ctx, GRect(cx - 6, cy + 4, 12, 3), 1, GCornersAll);
}

// Warning: upward triangle outline with a vertical bar inside.  Drawn filled
// since Pebble's outlined polygon support is limited.
static void icon_warning(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  fill_triangle(ctx,
                GPoint(cx - 8, cy + 7),
                GPoint(cx + 8, cy + 7),
                GPoint(cx,     cy - 7));
  // Cut out an inner triangle to make a hollow ring effect.
  graphics_context_set_fill_color(ctx, GColorBlack);
  fill_triangle(ctx,
                GPoint(cx - 5, cy + 5),
                GPoint(cx + 5, cy + 5),
                GPoint(cx,     cy - 3));
  // Exclamation bar + dot, in the warn colour.
  graphics_context_set_fill_color(ctx, col);
  graphics_fill_rect(ctx, GRect(cx - 1, cy - 2, 2, 5), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx - 1, cy + 4, 2, 2), 0, GCornerNone);
}

// Plus: two crossed filled bars (a thick "+" sign).
static void icon_plus(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_fill_color(ctx, col);
  int cx = rect.origin.x + rect.size.w / 2;
  int cy = rect.origin.y + rect.size.h / 2;
  graphics_fill_rect(ctx, GRect(cx - 6, cy - 1, 12, 3), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(cx - 1, cy - 6, 3, 12), 0, GCornerNone);
}

// Down-right arrow ("↳") drawn as two thin strokes.  Used in the "synced"
// subtitle on member rows.
static void icon_subarrow(GContext *ctx, GRect rect, GColor col) {
  graphics_context_set_stroke_color(ctx, col);
  graphics_context_set_stroke_width(ctx, 1);
  int x = rect.origin.x;
  int y = rect.origin.y + rect.size.h / 2;
  graphics_draw_line(ctx, GPoint(x,     y - 4), GPoint(x,     y + 2));
  graphics_draw_line(ctx, GPoint(x,     y + 2), GPoint(x + 6, y + 2));
  graphics_context_set_fill_color(ctx, col);
  fill_triangle(ctx,
                GPoint(x + 6, y),
                GPoint(x + 6, y + 4),
                GPoint(x + 9, y + 2));
}

// ─── AppMessage out helpers ──────────────────────────────────────────────

static void send_simple_cmd(uint8_t cmd) {
  DictionaryIterator *iter;
  AppMessageResult r = app_message_outbox_begin(&iter);
  if (r != APP_MSG_OK) { LOGE("outbox_begin: %d", r); return; }
  dict_write_uint8(iter, MESSAGE_KEY_CMD, cmd);
  r = app_message_outbox_send();
  if (r != APP_MSG_OK) LOGE("outbox_send: %d", r);
}

static void send_select_player(const char *player_id) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_CMD, CMD_SELECT_PLAYER);
  dict_write_cstring(iter, MESSAGE_KEY_ARG_PLAYER_ID, player_id);
  app_message_outbox_send();
}

static void send_quick_play(const char *uri) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_CMD, CMD_QUICK_PLAY);
  dict_write_cstring(iter, MESSAGE_KEY_ARG_STR, uri);
  app_message_outbox_send();
}

static void send_group(const char *src_id, const char *target_id) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_CMD, CMD_GROUP);
  dict_write_cstring(iter, MESSAGE_KEY_ARG_PLAYER_ID, src_id);
  dict_write_cstring(iter, MESSAGE_KEY_ARG_TARGET_PLAYER_ID, target_id);
  app_message_outbox_send();
}

static void send_ungroup(const char *player_id) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_CMD, CMD_UNGROUP);
  dict_write_cstring(iter, MESSAGE_KEY_ARG_PLAYER_ID, player_id);
  app_message_outbox_send();
}

static void send_ungroup_all(const char *csv_ids) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) return;
  dict_write_uint8(iter, MESSAGE_KEY_CMD, CMD_UNGROUP_ALL);
  dict_write_cstring(iter, MESSAGE_KEY_ARG_PLAYER_IDS, csv_ids);
  app_message_outbox_send();
}

// ─── Quick Play persistence ─────────────────────────────────────────────

static void quick_load_from_persist(void) {
  s_quick_count = 0;
  if (!persist_exists(PERSIST_QUICK_COUNT)) return;
  int n = persist_read_int(PERSIST_QUICK_COUNT);
  if (n < 0) n = 0;
  if (n > MAX_QUICK_SLOTS) n = MAX_QUICK_SLOTS;
  for (int i = 0; i < n; i++) {
    uint32_t klabel = PERSIST_QUICK_BASE + i * 2;
    uint32_t kuri   = PERSIST_QUICK_BASE + i * 2 + 1;
    if (!persist_exists(klabel) || !persist_exists(kuri)) continue;
    persist_read_string(klabel, s_quick_slots[s_quick_count].label, MAX_QUICK_LABEL);
    persist_read_string(kuri,   s_quick_slots[s_quick_count].uri,   MAX_QUICK_URI);
    s_quick_count++;
  }
  LOGI("loaded %d quick slots", s_quick_count);
}

static void quick_save_to_persist(void) {
  persist_write_int(PERSIST_QUICK_COUNT, s_quick_count);
  for (int i = 0; i < s_quick_count; i++) {
    uint32_t klabel = PERSIST_QUICK_BASE + i * 2;
    uint32_t kuri   = PERSIST_QUICK_BASE + i * 2 + 1;
    persist_write_string(klabel, s_quick_slots[i].label);
    persist_write_string(kuri,   s_quick_slots[i].uri);
  }
  for (int i = s_quick_count; i < MAX_QUICK_SLOTS; i++) {
    uint32_t klabel = PERSIST_QUICK_BASE + i * 2;
    uint32_t kuri   = PERSIST_QUICK_BASE + i * 2 + 1;
    if (persist_exists(klabel)) persist_delete(klabel);
    if (persist_exists(kuri))   persist_delete(kuri);
  }
}

// ─── Rendering — now-playing window ───────────────────────────────────────

static void draw_progress_bar(GContext *ctx) {
  int16_t bar_x = 6;
  int16_t bar_w = CONTENT_W - 12;
  GRect rail = GRect(bar_x, PROGRESS_TOP, bar_w, PROGRESS_BAR_H);

  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, rail, 1, GCornersAll);

  if (s_now.duration_s == 0) return;
  uint32_t e = s_now.elapsed_s;
  if (e > s_now.duration_s) e = s_now.duration_s;
  int16_t fill_w = (int16_t)((uint32_t)bar_w * e / s_now.duration_s);
  if (fill_w <= 0) return;
  graphics_context_set_fill_color(ctx, GColorVividCerulean);
  graphics_fill_rect(ctx, GRect(bar_x, PROGRESS_TOP, fill_w, PROGRESS_BAR_H), 1, GCornersAll);
}

static void get_action_rects(GRect *r_prev, GRect *r_pp, GRect *r_next) {
  int16_t px = ACTION_X + 2;
  int16_t pw = ACTION_W - 4;
  int16_t pad = 3;
  *r_prev = GRect(px, ACTION_Y + pad,                      pw, ACTION_PILL_H - pad * 2);
  *r_pp   = GRect(px, ACTION_Y + ACTION_PILL_H + pad,      pw, ACTION_PILL_H - pad * 2);
  *r_next = GRect(px, ACTION_Y + ACTION_PILL_H * 2 + pad,  pw, ACTION_PILL_H - pad * 2);
}

static void get_strip_rects(GRect *r_shuf, GRect *r_rep, GRect *r_vol) {
  *r_shuf = GRect(0,                STRIP_Y, STRIP_ZONE_W, STRIP_H);
  *r_rep  = GRect(STRIP_ZONE_W,     STRIP_Y, STRIP_ZONE_W, STRIP_H);
  *r_vol  = GRect(STRIP_ZONE_W * 2, STRIP_Y, SCREEN_W - STRIP_ZONE_W * 2, STRIP_H);
}

static void now_root_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Header.
  GRect header = GRect(0, HEADER_Y, SCREEN_W, HEADER_H);
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, header, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_now.player_name,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(8, HEADER_Y + 3, SCREEN_W - 30, HEADER_H - 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  icon_chevron_right(ctx, GRect(SCREEN_W - 22, HEADER_Y + 2, 18, HEADER_H - 4), GColorWhite);

  // Title.
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_now.title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(6, TITLE_TOP, CONTENT_W - 8, TITLE_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Meta.
  char meta[MAX_TRACK_TEXT * 2 + 4];
  if (s_now.artist[0] && s_now.album[0]) {
    snprintf(meta, sizeof(meta), "%s — %s", s_now.artist, s_now.album);
  } else if (s_now.artist[0]) {
    snprintf(meta, sizeof(meta), "%s", s_now.artist);
  } else if (s_now.album[0]) {
    snprintf(meta, sizeof(meta), "%s", s_now.album);
  } else {
    meta[0] = '\0';
  }
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, meta,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(6, META_TOP, CONTENT_W - 8, META_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  draw_progress_bar(ctx);

  char times[40];
  unsigned em = s_now.elapsed_s  / 60, es = s_now.elapsed_s  % 60;
  unsigned dm = s_now.duration_s / 60, ds = s_now.duration_s % 60;
  if (em > 999) em = 999;
  if (dm > 999) dm = 999;
  snprintf(times, sizeof(times), "%u:%02u / %u:%02u", em, es, dm, ds);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, times,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(6, TIME_TOP, CONTENT_W - 8, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Right-edge action column.
  GRect r_prev, r_pp, r_next;
  get_action_rects(&r_prev, &r_pp, &r_next);

  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, r_prev, 6, GCornersAll);
  graphics_context_set_fill_color(ctx, GColorVividCerulean);
  graphics_fill_rect(ctx, r_pp,   6, GCornersAll);
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, r_next, 6, GCornersAll);

  icon_prev(ctx, r_prev, GColorWhite);
  if (s_now.state == PS_PLAYING) {
    icon_pause(ctx, r_pp, GColorWhite);
  } else {
    icon_play(ctx, r_pp, GColorWhite);
  }
  icon_next(ctx, r_next, GColorWhite);

  // Bottom status strip.
  GRect r_shuf, r_rep, r_vol;
  get_strip_rects(&r_shuf, &r_rep, &r_vol);

  GRect strip = GRect(0, STRIP_Y, SCREEN_W, STRIP_H);
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, strip, 0, GCornerNone);

  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, GPoint(STRIP_ZONE_W,     STRIP_Y + 8),
                          GPoint(STRIP_ZONE_W,     STRIP_Y + STRIP_H - 8));
  graphics_draw_line(ctx, GPoint(STRIP_ZONE_W * 2, STRIP_Y + 8),
                          GPoint(STRIP_ZONE_W * 2, STRIP_Y + STRIP_H - 8));

  GColor shuf_col = s_now.shuffle ? GColorVividCerulean : GColorLightGray;
  icon_shuffle(ctx, GRect(r_shuf.origin.x + (STRIP_ZONE_W - 22) / 2,
                          r_shuf.origin.y + 4, 22, 16), shuf_col);
  graphics_context_set_text_color(ctx, shuf_col);
  graphics_draw_text(ctx, s_now.shuffle ? "ON" : "OFF",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(r_shuf.origin.x, r_shuf.origin.y + 20, r_shuf.size.w, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  GColor rep_col = (s_now.repeat == RM_OFF) ? GColorLightGray : GColorVividCerulean;
  const char *rep_label = (s_now.repeat == RM_OFF) ? "OFF" :
                          (s_now.repeat == RM_ONE) ? "ONE" : "ALL";
  icon_repeat(ctx, GRect(r_rep.origin.x + (STRIP_ZONE_W - 22) / 2,
                          r_rep.origin.y + 4, 22, 16), rep_col, s_now.repeat);
  graphics_context_set_text_color(ctx, rep_col);
  graphics_draw_text(ctx, rep_label,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(r_rep.origin.x, r_rep.origin.y + 20, r_rep.size.w, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  GColor vol_col = s_now.muted ? GColorRed : GColorWhite;
  icon_speaker(ctx, GRect(r_vol.origin.x + (r_vol.size.w - 24) / 2,
                          r_vol.origin.y + 4, 24, 16), vol_col, s_now.muted);
  char volbuf[8];
  if (s_now.muted) {
    snprintf(volbuf, sizeof(volbuf), "MUTE");
  } else {
    snprintf(volbuf, sizeof(volbuf), "%u%%", (unsigned)s_now.volume);
  }
  graphics_context_set_text_color(ctx, vol_col);
  graphics_draw_text(ctx, volbuf,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     GRect(r_vol.origin.x, r_vol.origin.y + 20, r_vol.size.w, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Error banner.
  if (s_now.last_error[0]) {
    GRect eb = GRect(0, 0, bounds.size.w, 14);
    graphics_context_set_fill_color(ctx, GColorDarkCandyAppleRed);
    graphics_fill_rect(ctx, eb, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_now.last_error,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(4, -2, bounds.size.w - 8, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

// ─── Hit testing ──────────────────────────────────────────────────────────

typedef enum {
  HIT_NONE,
  HIT_HEADER,
  HIT_PREV,
  HIT_PLAYPAUSE,
  HIT_NEXT,
  HIT_SHUFFLE,
  HIT_REPEAT,
  HIT_VOLUME,
} Hit;

static Hit hit_test_now(int16_t x, int16_t y) {
  if (y < HEADER_H) return HIT_HEADER;
  if (y >= STRIP_Y) {
    if (x < STRIP_ZONE_W)        return HIT_SHUFFLE;
    if (x < STRIP_ZONE_W * 2)    return HIT_REPEAT;
    return HIT_VOLUME;
  }
  // Right-edge action column. Use the full column width as the tap target,
  // not just the pill — fingers are bigger than the pills.
  if (x >= ACTION_X) {
    int16_t ry = y - ACTION_Y;
    if (ry < ACTION_PILL_H)            return HIT_PREV;
    if (ry < ACTION_PILL_H * 2)        return HIT_PLAYPAUSE;
    return HIT_NEXT;
  }
  return HIT_NONE;
}

// ─── Forward declarations ─────────────────────────────────────────────────

static void players_window_push(void);
static void volume_window_push(void);
static void quick_window_push(void);

static void handle_tap_now(int16_t x, int16_t y) {
  Hit h = hit_test_now(x, y);
  LOGD("tap (%d,%d) -> hit=%d", x, y, h);
  switch (h) {
    case HIT_PREV:           send_simple_cmd(CMD_PREVIOUS);       break;
    case HIT_PLAYPAUSE:      send_simple_cmd(CMD_PLAY_PAUSE);     break;
    case HIT_NEXT:           send_simple_cmd(CMD_NEXT);           break;
    case HIT_SHUFFLE:        send_simple_cmd(CMD_SHUFFLE_TOGGLE); break;
    case HIT_REPEAT:         send_simple_cmd(CMD_REPEAT_CYCLE);   break;
    case HIT_VOLUME:         volume_window_push();                break;
    case HIT_HEADER:         send_simple_cmd(CMD_REQUEST_PLAYERS);
                             players_window_push();               break;
    default: break;
  }
}

// ─── Touch handling — now-playing ─────────────────────────────────────────

static void touch_now_handler(const TouchEvent *e, void *ctx) {
  switch (e->type) {
    case TouchEvent_Touchdown:
      s_touch.phase   = TG_TRACKING;
      s_touch.start_x = e->x;
      s_touch.start_y = e->y;
      s_touch.last_x  = e->x;
      s_touch.last_y  = e->y;
      s_touch.start_ms = time_ms(NULL, NULL);
      break;
    case TouchEvent_PositionUpdate:
      s_touch.last_x = e->x;
      s_touch.last_y = e->y;
      break;
    case TouchEvent_Liftoff: {
      if (s_touch.phase != TG_TRACKING) break;
      int16_t dx = s_touch.last_x - s_touch.start_x;
      int16_t dy = s_touch.last_y - s_touch.start_y;
      uint32_t dt = time_ms(NULL, NULL) - s_touch.start_ms;
      bool is_tap = (dx > -TAP_SLOP_PX && dx < TAP_SLOP_PX &&
                     dy > -TAP_SLOP_PX && dy < TAP_SLOP_PX &&
                     dt < TAP_MAX_MS);
      s_touch.phase = TG_IDLE;
      if (is_tap) handle_tap_now(s_touch.start_x, s_touch.start_y);
      break;
    }
  }
}

// ─── Button handlers — now-playing ────────────────────────────────────────

static void btn_now_select(ClickRecognizerRef r, void *c)      { send_simple_cmd(CMD_PLAY_PAUSE); }
static void btn_now_select_long(ClickRecognizerRef r, void *c) { quick_window_push(); }
static void btn_now_up(ClickRecognizerRef r, void *c)          { send_simple_cmd(CMD_PREVIOUS);  }
static void btn_now_down(ClickRecognizerRef r, void *c)        { send_simple_cmd(CMD_NEXT);      }

static void now_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, btn_now_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, btn_now_select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_UP,    btn_now_up);
  window_single_click_subscribe(BUTTON_ID_DOWN,  btn_now_down);
}

// ─── Local interpolation tick ─────────────────────────────────────────────
//
// Bumps elapsed_s once per second while the watch is awake AND we have a
// playing track AND the now-playing window is visible.  We MUST stop ticking
// when the app loses focus (screen-off, notification overlay, sub-window
// pushed onto our stack) — otherwise a runaway 1 Hz redraw during display-off
// trips the system watchdog and the app gets killed silently.  Re-arm when
// focus returns via app_focus_service.

static bool s_app_focused = true;
static bool s_now_visible = true;

static void schedule_tick(void);
static void tick_cb(void *ctx) {
  s_tick_timer = NULL;
  if (!s_app_focused || !s_now_visible) return;     // settle while idle
  if (s_now.state == PS_PLAYING && s_now.elapsed_s < s_now.duration_s) {
    s_now.elapsed_s++;
    if (s_now_root_layer) layer_mark_dirty(s_now_root_layer);
  }
  schedule_tick();
}
static void schedule_tick(void) {
  if (s_tick_timer) return;
  if (!s_app_focused || !s_now_visible) return;     // don't even arm if hidden
  s_tick_timer = app_timer_register(TICK_MS, tick_cb, NULL);
}
static void cancel_tick(void) {
  if (s_tick_timer) { app_timer_cancel(s_tick_timer); s_tick_timer = NULL; }
}

static void app_focus_handler(bool in_focus) {
  s_app_focused = in_focus;
  if (in_focus) schedule_tick();
  else          cancel_tick();
}

// ─── Player list window ───────────────────────────────────────────────────

#define PL_ROW_H 44

static uint16_t pl_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_players_count > 0 ? s_players_count : 1;
}

static int16_t pl_get_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return PL_ROW_H; }

static int16_t pl_get_sep_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return 1; }

static void pl_draw_sep(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
}

// Custom row drawing — clear two-line layout with explicit padding, a
// coloured state dot on the left, and ample gap between title and subtitle
// so nothing overlaps.  v0.3.0: master rows get a chain badge on the right,
// member rows get a left rail + indent + "synced" subtitle.
static void pl_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect b = layer_get_bounds(cell);
  bool hi = menu_cell_layer_is_highlighted(cell);

  if (s_players_count == 0) {
    graphics_context_set_text_color(ctx, hi ? GColorWhite : GColorLightGray);
    graphics_draw_text(ctx, "No players",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(b.origin.x + 12, b.origin.y + 6, b.size.w - 24, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    graphics_draw_text(ctx, "Pull to refresh",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(b.origin.x + 12, b.origin.y + 24, b.size.w - 24, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }

  const PlayerRow *p = &s_players[idx->row];
  bool is_master = (p->flags & ROW_FLAG_MASTER) != 0;
  bool is_member = (p->flags & ROW_FLAG_MEMBER) != 0;

  // Member rows: 2-px vertical rail down the left edge, indent dot 18 px.
  int dot_cx     = b.origin.x + 14;
  int dot_r      = 5;
  int text_x     = b.origin.x + 28;
  int text_w_pad = 40;  // room for chain badge on master rows
  if (is_member) {
    graphics_context_set_stroke_color(ctx, hi ? GColorWhite : GColorDarkGray);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(b.origin.x + 14, b.origin.y),
                            GPoint(b.origin.x + 14, b.origin.y + PL_ROW_H));
    dot_cx = b.origin.x + 32;
    dot_r  = 4;
    text_x = b.origin.x + 46;
    text_w_pad = 56;
  }

  // Left dot: green = playing, amber = paused, grey = idle.
  GColor dot =
      p->state == PS_PLAYING ? GColorIslamicGreen :
      p->state == PS_PAUSED  ? GColorChromeYellow : GColorDarkGray;
  graphics_context_set_fill_color(ctx, dot);
  graphics_fill_circle(ctx, GPoint(dot_cx, b.origin.y + PL_ROW_H / 2), dot_r);

  // Name — always white on the row background, or white on the cerulean highlight.
  // Member rows use the slightly smaller 16-bold to match the mock.
  const char *name_font = is_member ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_18_BOLD;
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, p->name,
                     fonts_get_system_font(name_font),
                     GRect(text_x, b.origin.y + 4, b.size.w - text_w_pad, 24),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Subtitle.  Member rows get "↳ synced" rendered with the native subarrow.
  if (is_member) {
    icon_subarrow(ctx, GRect(text_x, b.origin.y + 22, 12, 14),
                  hi ? GColorPastelYellow : GColorLightGray);
    graphics_context_set_text_color(ctx, hi ? GColorPastelYellow : GColorLightGray);
    graphics_draw_text(ctx, "synced",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(text_x + 14, b.origin.y + 24, b.size.w - text_w_pad - 14, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  } else {
    const char *sub =
        is_master              ? (p->state == PS_PLAYING ? "Playing - group" :
                                  p->state == PS_PAUSED  ? "Paused - group"  :
                                                           "Idle - group")
      : p->state == PS_PLAYING ? "Playing"
      : p->state == PS_PAUSED  ? "Paused"
      : p->state == PS_IDLE    ? "Idle"
      :                          "-";
    graphics_context_set_text_color(ctx, hi ? GColorPastelYellow : GColorLightGray);
    graphics_draw_text(ctx, sub,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(text_x, b.origin.y + 24, b.size.w - text_w_pad, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

  // Master rows get a chain badge on the right.  Tiny outlined rounded-rect
  // with the chain glyph + member count inside.
  if (is_master && p->group_count > 0) {
    GRect badge = GRect(b.origin.x + b.size.w - 38, b.origin.y + 10, 32, 16);
    graphics_context_set_fill_color(ctx, hi ? GColorOxfordBlue : GColorOxfordBlue);
    graphics_fill_rect(ctx, badge, 4, GCornersAll);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_round_rect(ctx, badge, 4);
    icon_chain(ctx, GRect(badge.origin.x + 2, badge.origin.y + 4, 12, 8), GColorWhite);
    char count_buf[6];
    snprintf(count_buf, sizeof(count_buf), "%u", (unsigned)p->group_count);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, count_buf,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(badge.origin.x + 16, badge.origin.y - 1, 14, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
}

static void pl_select(MenuLayer *ml, MenuIndex *idx, void *c) {
  if (s_players_count == 0) return;
  const PlayerRow *p = &s_players[idx->row];
  LOGI("select player %s (%s)", p->name, p->id);
  send_select_player(p->id);
  window_stack_pop(true);
}

// Forward decl — implemented after groupsheet_window below.
static void groupsheet_window_push(int target_idx);

// Compute the row index from a finger Y coordinate, with the y-pitch math
// unchanged from v0.2.0 (44 px + 1 px separator).
static int pl_row_from_y(int16_t y) {
  int row = y / (PL_ROW_H + 1);
  if (row < 0) row = 0;
  if (row >= s_players_count) row = s_players_count - 1;
  return row;
}

// Fired by AppTimer after LONG_PRESS_MS if the finger hasn't moved outside
// TAP_SLOP_PX and hasn't lifted.  Opens the action sheet on the row currently
// under the finger.
static void pl_long_press_fire(void *ctx) {
  s_pl_long_timer = NULL;
  if (!s_players_menu || s_players_count == 0) return;
  int row = pl_row_from_y(s_pl_long_y);
  LOGI("long-press touch -> row %d (%s)", row, s_players[row].name);
  // Cancel the in-flight tap so liftoff doesn't also fire pl_select.
  s_touch.phase = TG_IDLE;
  groupsheet_window_push(row);
}

static void pl_cancel_long_press(void) {
  if (s_pl_long_timer) { app_timer_cancel(s_pl_long_timer); s_pl_long_timer = NULL; }
}

static void touch_players_handler(const TouchEvent *e, void *ctx) {
  if (!s_players_menu || s_players_count == 0) return;
  int row = pl_row_from_y(e->y);

  if (e->type == TouchEvent_Touchdown) {
    s_touch.phase    = TG_TRACKING;
    s_touch.start_x  = e->x;
    s_touch.start_y  = e->y;
    s_touch.last_x   = e->x;
    s_touch.last_y   = e->y;
    s_touch.start_ms = time_ms(NULL, NULL);
    s_pl_long_x = e->x; s_pl_long_y = e->y;
    pl_cancel_long_press();
    s_pl_long_timer = app_timer_register(LONG_PRESS_MS, pl_long_press_fire, NULL);
    MenuIndex idx = { 0, (uint16_t)row };
    menu_layer_set_selected_index(s_players_menu, idx, MenuRowAlignCenter, false);
  } else if (e->type == TouchEvent_PositionUpdate) {
    s_touch.last_x = e->x; s_touch.last_y = e->y;
    s_pl_long_x = e->x; s_pl_long_y = e->y;
    // Out-of-slop drag cancels the pending long-press.
    int16_t dx = e->x - s_touch.start_x;
    int16_t dy = e->y - s_touch.start_y;
    if (dx < -TAP_SLOP_PX || dx > TAP_SLOP_PX ||
        dy < -TAP_SLOP_PX || dy > TAP_SLOP_PX) {
      pl_cancel_long_press();
    }
    MenuIndex idx = { 0, (uint16_t)row };
    menu_layer_set_selected_index(s_players_menu, idx, MenuRowAlignCenter, false);
  } else if (e->type == TouchEvent_Liftoff) {
    pl_cancel_long_press();
    if (s_touch.phase != TG_TRACKING) {
      // The long-press timer already fired and cleared phase; ignore this lift.
      s_touch.phase = TG_IDLE;
      return;
    }
    int16_t dx = e->x - s_touch.start_x;
    int16_t dy = e->y - s_touch.start_y;
    uint32_t dt = time_ms(NULL, NULL) - s_touch.start_ms;
    bool is_tap = (dx > -TAP_SLOP_PX && dx < TAP_SLOP_PX &&
                   dy > -TAP_SLOP_PX && dy < TAP_SLOP_PX &&
                   dt < TAP_MAX_MS);
    s_touch.phase = TG_IDLE;
    if (is_tap) {
      MenuIndex idx = { 0, (uint16_t)row };
      pl_select(s_players_menu, &idx, NULL);
    }
  }
}

// Button bridges so the players-window long-click can sit on top of the menu's
// own click-config without losing scroll behaviour.
static void btn_pl_select(ClickRecognizerRef r, void *c) {
  if (!s_players_menu || s_players_count == 0) return;
  MenuIndex idx = menu_layer_get_selected_index(s_players_menu);
  pl_select(s_players_menu, &idx, NULL);
}
static void btn_pl_select_long(ClickRecognizerRef r, void *c) {
  if (!s_players_menu || s_players_count == 0) return;
  MenuIndex idx = menu_layer_get_selected_index(s_players_menu);
  LOGI("long-press button -> row %u", (unsigned)idx.row);
  groupsheet_window_push(idx.row);
}
static void btn_pl_up(ClickRecognizerRef r, void *c) {
  if (s_players_menu) menu_layer_set_selected_next(s_players_menu, true,
                                                   MenuRowAlignCenter, true);
}
static void btn_pl_down(ClickRecognizerRef r, void *c) {
  if (s_players_menu) menu_layer_set_selected_next(s_players_menu, false,
                                                   MenuRowAlignCenter, true);
}

static void players_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, btn_pl_select);
  window_long_click_subscribe(BUTTON_ID_SELECT, LONG_PRESS_MS, btn_pl_select_long, NULL);
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   150, btn_pl_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, btn_pl_down);
}

static void players_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect bounds = layer_get_bounds(root);
  s_players_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_players_menu, NULL, (MenuLayerCallbacks){
      .get_num_rows         = pl_get_num_rows,
      .get_cell_height      = pl_get_cell_height,
      .get_separator_height = pl_get_sep_height,
      .draw_row             = pl_draw_row,
      .draw_separator       = pl_draw_sep,
      .select_click         = pl_select,
  });
  // Our own click provider replaces the menu's default so we can layer a
  // long-press onto SELECT.  UP/DOWN scroll handlers are routed back through
  // the menu API to keep the v0.2.0 behaviour intact.
  window_set_click_config_provider(w, players_click_config);
  menu_layer_set_normal_colors(s_players_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_players_menu, GColorVividCerulean, GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_players_menu));
  touch_service_unsubscribe();
  touch_service_subscribe(touch_players_handler, NULL);
}
static void players_window_unload(Window *w) {
  pl_cancel_long_press();
  menu_layer_destroy(s_players_menu);
  s_players_menu = NULL;
  touch_service_unsubscribe();
  touch_service_subscribe(touch_now_handler, NULL);
}

static void players_window_push(void) {
  if (!s_players_window) {
    s_players_window = window_create();
    window_set_window_handlers(s_players_window, (WindowHandlers){
        .load = players_window_load,
        .unload = players_window_unload,
    });
  }
  window_stack_push(s_players_window, true);
}

// ─── Group action sheet window ────────────────────────────────────────────
//
// Two visual variants live in the same window:
//   A. Target is solo  → list of "Join <master>" / "Group with <solo>" rows.
//   B. Target is member → readout + Leave / Add-to-group (+ Ungroup-all for master).
//
// Layout: 28-px header (oxford blue) at top, 28-px hint strip (darker blue)
// at bottom, MenuLayer in between (171 px tall).  Header + hint are static
// child layers of the window root, painted in their own update procs.

#define GS_HEADER_H 28
#define GS_HINT_H   28
#define GS_MENU_Y   (GS_HEADER_H + 1)
#define GS_MENU_H   (SCREEN_H - GS_HEADER_H - GS_HINT_H - 1)

// Row "kinds" for the action sheet — each kind has its own draw + select.
typedef enum {
  GS_KIND_NONE = 0,
  GS_KIND_JOIN,       // "Join <name>" — target a master player
  GS_KIND_PAIR,       // "Group with <name>" — target a solo player
  GS_KIND_READOUT,    // Non-selectable "in group with <master>" line
  GS_KIND_LEAVE,      // "Leave group"
  GS_KIND_ADD,        // "Add to this group" — v0.3.1 stub
  GS_KIND_UNGROUP_ALL,// "Ungroup all"
} GsRowKind;

typedef struct {
  GsRowKind kind;
  uint8_t   player_idx;   // index into s_players[] (for JOIN/PAIR)
  bool      enabled;
} GsRow;

#define MAX_GS_ROWS (MAX_PLAYERS + 4)
static GsRow  s_gs_rows[MAX_GS_ROWS];
static int    s_gs_row_count;
static bool   s_gs_variant_member;

// Find index of a player by id, or -1.
static int player_index_by_id(const char *id) {
  if (!id || !id[0]) return -1;
  for (int i = 0; i < s_players_count; i++) {
    if (strncmp(s_players[i].id, id, MAX_PLAYER_ID) == 0) return i;
  }
  return -1;
}

static void gs_build_rows(int target_idx) {
  s_gs_row_count = 0;
  if (target_idx < 0 || target_idx >= s_players_count) return;
  const PlayerRow *t = &s_players[target_idx];
  s_gs_variant_member = (t->flags & ROW_FLAG_MEMBER) != 0;

  if (!s_gs_variant_member) {
    // Variant A: solo / master target.  Show every other player as joinable.
    for (int i = 0; i < s_players_count && s_gs_row_count < MAX_GS_ROWS; i++) {
      if (i == target_idx) continue;
      GsRow r = { 0 };
      const PlayerRow *p = &s_players[i];
      r.player_idx = (uint8_t)i;
      r.kind    = (p->flags & ROW_FLAG_MASTER) ? GS_KIND_JOIN : GS_KIND_PAIR;
      // can_group_with is not mirrored to the watch (the probe shows it's
      // commonly empty), so we don't filter here.  All other players enabled.
      r.enabled = true;
      s_gs_rows[s_gs_row_count++] = r;
    }
  } else {
    // Variant B: target is a member of a group.
    // Row 0 — readout, non-selectable.
    s_gs_rows[s_gs_row_count++] = (GsRow){ .kind = GS_KIND_READOUT,     .enabled = false };
    // Row 1 — Leave group.
    s_gs_rows[s_gs_row_count++] = (GsRow){ .kind = GS_KIND_LEAVE,       .enabled = true  };
    // Row 2 — Add to this group (v0.3.1 stub — the picker sub-sheet ships in
    // the next release; for v0.3.0 the row logs and pops).
    s_gs_rows[s_gs_row_count++] = (GsRow){ .kind = GS_KIND_ADD,         .enabled = true  };
    // Row 3 — Ungroup all (tear down the whole group from any member's POV).
    // Mock screen 3 includes this on a member sheet, so we mirror that even
    // though it isn't strictly required by the cmd surface — ungrouping any
    // member sends ungroup_many over the full member set.
    s_gs_rows[s_gs_row_count++] = (GsRow){ .kind = GS_KIND_UNGROUP_ALL, .enabled = true  };
  }
}

static uint16_t gs_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_gs_row_count > 0 ? s_gs_row_count : 1;
}

static int16_t gs_get_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (s_gs_row_count == 0) return 40;
  GsRowKind k = s_gs_rows[idx->row].kind;
  if (k == GS_KIND_READOUT) return 36;
  if (k == GS_KIND_LEAVE || k == GS_KIND_ADD || k == GS_KIND_UNGROUP_ALL) return 44;
  return 40; // join / pair
}

static int16_t gs_get_sep_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return 1; }

static void gs_draw_sep(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
}

static void gs_draw_join(GContext *ctx, GRect b, bool hi, const GsRow *r) {
  const PlayerRow *p = &s_players[r->player_idx];
  GColor primary = r->enabled ? GColorWhite     : GColorDarkGray;
  GColor dim     = r->enabled ? (hi ? GColorPastelYellow : GColorLightGray)
                              : GColorDarkGray;

  char title[64];
  if (r->kind == GS_KIND_JOIN) {
    snprintf(title, sizeof(title), "Join %s", p->name);
  } else {
    snprintf(title, sizeof(title), "Group with %s", p->name);
  }
  graphics_context_set_text_color(ctx, primary);
  graphics_draw_text(ctx, title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(b.origin.x + 8, b.origin.y + 2, b.size.w - 16, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  const char *sub;
  char subbuf[40];
  if (!r->enabled) {
    sub = "incompatible";
  } else if (r->kind == GS_KIND_JOIN) {
    snprintf(subbuf, sizeof(subbuf), "%u players in group", (unsigned)p->group_count);
    sub = subbuf;
  } else {
    sub = p->state == PS_PLAYING ? "playing" :
          p->state == PS_PAUSED  ? "paused"  :
          p->state == PS_IDLE    ? "idle"    : "ready";
  }
  graphics_context_set_text_color(ctx, dim);
  graphics_draw_text(ctx, sub,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(b.origin.x + 8, b.origin.y + 20, b.size.w - 16, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void gs_draw_readout(GContext *ctx, GRect b) {
  // Find the master this target is synced to.
  const PlayerRow *target = (s_groupsheet_target >= 0 && s_groupsheet_target < s_players_count)
      ? &s_players[s_groupsheet_target] : NULL;
  int master_idx = target ? player_index_by_id(target->synced_to) : -1;
  const PlayerRow *master = (master_idx >= 0) ? &s_players[master_idx] : NULL;

  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, "in group with",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(b.origin.x + 8, b.origin.y + 2, b.size.w - 16, 16),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  char line[64];
  if (master) {
    // group_count is the total players in the group including the master.
    // The "+N" suffix reads as "master plus N OTHER members beyond myself",
    // i.e. count minus the master AND minus the viewing player.
    int others = master->group_count > 0 ? (int)master->group_count - 2 : 0;
    if (others < 0) others = 0;
    if (others > 0) snprintf(line, sizeof(line), "%s +%d", master->name, others);
    else            snprintf(line, sizeof(line), "%s",     master->name);
  } else {
    snprintf(line, sizeof(line), "unknown master");
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, line,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(b.origin.x + 8, b.origin.y + 16, b.size.w - 16, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void gs_draw_action(GContext *ctx, GRect b, bool hi, const GsRow *r) {
  const PlayerRow *target = (s_groupsheet_target >= 0 && s_groupsheet_target < s_players_count)
      ? &s_players[s_groupsheet_target] : NULL;

  // Icon area: 28 px wide on the left.
  GRect ic = GRect(b.origin.x + 8, b.origin.y + 8, 22, 22);
  const char *title = "";
  char subbuf[64];
  const char *sub = "";

  switch (r->kind) {
    case GS_KIND_LEAVE:
      icon_eject(ctx, ic, GColorWhite);
      title = "Leave group";
      snprintf(subbuf, sizeof(subbuf), "%s plays on its own",
               target ? target->name : "Player");
      sub = subbuf;
      break;
    case GS_KIND_ADD:
      icon_plus(ctx, ic, GColorWhite);
      title = "Add to this group";
      sub   = "pick a player to add";
      break;
    case GS_KIND_UNGROUP_ALL:
      icon_warning(ctx, ic, GColorOrange);
      title = "Ungroup all";
      sub   = "tear down whole group";
      break;
    default: break;
  }

  GColor title_col = (r->kind == GS_KIND_UNGROUP_ALL) ? GColorOrange : GColorWhite;
  graphics_context_set_text_color(ctx, title_col);
  graphics_draw_text(ctx, title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(b.origin.x + 36, b.origin.y + 4, b.size.w - 44, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  graphics_context_set_text_color(ctx, hi ? GColorPastelYellow : GColorLightGray);
  graphics_draw_text(ctx, sub,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(b.origin.x + 36, b.origin.y + 24, b.size.w - 44, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void gs_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  GRect b = layer_get_bounds(cell);
  bool hi = menu_cell_layer_is_highlighted(cell);
  if (s_gs_row_count == 0) {
    graphics_context_set_text_color(ctx, GColorLightGray);
    graphics_draw_text(ctx, "No actions",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(b.origin.x + 8, b.origin.y + 8, b.size.w - 16, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }
  const GsRow *r = &s_gs_rows[idx->row];
  switch (r->kind) {
    case GS_KIND_JOIN:
    case GS_KIND_PAIR:
      gs_draw_join(ctx, b, hi, r); break;
    case GS_KIND_READOUT:
      gs_draw_readout(ctx, b); break;
    case GS_KIND_LEAVE:
    case GS_KIND_ADD:
    case GS_KIND_UNGROUP_ALL:
      gs_draw_action(ctx, b, hi, r); break;
    default: break;
  }
}

static void gs_select(MenuLayer *ml, MenuIndex *idx, void *c) {
  if (s_gs_row_count == 0) return;
  if (idx->row >= s_gs_row_count) return;
  const GsRow *r = &s_gs_rows[idx->row];
  if (!r->enabled) { LOGI("gs row disabled, ignoring"); return; }

  const PlayerRow *target = (s_groupsheet_target >= 0 && s_groupsheet_target < s_players_count)
      ? &s_players[s_groupsheet_target] : NULL;
  if (!target) { LOGE("gs select with no target"); window_stack_pop(true); return; }

  switch (r->kind) {
    case GS_KIND_JOIN:
    case GS_KIND_PAIR: {
      const PlayerRow *other = &s_players[r->player_idx];
      LOGI("group %s -> %s", target->id, other->id);
      // For "Join <master>": source = target, destination = master (so target syncs to master).
      // For "Group with <solo>": same shape — target becomes a member of the other.
      send_group(target->id, other->id);
      window_stack_pop(true);
      break;
    }
    case GS_KIND_LEAVE:
      LOGI("ungroup %s", target->id);
      send_ungroup(target->id);
      window_stack_pop(true);
      break;
    case GS_KIND_ADD:
      // v0.3.1 followup: drill into a sub-sheet listing ungrouped candidates,
      // then call set_members against the master.  For v0.3.0 we log and pop.
      LOGI("TODO add-to-group picker (v0.3.1 followup)");
      window_stack_pop(true);
      break;
    case GS_KIND_UNGROUP_ALL: {
      // Build a CSV of every player_id in this group (master + all members).
      // If the long-pressed target is itself a member we resolve up to the
      // master via synced_to; otherwise the target IS the master.
      const char *master_id = (target->flags & ROW_FLAG_MEMBER)
          ? target->synced_to : target->id;
      char csv[MAX_PLAYER_ID * MAX_PLAYERS + MAX_PLAYERS];
      csv[0] = '\0';
      size_t pos = 0;
      pos += snprintf(csv + pos, sizeof(csv) - pos, "%s", master_id);
      for (int i = 0; i < s_players_count && pos < sizeof(csv) - 1; i++) {
        if (strncmp(s_players[i].synced_to, master_id, MAX_PLAYER_ID) == 0) {
          pos += snprintf(csv + pos, sizeof(csv) - pos, ",%s", s_players[i].id);
        }
      }
      LOGI("ungroup_all: %s", csv);
      send_ungroup_all(csv);
      window_stack_pop(true);
      break;
    }
    default: break;
  }
}

// Static header + hint overlays.  Stored as a single root-layer update proc.
static Layer *s_gs_chrome_layer;
static void gs_chrome_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Background.
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  const PlayerRow *target = (s_groupsheet_target >= 0 && s_groupsheet_target < s_players_count)
      ? &s_players[s_groupsheet_target] : NULL;

  // Header strip.
  GRect header = GRect(0, 0, SCREEN_W, GS_HEADER_H);
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, header, 0, GCornerNone);

  if (target) {
    GColor dot =
        target->state == PS_PLAYING ? GColorIslamicGreen :
        target->state == PS_PAUSED  ? GColorChromeYellow : GColorDarkGray;
    graphics_context_set_fill_color(ctx, dot);
    graphics_fill_circle(ctx, GPoint(12, GS_HEADER_H / 2), 4);

    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, target->name,
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(22, 2, SCREEN_W - 26, GS_HEADER_H - 4),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }

  // Hint strip — darker oxford-blue-ish accent at the bottom.
  GRect hint = GRect(0, SCREEN_H - GS_HINT_H, SCREEN_W, GS_HINT_H);
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, hint, 0, GCornerNone);
  const char *hint_str = s_gs_variant_member
      ? "SELECT to confirm  BACK to cancel"
      : "SELECT to join  BACK to cancel";
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, hint_str,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, SCREEN_H - GS_HINT_H + 6, SCREEN_W, GS_HINT_H - 6),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Button bridges for the action sheet — keep BACK as the standard pop, SELECT
// confirms via the menu.
static void btn_gs_select(ClickRecognizerRef r, void *c) {
  if (!s_groupsheet_menu) return;
  MenuIndex idx = menu_layer_get_selected_index(s_groupsheet_menu);
  gs_select(s_groupsheet_menu, &idx, NULL);
}
static void btn_gs_up(ClickRecognizerRef r, void *c) {
  if (s_groupsheet_menu) menu_layer_set_selected_next(s_groupsheet_menu, true,
                                                      MenuRowAlignCenter, true);
}
static void btn_gs_down(ClickRecognizerRef r, void *c) {
  if (s_groupsheet_menu) menu_layer_set_selected_next(s_groupsheet_menu, false,
                                                      MenuRowAlignCenter, true);
}
static void gs_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, btn_gs_select);
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   150, btn_gs_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, btn_gs_down);
}

static void touch_groupsheet_handler(const TouchEvent *e, void *ctx) {
  if (!s_groupsheet_menu || s_gs_row_count == 0) return;
  // The MenuLayer sits between GS_MENU_Y and SCREEN_H - GS_HINT_H.  Map the
  // raw finger Y back into menu-local coordinates and walk the rows.
  int16_t y = e->y;
  if (y < GS_MENU_Y) y = GS_MENU_Y;
  if (y > SCREEN_H - GS_HINT_H) y = SCREEN_H - GS_HINT_H;
  int local_y = y - GS_MENU_Y;
  int row = 0;
  int acc = 0;
  for (int i = 0; i < s_gs_row_count; i++) {
    MenuIndex mi = { 0, (uint16_t)i };
    int h = gs_get_cell_height(s_groupsheet_menu, &mi, NULL) + 1;
    if (local_y < acc + h) { row = i; break; }
    acc += h;
    row = i; // clamp to last row if y is past the end
  }

  if (e->type == TouchEvent_Touchdown) {
    s_touch.phase   = TG_TRACKING;
    s_touch.start_x = e->x; s_touch.start_y = e->y;
    s_touch.start_ms = time_ms(NULL, NULL);
    MenuIndex idx = { 0, (uint16_t)row };
    menu_layer_set_selected_index(s_groupsheet_menu, idx, MenuRowAlignCenter, false);
  } else if (e->type == TouchEvent_PositionUpdate) {
    MenuIndex idx = { 0, (uint16_t)row };
    menu_layer_set_selected_index(s_groupsheet_menu, idx, MenuRowAlignCenter, false);
  } else if (e->type == TouchEvent_Liftoff) {
    int16_t dx = e->x - s_touch.start_x;
    int16_t dy = e->y - s_touch.start_y;
    uint32_t dt = time_ms(NULL, NULL) - s_touch.start_ms;
    bool is_tap = (dx > -TAP_SLOP_PX && dx < TAP_SLOP_PX &&
                   dy > -TAP_SLOP_PX && dy < TAP_SLOP_PX &&
                   dt < TAP_MAX_MS);
    s_touch.phase = TG_IDLE;
    if (is_tap) {
      MenuIndex idx = { 0, (uint16_t)row };
      gs_select(s_groupsheet_menu, &idx, NULL);
    }
  }
}

static void groupsheet_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect bounds = layer_get_bounds(root);

  // Chrome layer (header + hint) covers the full root, but only draws the
  // top + bottom strips.  Menu layer sits on top in the middle band.
  s_gs_chrome_layer = layer_create(bounds);
  layer_set_update_proc(s_gs_chrome_layer, gs_chrome_update);
  layer_add_child(root, s_gs_chrome_layer);

  GRect menu_rect = GRect(0, GS_MENU_Y, SCREEN_W, GS_MENU_H);
  s_groupsheet_menu = menu_layer_create(menu_rect);
  menu_layer_set_callbacks(s_groupsheet_menu, NULL, (MenuLayerCallbacks){
      .get_num_rows         = gs_get_num_rows,
      .get_cell_height      = gs_get_cell_height,
      .get_separator_height = gs_get_sep_height,
      .draw_row             = gs_draw_row,
      .draw_separator       = gs_draw_sep,
      .select_click         = gs_select,
  });
  window_set_click_config_provider(w, gs_click_config);
  menu_layer_set_normal_colors(s_groupsheet_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_groupsheet_menu, GColorVividCerulean, GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_groupsheet_menu));

  // Default-select first enabled row.
  for (int i = 0; i < s_gs_row_count; i++) {
    if (s_gs_rows[i].enabled) {
      MenuIndex idx = { 0, (uint16_t)i };
      menu_layer_set_selected_index(s_groupsheet_menu, idx, MenuRowAlignCenter, false);
      break;
    }
  }

  touch_service_unsubscribe();
  touch_service_subscribe(touch_groupsheet_handler, NULL);
}

static void groupsheet_window_unload(Window *w) {
  if (s_groupsheet_menu) { menu_layer_destroy(s_groupsheet_menu); s_groupsheet_menu = NULL; }
  if (s_gs_chrome_layer) { layer_destroy(s_gs_chrome_layer);      s_gs_chrome_layer = NULL; }
  s_groupsheet_target = -1;
  touch_service_unsubscribe();
  touch_service_subscribe(touch_players_handler, NULL);
}

static void groupsheet_window_push(int target_idx) {
  if (target_idx < 0 || target_idx >= s_players_count) return;
  s_groupsheet_target = target_idx;
  gs_build_rows(target_idx);

  if (!s_groupsheet_window) {
    s_groupsheet_window = window_create();
    window_set_background_color(s_groupsheet_window, GColorBlack);
    window_set_window_handlers(s_groupsheet_window, (WindowHandlers){
        .load   = groupsheet_window_load,
        .unload = groupsheet_window_unload,
    });
  }
  window_stack_push(s_groupsheet_window, true);
}

// ─── Volume window ────────────────────────────────────────────────────────
//
// Two visual layouts:
//   A. Solo / non-group  — single-row legacy layout (centred big percentage +
//      chunky bar).  Unchanged from v1.0.
//   B. Group context      — two stacked rows.  Top row = Group volume, bottom
//      row = the individual control player.  One row has focus at a time;
//      UP/DOWN step the focused row, SELECT mutes the focused row, BACK pops.
//      Touch on any pixel in a row's vertical band changes focus.

typedef enum {
  VOL_FOCUS_GROUP = 0,
  VOL_FOCUS_INDIV = 1,
} VolFocus;

static VolFocus s_vol_focus = VOL_FOCUS_GROUP;

// Layout constants for the two-row variant.  Total = 28 + 2 + 84 + 2 + 84 + 28
// = 228 = SCREEN_H.  Header strip mirrors the now-playing chrome height; the
// hint strip leaves room for two lines of 14-pt help text.
#define V2_HEADER_H   28
#define V2_HEADER_Y   0
#define V2_DIV1_Y     28
#define V2_DIV1_H     2
#define V2_GROUP_Y    30
#define V2_ROW_H      84
#define V2_DIV2_Y     114
#define V2_DIV2_H     2
#define V2_INDIV_Y    116
#define V2_HINT_Y     200
#define V2_HINT_H     28

static bool in_group_volume_mode(void) {
  return s_now.group_volume >= 0 && s_now.control_player_id[0] != '\0';
}

// Draw a single row of the two-row volume window.
//   row_y      = top pixel of the 84 px row band
//   focused    = is this the currently selected row?
//   muted      = is THIS row muted (group_muted or s_now.muted)?
//   percent    = 0..100 reading for THIS row's bar
//   title      = "Group" / control player name
static void draw_vol_row(GContext *ctx, int16_t row_y, bool focused,
                         bool muted, uint8_t percent, const char *title) {
  int16_t row_w = SCREEN_W;

  // 4 px accent strip on the left edge.  Cerulean when focused, dark grey
  // otherwise — this is the primary "which row has focus" affordance.
  GColor accent = focused ? GColorVividCerulean : GColorDarkGray;
  graphics_context_set_fill_color(ctx, accent);
  graphics_fill_rect(ctx, GRect(0, row_y, 4, V2_ROW_H), 0, GCornerNone);

  // Title (left).  Focused = white bold 18, unfocused = light grey bold 18.
  GColor title_col = focused ? GColorWhite : GColorLightGray;
  graphics_context_set_text_color(ctx, title_col);
  graphics_draw_text(ctx, title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(12, row_y + 8, row_w - 80, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Percentage readout (right).  Focused = white bold 24; unfocused = dim
  // grey 18.  When muted we replace the number with "MUTED" in chrome yellow
  // — keep the colour even in the unfocused row so the user can see at a
  // glance that the inactive row is muted.
  char volbuf[8];
  const char *fnt;
  GColor pct_col;
  if (muted) {
    snprintf(volbuf, sizeof(volbuf), "MUTED");
    pct_col = GColorChromeYellow;
    fnt = focused ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18_BOLD;
  } else {
    snprintf(volbuf, sizeof(volbuf), "%u%%", (unsigned)percent);
    pct_col = focused ? GColorWhite : GColorLightGray;
    fnt = focused ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD;
  }
  graphics_context_set_text_color(ctx, pct_col);
  graphics_draw_text(ctx, volbuf,
                     fonts_get_system_font(fnt),
                     GRect(row_w - 76, row_y + 4, 68, 32),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);

  // Chunky bar.  Rail in dark grey; fill cerulean when focused, GColorBlueMoon
  // when unfocused.  Muted forces fill width to 0 so the bar reads "empty".
  int16_t bar_x = 12;
  int16_t bar_y = row_y + 50;
  int16_t bar_w = row_w - 24;
  int16_t bar_h = 14;
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(bar_x, bar_y, bar_w, bar_h), 4, GCornersAll);
  if (!muted && percent > 0) {
    int16_t fill_w = (int16_t)((uint32_t)bar_w * percent / 100);
    if (fill_w < 8) fill_w = 8; // keep corner radius visible
    GColor fill_col = focused ? GColorVividCerulean : GColorBlueMoon;
    graphics_context_set_fill_color(ctx, fill_col);
    graphics_fill_rect(ctx, GRect(bar_x, bar_y, fill_w, bar_h), 4, GCornersAll);
  }
}

// Two-row variant draw proc.
static void volume_root_update_group(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Header strip — control player name + chain glyph when in a group.
  GRect header = GRect(0, V2_HEADER_Y, SCREEN_W, V2_HEADER_H);
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, header, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  // Reserve 22 px on the right for the chain badge.
  graphics_draw_text(ctx, s_now.player_name,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(8, V2_HEADER_Y + 3, SCREEN_W - 30, V2_HEADER_H - 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  icon_chain(ctx, GRect(SCREEN_W - 22, V2_HEADER_Y + 8, 16, 12), GColorWhite);

  // Divider beneath the header.
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(0, V2_DIV1_Y, SCREEN_W, V2_DIV1_H), 0, GCornerNone);

  // Row 1 — Group.
  uint8_t group_pct = (s_now.group_volume < 0) ? 0
                     : (s_now.group_volume > 100 ? 100 : (uint8_t)s_now.group_volume);
  draw_vol_row(ctx, V2_GROUP_Y, s_vol_focus == VOL_FOCUS_GROUP,
               s_now.group_muted, group_pct, "Group");

  // Row separator (2 px).
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(0, V2_DIV2_Y, SCREEN_W, V2_DIV2_H), 0, GCornerNone);

  // Row 2 — Individual.  Title is the control player's display name (held in
  // s_now.player_name, which the bugfix now sources from the CONTROL player
  // not the master).
  draw_vol_row(ctx, V2_INDIV_Y, s_vol_focus == VOL_FOCUS_INDIV,
               s_now.muted, s_now.volume, s_now.player_name);

  // Hint strip.
  GRect hint = GRect(0, V2_HINT_Y, SCREEN_W, V2_HINT_H);
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, hint, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, "UP/DOWN volume - SELECT mutes",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, V2_HINT_Y + 2, SCREEN_W, 14),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, "tap to switch",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, V2_HINT_Y + 13, SCREEN_W, 14),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Single-row legacy layout — used when the control player is solo / not in a
// group.  Unchanged behaviour from v1.0.
static void volume_root_update_single(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Title.
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "Volume",
                     fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                     GRect(0, 8, bounds.size.w, 32),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Big readout.
  char buf[16];
  if (s_now.muted) {
    snprintf(buf, sizeof(buf), "MUTE");
  } else {
    snprintf(buf, sizeof(buf), "%u %%", (unsigned)s_now.volume);
  }
  graphics_context_set_text_color(ctx, s_now.muted ? GColorRed : GColorWhite);
  graphics_draw_text(ctx, buf,
                     fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD),
                     GRect(0, 56, bounds.size.w, 56),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Chunky bar.
  int16_t bar_x = 18;
  int16_t bar_y = 124;
  int16_t bar_w = bounds.size.w - 36;
  int16_t bar_h = 14;
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(bar_x, bar_y, bar_w, bar_h), 4, GCornersAll);
  if (!s_now.muted && s_now.volume > 0) {
    int16_t fill_w = (int16_t)((uint32_t)bar_w * s_now.volume / 100);
    graphics_context_set_fill_color(ctx, GColorVividCerulean);
    graphics_fill_rect(ctx, GRect(bar_x, bar_y, fill_w, bar_h), 4, GCornersAll);
  }

  icon_speaker(ctx, GRect(bar_x - 4, bar_y - 4, 24, 22), GColorWhite, s_now.muted);

  // Hint.
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, "UP / DOWN - SELECT mutes",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, bounds.size.h - 26, bounds.size.w, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Layout-switch dispatcher.
static void volume_root_update(Layer *layer, GContext *ctx) {
  if (in_group_volume_mode()) {
    volume_root_update_group(layer, ctx);
  } else {
    volume_root_update_single(layer, ctx);
  }
}

static void btn_vol_select(ClickRecognizerRef r, void *c) {
  if (in_group_volume_mode() && s_vol_focus == VOL_FOCUS_GROUP) {
    send_simple_cmd(CMD_GROUP_MUTE_TOGGLE);
  } else {
    send_simple_cmd(CMD_MUTE_TOGGLE);
  }
}
static void btn_vol_up(ClickRecognizerRef r, void *c) {
  if (in_group_volume_mode() && s_vol_focus == VOL_FOCUS_GROUP) {
    send_simple_cmd(CMD_GROUP_VOLUME_UP);
  } else {
    send_simple_cmd(CMD_VOLUME_UP);
  }
}
static void btn_vol_down(ClickRecognizerRef r, void *c) {
  if (in_group_volume_mode() && s_vol_focus == VOL_FOCUS_GROUP) {
    send_simple_cmd(CMD_GROUP_VOLUME_DOWN);
  } else {
    send_simple_cmd(CMD_VOLUME_DOWN);
  }
}

static void volume_click_config(void *ctx) {
  // SELECT short only — long-press is reserved (spec §6.1.2).
  window_single_click_subscribe(BUTTON_ID_SELECT, btn_vol_select);
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   150, btn_vol_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, btn_vol_down);
}

// Touch handler — only meaningful in group mode.  Tap inside either row's
// vertical band sets focus to that row; header / hint / dividers ignored.
static void touch_volume_handler(const TouchEvent *e, void *ctx_) {
  if (!in_group_volume_mode()) return;
  switch (e->type) {
    case TouchEvent_Touchdown:
      s_touch.phase = TG_TRACKING;
      s_touch.start_x = e->x; s_touch.start_y = e->y;
      s_touch.last_x  = e->x; s_touch.last_y  = e->y;
      s_touch.start_ms = time_ms(NULL, NULL);
      break;
    case TouchEvent_PositionUpdate:
      s_touch.last_x = e->x; s_touch.last_y = e->y;
      break;
    case TouchEvent_Liftoff: {
      if (s_touch.phase != TG_TRACKING) break;
      int16_t dx = s_touch.last_x - s_touch.start_x;
      int16_t dy = s_touch.last_y - s_touch.start_y;
      uint32_t dt = time_ms(NULL, NULL) - s_touch.start_ms;
      bool is_tap = (dx > -TAP_SLOP_PX && dx < TAP_SLOP_PX &&
                     dy > -TAP_SLOP_PX && dy < TAP_SLOP_PX &&
                     dt < TAP_MAX_MS);
      s_touch.phase = TG_IDLE;
      if (!is_tap) break;
      // Hit-test the LIFT-OFF coordinate (most touchscreens release within
      // the original target).  Header / divider / hint = no-op.
      int16_t y = s_touch.start_y;
      VolFocus next = s_vol_focus;
      if (y >= V2_GROUP_Y && y < V2_DIV2_Y) {
        next = VOL_FOCUS_GROUP;
      } else if (y >= V2_INDIV_Y && y < V2_HINT_Y) {
        next = VOL_FOCUS_INDIV;
      }
      if (next != s_vol_focus) {
        s_vol_focus = next;
        if (s_volume_root_layer) layer_mark_dirty(s_volume_root_layer);
      }
      break;
    }
  }
}

static void volume_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_volume_root_layer = root;
  // Group always gets focus on open (spec §6.1.2).
  s_vol_focus = VOL_FOCUS_GROUP;
  layer_set_update_proc(root, volume_root_update);
  window_set_click_config_provider(w, volume_click_config);
  touch_service_unsubscribe();
  touch_service_subscribe(touch_volume_handler, NULL);
}
static void volume_window_unload(Window *w) {
  s_volume_root_layer = NULL;
  touch_service_unsubscribe();
  touch_service_subscribe(touch_now_handler, NULL);
}

static void volume_window_push(void) {
  if (!s_volume_window) {
    s_volume_window = window_create();
    window_set_background_color(s_volume_window, GColorBlack);
    window_set_window_handlers(s_volume_window, (WindowHandlers){
        .load = volume_window_load,
        .unload = volume_window_unload,
    });
  }
  window_stack_push(s_volume_window, true);
}

// ─── Quick Play window ────────────────────────────────────────────────────

static uint16_t qk_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_quick_count > 0 ? s_quick_count : 1;
}

static int16_t qk_get_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return 36; }

static void qk_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  if (s_quick_count == 0) {
    menu_cell_basic_draw(ctx, cell, "No shortcuts", "Add in Settings.", NULL);
    return;
  }
  const QuickSlot *q = &s_quick_slots[idx->row];
  menu_cell_basic_draw(ctx, cell, q->label, NULL, NULL);
}

static void qk_select(MenuLayer *ml, MenuIndex *idx, void *c) {
  if (s_quick_count == 0) return;
  const QuickSlot *q = &s_quick_slots[idx->row];
  LOGI("quick play: %s -> %s", q->label, q->uri);
  send_quick_play(q->uri);
  window_stack_pop(true);
}

static void touch_quick_handler(const TouchEvent *e, void *ctx) {
  if (!s_quick_menu || s_quick_count == 0) return;
  int row = e->y / 36;
  if (row < 0) row = 0;
  if (row >= s_quick_count) row = s_quick_count - 1;

  if (e->type == TouchEvent_Touchdown || e->type == TouchEvent_PositionUpdate) {
    MenuIndex idx = { 0, (uint16_t)row };
    menu_layer_set_selected_index(s_quick_menu, idx, MenuRowAlignCenter, false);
  } else if (e->type == TouchEvent_Liftoff) {
    int16_t dx = e->x - s_touch.start_x;
    int16_t dy = e->y - s_touch.start_y;
    uint32_t dt = time_ms(NULL, NULL) - s_touch.start_ms;
    bool is_tap = (dx > -TAP_SLOP_PX && dx < TAP_SLOP_PX &&
                   dy > -TAP_SLOP_PX && dy < TAP_SLOP_PX &&
                   dt < TAP_MAX_MS);
    if (is_tap) {
      MenuIndex idx = { 0, (uint16_t)row };
      qk_select(s_quick_menu, &idx, NULL);
    }
  }
  if (e->type == TouchEvent_Touchdown) {
    s_touch.start_x = e->x; s_touch.start_y = e->y;
    s_touch.start_ms = time_ms(NULL, NULL);
  }
}

static void quick_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect bounds = layer_get_bounds(root);
  s_quick_menu = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_quick_menu, NULL, (MenuLayerCallbacks){
      .get_num_rows    = qk_get_num_rows,
      .get_cell_height = qk_get_cell_height,
      .draw_row        = qk_draw_row,
      .select_click    = qk_select,
  });
  menu_layer_set_click_config_onto_window(s_quick_menu, w);
  menu_layer_set_normal_colors(s_quick_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_quick_menu, GColorVividCerulean, GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_quick_menu));
  touch_service_unsubscribe();
  touch_service_subscribe(touch_quick_handler, NULL);
}
static void quick_window_unload(Window *w) {
  menu_layer_destroy(s_quick_menu);
  s_quick_menu = NULL;
  touch_service_unsubscribe();
  touch_service_subscribe(touch_now_handler, NULL);
}

static void quick_window_push(void) {
  if (!s_quick_window) {
    s_quick_window = window_create();
    window_set_window_handlers(s_quick_window, (WindowHandlers){
        .load = quick_window_load,
        .unload = quick_window_unload,
    });
  }
  window_stack_push(s_quick_window, true);
}

// ─── AppMessage inbox ─────────────────────────────────────────────────────

static void copy_tuple_str(DictionaryIterator *iter, uint32_t key, char *dst, size_t cap) {
  Tuple *t = dict_find(iter, key);
  if (!t) return;
  strncpy(dst, t->value->cstring, cap - 1);
  dst[cap - 1] = '\0';
}

static int32_t tuple_int(DictionaryIterator *iter, uint32_t key, int32_t fallback) {
  Tuple *t = dict_find(iter, key);
  return t ? t->value->int32 : fallback;
}

// Emulator-only canonical mock setup.  Triggered by
// `pebble send-app-message --emulator emery --int 10000=99` (10000 is the
// resource ID of ARG_INT in this app's manifest — confirm via build artifacts
// if it ever changes).  Populates s_players[] with a 4-player layout matching
// the v0.3.0 visual contract: Living Room master, Kitchen + Back garden
// members, Bedroom solo idle, Office solo paused.
static void debug_seed_players(void) {
  s_players_count = 0;

  // Row 0: Living Room — master with 2 members (Kitchen + Back garden).
  strncpy(s_players[0].id,        "lr",          MAX_PLAYER_ID);
  strncpy(s_players[0].name,      "Living Room", MAX_PLAYER_NAME);
  s_players[0].state       = PS_PLAYING;
  s_players[0].synced_to[0] = '\0';
  s_players[0].group_count = 3;
  s_players[0].flags       = ROW_FLAG_MASTER;

  // Row 1: Kitchen — member of Living Room.
  strncpy(s_players[1].id,        "kt",      MAX_PLAYER_ID);
  strncpy(s_players[1].name,      "Kitchen", MAX_PLAYER_NAME);
  s_players[1].state       = PS_PLAYING;
  strncpy(s_players[1].synced_to, "lr", MAX_PLAYER_ID);
  s_players[1].group_count = 0;
  s_players[1].flags       = ROW_FLAG_MEMBER;

  // Row 2: Back garden — member of Living Room.
  strncpy(s_players[2].id,        "bg",          MAX_PLAYER_ID);
  strncpy(s_players[2].name,      "Back garden", MAX_PLAYER_NAME);
  s_players[2].state       = PS_PLAYING;
  strncpy(s_players[2].synced_to, "lr", MAX_PLAYER_ID);
  s_players[2].group_count = 0;
  s_players[2].flags       = ROW_FLAG_MEMBER;

  // Row 3: Bedroom — solo, idle.
  strncpy(s_players[3].id,        "bd",      MAX_PLAYER_ID);
  strncpy(s_players[3].name,      "Bedroom", MAX_PLAYER_NAME);
  s_players[3].state       = PS_IDLE;
  s_players[3].synced_to[0] = '\0';
  s_players[3].group_count = 0;
  s_players[3].flags       = 0;

  // Row 4: Office — solo, paused.
  strncpy(s_players[4].id,        "of",     MAX_PLAYER_ID);
  strncpy(s_players[4].name,      "Office", MAX_PLAYER_NAME);
  s_players[4].state       = PS_PAUSED;
  s_players[4].synced_to[0] = '\0';
  s_players[4].group_count = 0;
  s_players[4].flags       = 0;

  s_players_count = 5;

  // Also seed a now-playing snapshot so the header looks normal.
  strncpy(s_now.player_name, "Living Room", MAX_PLAYER_NAME);
  strncpy(s_now.title,       "Demo Track",  MAX_TRACK_TEXT);
  strncpy(s_now.artist,      "Demo Artist", MAX_TRACK_TEXT);
  strncpy(s_now.album,       "Demo Album",  MAX_TRACK_TEXT);
  s_now.state    = PS_PLAYING;
  s_now.volume   = 42;
  s_now.muted    = false;
  s_now.shuffle  = false;
  s_now.repeat   = RM_OFF;
  s_now.elapsed_s  = 75;
  s_now.duration_s = 240;
  s_now.connected = true;
  s_now.last_error[0] = '\0';

  LOGI("debug seed: 5 players (1 master + 2 members + 2 solo)");
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  // Emulator-only debug seed.  Sent via
  // `pebble send-app-message --emulator emery --int 10000=99`, where 10000 is
  // the resource ID for CMD in this app's manifest and 99 is CMD_DEBUG_SEED.
  // Don't gate on a #define — leaving the command compiled but unreachable on
  // real hardware (the JS bridge never forwards 99) is the simplest contract.
  Tuple *cmd_t = dict_find(iter, MESSAGE_KEY_CMD);
  if (cmd_t && cmd_t->value->int32 == CMD_DEBUG_SEED) {
    debug_seed_players();
    // Open the players window if it's not already on top so screenshots
    // capture the canonical group layout without manual navigation.
    if (!s_players_window || window_stack_get_top_window() != s_players_window) {
      players_window_push();
    }
    if (s_players_menu) menu_layer_reload_data(s_players_menu);
    if (s_now_root_layer) layer_mark_dirty(s_now_root_layer);
    return;
  }
  if (cmd_t && cmd_t->value->int32 == CMD_DEBUG_OPEN_VOL) {
    // Look at ARG_INT to pick a focus override — 0 = group (default),
    // 1 = individual.  We mutate s_vol_focus AFTER push so the window
    // load proc's reset doesn't clobber the test setting.
    volume_window_push();
    Tuple *focus_t = dict_find(iter, MESSAGE_KEY_ARG_INT);
    if (focus_t && focus_t->value->int32 == 1) {
      s_vol_focus = VOL_FOCUS_INDIV;
    } else {
      s_vol_focus = VOL_FOCUS_GROUP;
    }
    if (s_volume_root_layer) layer_mark_dirty(s_volume_root_layer);
    return;
  }

  Tuple *err = dict_find(iter, MESSAGE_KEY_ST_ERROR);
  if (err) {
    strncpy(s_now.last_error, err->value->cstring, sizeof(s_now.last_error) - 1);
    s_now.last_error[sizeof(s_now.last_error) - 1] = '\0';
    s_now.connected = false;
    LOGE("phone error: %s", s_now.last_error);
  }
  if (dict_find(iter, MESSAGE_KEY_ST_OK)) {
    s_now.connected = true;
    s_now.last_error[0] = '\0';
  }

  // Player-list chunks.
  Tuple *pl_begin = dict_find(iter, MESSAGE_KEY_PLAYERS_BEGIN);
  if (pl_begin) {
    s_players_count = 0;
    LOGI("players list reset");
  }
  Tuple *pl_chunk_idx = dict_find(iter, MESSAGE_KEY_PLAYER_ROW_INDEX);
  if (pl_chunk_idx) {
    int idx = pl_chunk_idx->value->int32;
    if (idx >= 0 && idx < MAX_PLAYERS) {
      copy_tuple_str(iter, MESSAGE_KEY_PLAYER_ROW_ID,   s_players[idx].id,   MAX_PLAYER_ID);
      copy_tuple_str(iter, MESSAGE_KEY_PLAYER_ROW_NAME, s_players[idx].name, MAX_PLAYER_NAME);
      int32_t st = tuple_int(iter, MESSAGE_KEY_PLAYER_ROW_STATE, PS_UNKNOWN);
      s_players[idx].state = (PlaybackState)st;
      // Reset group fields first so a stale row doesn't carry over if the
      // phone now reports this player solo (the synced_to key won't fire
      // copy_tuple_str if the phone sends "" omitted in some future opt).
      s_players[idx].synced_to[0] = '\0';
      s_players[idx].group_count  = 0;
      s_players[idx].flags        = 0;
      copy_tuple_str(iter, MESSAGE_KEY_PLAYER_ROW_SYNCED_TO,
                     s_players[idx].synced_to, MAX_PLAYER_ID);
      s_players[idx].group_count = (uint8_t)tuple_int(iter, MESSAGE_KEY_PLAYER_ROW_GROUP_COUNT, 0);
      s_players[idx].flags       = (uint8_t)tuple_int(iter, MESSAGE_KEY_PLAYER_ROW_FLAGS, 0);
      if (idx + 1 > s_players_count) s_players_count = idx + 1;
    }
  }
  if (dict_find(iter, MESSAGE_KEY_PLAYERS_END) && s_players_menu) {
    menu_layer_reload_data(s_players_menu);
  }

  // Quick Play chunks.
  if (dict_find(iter, MESSAGE_KEY_QUICK_BEGIN)) {
    s_quick_recv_count = 0;
    LOGI("quick slots reset");
  }
  Tuple *qk_idx = dict_find(iter, MESSAGE_KEY_QUICK_ROW_INDEX);
  if (qk_idx) {
    int idx = qk_idx->value->int32;
    if (idx >= 0 && idx < MAX_QUICK_SLOTS) {
      copy_tuple_str(iter, MESSAGE_KEY_QUICK_ROW_LABEL, s_quick_slots[idx].label, MAX_QUICK_LABEL);
      copy_tuple_str(iter, MESSAGE_KEY_QUICK_ROW_URI,   s_quick_slots[idx].uri,   MAX_QUICK_URI);
      if (idx + 1 > s_quick_recv_count) s_quick_recv_count = idx + 1;
    }
  }
  if (dict_find(iter, MESSAGE_KEY_QUICK_END)) {
    s_quick_count = s_quick_recv_count;
    quick_save_to_persist();
    LOGI("quick slots saved: %d", s_quick_count);
    if (s_quick_menu) menu_layer_reload_data(s_quick_menu);
  }

  // Now-playing state snapshot.
  copy_tuple_str(iter, MESSAGE_KEY_ST_PLAYER_NAME, s_now.player_name, MAX_PLAYER_NAME);
  copy_tuple_str(iter, MESSAGE_KEY_ST_TITLE,       s_now.title,       MAX_TRACK_TEXT);
  copy_tuple_str(iter, MESSAGE_KEY_ST_ARTIST,      s_now.artist,      MAX_TRACK_TEXT);
  copy_tuple_str(iter, MESSAGE_KEY_ST_ALBUM,       s_now.album,       MAX_TRACK_TEXT);

  Tuple *t = dict_find(iter, MESSAGE_KEY_ST_STATE);
  if (t)  s_now.state    = (PlaybackState)t->value->int32;
  t = dict_find(iter, MESSAGE_KEY_ST_VOLUME);
  if (t)  s_now.volume   = (uint8_t)t->value->int32;
  t = dict_find(iter, MESSAGE_KEY_ST_MUTED);
  if (t)  s_now.muted    = t->value->int32 != 0;
  t = dict_find(iter, MESSAGE_KEY_ST_SHUFFLE);
  if (t)  s_now.shuffle  = t->value->int32 != 0;
  t = dict_find(iter, MESSAGE_KEY_ST_REPEAT);
  if (t)  s_now.repeat   = (RepeatMode)t->value->int32;
  t = dict_find(iter, MESSAGE_KEY_ST_ELAPSED);
  if (t)  s_now.elapsed_s = (uint32_t)t->value->int32;
  t = dict_find(iter, MESSAGE_KEY_ST_DURATION);
  if (t)  s_now.duration_s = (uint32_t)t->value->int32;

  // v1.1 keys — control player id (volume target) + group volume mirror.
  copy_tuple_str(iter, MESSAGE_KEY_ST_CONTROL_PLAYER_ID,
                 s_now.control_player_id, MAX_PLAYER_ID);
  t = dict_find(iter, MESSAGE_KEY_ST_GROUP_VOLUME);
  if (t)  s_now.group_volume = (int16_t)t->value->int32;
  t = dict_find(iter, MESSAGE_KEY_ST_GROUP_MUTED);
  if (t)  s_now.group_muted = t->value->int32 != 0;

  if (s_now_root_layer) layer_mark_dirty(s_now_root_layer);
  if (s_volume_root_layer) layer_mark_dirty(s_volume_root_layer);
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  LOGE("inbox dropped: %d", reason);
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  LOGE("outbox failed: %d", reason);
}

// ─── Now-playing window lifecycle ─────────────────────────────────────────

static void now_window_appear(Window *w) {
  // Re-arm tick when we come back to the front (sub-window popped, etc).
  s_now_visible = true;
  touch_service_subscribe(touch_now_handler, NULL);
  schedule_tick();
}

static void now_window_disappear(Window *w) {
  // Sub-window pushed on top OR app backgrounded: stop the redraw loop.
  s_now_visible = false;
  cancel_tick();
  touch_service_unsubscribe();
}

static void now_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_now_root_layer = root;
  layer_set_update_proc(root, now_root_update);
  window_set_click_config_provider(w, now_click_config);
}

static void now_window_unload(Window *w) {
  cancel_tick();
}

// ─── App lifecycle ────────────────────────────────────────────────────────

static void init(void) {
  quick_load_from_persist();

  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  app_message_register_outbox_failed(outbox_failed);
  app_message_open(app_message_inbox_size_maximum(),
                   app_message_outbox_size_maximum());

  s_now_window = window_create();
  window_set_background_color(s_now_window, GColorBlack);
  window_set_window_handlers(s_now_window, (WindowHandlers){
      .load     = now_window_load,
      .unload   = now_window_unload,
      .appear   = now_window_appear,
      .disappear= now_window_disappear,
  });
  window_stack_push(s_now_window, true);

  // Pause our redraw / poll loop while the screen is off or another app is on
  // top — otherwise the 1 Hz tick keeps the watch awake and trips the
  // system watchdog after the display blanks.
  app_focus_service_subscribe_handlers((AppFocusHandlers){
      .did_focus = app_focus_handler,
  });

  send_simple_cmd(CMD_REQUEST_STATE);
}

static void deinit(void) {
  window_destroy(s_now_window);
  if (s_players_window)    window_destroy(s_players_window);
  if (s_volume_window)     window_destroy(s_volume_window);
  if (s_quick_window)      window_destroy(s_quick_window);
  if (s_groupsheet_window) window_destroy(s_groupsheet_window);
}

int main(void) {
  init();
  LOGI("Music Assistant remote v0.3.0 started.");
  app_event_loop();
  deinit();
  return 0;
}
