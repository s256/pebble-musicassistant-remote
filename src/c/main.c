/*
 * pebble-musicassistant-remote — v0.2.0
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
} s_now = {
  .player_name = "—",
  .title       = "Not connected",
  .state       = PS_UNKNOWN,
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

static void schedule_tick(void);
static void tick_cb(void *ctx) {
  s_tick_timer = NULL;
  if (s_now.state == PS_PLAYING && s_now.elapsed_s < s_now.duration_s) {
    s_now.elapsed_s++;
    if (s_now_root_layer) layer_mark_dirty(s_now_root_layer);
  }
  schedule_tick();
}
static void schedule_tick(void) {
  if (s_tick_timer) return;
  s_tick_timer = app_timer_register(TICK_MS, tick_cb, NULL);
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
// so nothing overlaps.  Restored from v0.1.1 (the menu_cell_basic_draw
// shortcut that crept back in lost the dot + spacing).
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

  // Left dot: green = playing, amber = paused, grey = idle.
  GColor dot =
      p->state == PS_PLAYING ? GColorIslamicGreen :
      p->state == PS_PAUSED  ? GColorChromeYellow : GColorDarkGray;
  graphics_context_set_fill_color(ctx, dot);
  graphics_fill_circle(ctx, GPoint(b.origin.x + 14, b.origin.y + PL_ROW_H / 2), 5);

  // Name — always white on the row background (was black-on-black in the
  // regressed version), or white on the cerulean highlight.
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, p->name,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(b.origin.x + 28, b.origin.y + 4, b.size.w - 40, 24),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Subtitle: playback state, dim grey when inactive, near-white when highlighted.
  const char *sub =
      p->state == PS_PLAYING ? "Playing" :
      p->state == PS_PAUSED  ? "Paused"  :
      p->state == PS_IDLE    ? "Idle"    : "—";
  graphics_context_set_text_color(ctx, hi ? GColorPastelYellow : GColorLightGray);
  graphics_draw_text(ctx, sub,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(b.origin.x + 28, b.origin.y + 24, b.size.w - 40, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void pl_select(MenuLayer *ml, MenuIndex *idx, void *c) {
  if (s_players_count == 0) return;
  const PlayerRow *p = &s_players[idx->row];
  LOGI("select player %s (%s)", p->name, p->id);
  send_select_player(p->id);
  window_stack_pop(true);
}

static void touch_players_handler(const TouchEvent *e, void *ctx) {
  if (!s_players_menu || s_players_count == 0) return;
  int row = e->y / (PL_ROW_H + 1);   // include the 1-px separator
  if (row < 0) row = 0;
  if (row >= s_players_count) row = s_players_count - 1;

  if (e->type == TouchEvent_Touchdown || e->type == TouchEvent_PositionUpdate) {
    MenuIndex idx = { 0, (uint16_t)row };
    menu_layer_set_selected_index(s_players_menu, idx, MenuRowAlignCenter, false);
  } else if (e->type == TouchEvent_Liftoff) {
    int16_t dx = e->x - s_touch.start_x;
    int16_t dy = e->y - s_touch.start_y;
    uint32_t dt = time_ms(NULL, NULL) - s_touch.start_ms;
    bool is_tap = (dx > -TAP_SLOP_PX && dx < TAP_SLOP_PX &&
                   dy > -TAP_SLOP_PX && dy < TAP_SLOP_PX &&
                   dt < TAP_MAX_MS);
    if (is_tap) {
      MenuIndex idx = { 0, (uint16_t)row };
      pl_select(s_players_menu, &idx, NULL);
    }
  }
  if (e->type == TouchEvent_Touchdown) {
    s_touch.start_x = e->x; s_touch.start_y = e->y;
    s_touch.start_ms = time_ms(NULL, NULL);
  }
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
  menu_layer_set_click_config_onto_window(s_players_menu, w);
  menu_layer_set_normal_colors(s_players_menu, GColorBlack, GColorWhite);
  menu_layer_set_highlight_colors(s_players_menu, GColorVividCerulean, GColorWhite);
  layer_add_child(root, menu_layer_get_layer(s_players_menu));
  touch_service_unsubscribe();
  touch_service_subscribe(touch_players_handler, NULL);
}
static void players_window_unload(Window *w) {
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

// ─── Volume window ────────────────────────────────────────────────────────

static void volume_root_update(Layer *layer, GContext *ctx) {
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
  graphics_draw_text(ctx, "UP / DOWN — SELECT mutes",
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(0, bounds.size.h - 26, bounds.size.w, 20),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void btn_vol_select(ClickRecognizerRef r, void *c) {
  send_simple_cmd(CMD_MUTE_TOGGLE);
}
static void btn_vol_up(ClickRecognizerRef r, void *c)     { send_simple_cmd(CMD_VOLUME_UP);   }
static void btn_vol_down(ClickRecognizerRef r, void *c)   { send_simple_cmd(CMD_VOLUME_DOWN); }

static void volume_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, btn_vol_select);
  window_single_repeating_click_subscribe(BUTTON_ID_UP,   150, btn_vol_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 150, btn_vol_down);
}

static void volume_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_volume_root_layer = root;
  layer_set_update_proc(root, volume_root_update);
  window_set_click_config_provider(w, volume_click_config);
}
static void volume_window_unload(Window *w) {
  s_volume_root_layer = NULL;
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

static void inbox_received(DictionaryIterator *iter, void *ctx) {
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

static void now_window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_now_root_layer = root;
  layer_set_update_proc(root, now_root_update);
  window_set_click_config_provider(w, now_click_config);
  touch_service_subscribe(touch_now_handler, NULL);
  schedule_tick();
}

static void now_window_unload(Window *w) {
  if (s_tick_timer) { app_timer_cancel(s_tick_timer); s_tick_timer = NULL; }
  touch_service_unsubscribe();
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
      .load = now_window_load,
      .unload = now_window_unload,
  });
  window_stack_push(s_now_window, true);

  send_simple_cmd(CMD_REQUEST_STATE);
}

static void deinit(void) {
  window_destroy(s_now_window);
  if (s_players_window) window_destroy(s_players_window);
  if (s_volume_window)  window_destroy(s_volume_window);
  if (s_quick_window)   window_destroy(s_quick_window);
}

int main(void) {
  init();
  LOGI("Music Assistant remote v0.2.0 started.");
  app_event_loop();
  deinit();
  return 0;
}
