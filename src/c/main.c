/*
 * pebble-musicassistant-remote
 *
 * Native Pebble Time 2 (emery) watchapp.  All Music Assistant traffic happens
 * on the phone (src/pkjs/index.js); we just render state and forward user
 * intent over AppMessage.
 *
 * Two windows live on the stack:
 *
 *   1. Now-playing — default.  Status bar at the bottom is a touch target that
 *      pushes the player list.
 *   2. Player list — touch-driven row picker built on MenuLayer plus a raw
 *      TouchService overlay (MenuLayer has no native touch support on emery).
 *
 * Touch is derived from raw TouchService events: Touchdown remembers the
 * coordinates and timestamp, Liftoff resolves the gesture.  Anything within
 * TAP_SLOP_PX and TAP_MAX_MS counts as a tap; everything else is currently
 * ignored (drag/swipe land in 0.2.0).
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

#define STATUSBAR_H 28
#define TRANSPORT_H 56
#define TITLE_TOP   8
#define TITLE_H     32
#define META_TOP    44
#define META_H      24
#define PROGRESS_TOP   132
#define PROGRESS_BAR_H 4
#define TIME_TOP    142

#define TRANSPORT_TOP   72
#define TRANSPORT_BTN_W 56
#define TRANSPORT_GAP   8

#define SHUFFLE_TOP 184
#define ICON_BTN_W  48

#define STATUSBAR_Y (SCREEN_H - STATUSBAR_H)

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

typedef struct {
  char          id[MAX_PLAYER_ID];
  char          name[MAX_PLAYER_NAME];
  PlaybackState state;
} PlayerRow;

static struct {
  char          player_name[MAX_PLAYER_NAME];
  char          title[MAX_TRACK_TEXT];
  char          artist[MAX_TRACK_TEXT];
  char          album[MAX_TRACK_TEXT];
  PlaybackState state;
  uint8_t       volume;    // 0..100
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

// Local interpolation: bump elapsed once per second while PLAYING so the
// progress bar doesn't stall between phone polls.
static AppTimer *s_tick_timer;
#define TICK_MS 1000

// Player list buffer; populated by chunked AppMessages from the phone.
static PlayerRow s_players[MAX_PLAYERS];
static int       s_players_count;

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
};

// ─── Windows ──────────────────────────────────────────────────────────────

static Window     *s_now_window;
static Layer      *s_now_root_layer;

static Window     *s_players_window;
static MenuLayer  *s_players_menu;

// ─── Helpers ──────────────────────────────────────────────────────────────

static const char *repeat_glyph(RepeatMode r) {
  switch (r) {
    case RM_OFF: return "\xE2\x86\xBB"; // ↻ dim
    case RM_ALL: return "\xE2\x86\xBB";
    case RM_ONE: return "1";
    default:     return "?";
  }
}

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

// ─── Rendering — now-playing window ───────────────────────────────────────

static void draw_progress_bar(GContext *ctx, GRect bounds) {
  int16_t bar_x = 8;
  int16_t bar_w = bounds.size.w - 16;
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

static void draw_volume_pips(GContext *ctx, int16_t x, int16_t y, int16_t w, int16_t h, uint8_t v) {
  int n = 10;
  int16_t pip_w = (w - (n - 1)) / n;
  for (int i = 0; i < n; i++) {
    GRect r = GRect(x + i * (pip_w + 1), y, pip_w, h);
    bool on = (i * 10) < v;
    graphics_context_set_fill_color(ctx, on ? GColorWhite : GColorDarkGray);
    graphics_fill_rect(ctx, r, 0, GCornerNone);
  }
}

static void now_root_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Background
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Title
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_now.title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(8, TITLE_TOP, bounds.size.w - 16, TITLE_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Artist — Album
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
                     GRect(8, META_TOP, bounds.size.w - 16, META_H),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Transport row (centre prev / play|pause / next)
  int16_t group_w = TRANSPORT_BTN_W * 3 + TRANSPORT_GAP * 2;
  int16_t group_x = (bounds.size.w - group_w) / 2;
  GRect r_prev = GRect(group_x,                                 TRANSPORT_TOP, TRANSPORT_BTN_W, TRANSPORT_H);
  GRect r_pp   = GRect(group_x + (TRANSPORT_BTN_W + TRANSPORT_GAP),     TRANSPORT_TOP, TRANSPORT_BTN_W, TRANSPORT_H);
  GRect r_next = GRect(group_x + (TRANSPORT_BTN_W + TRANSPORT_GAP) * 2, TRANSPORT_TOP, TRANSPORT_BTN_W, TRANSPORT_H);

  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_draw_round_rect(ctx, r_prev, 6);
  graphics_draw_round_rect(ctx, r_pp,   6);
  graphics_draw_round_rect(ctx, r_next, 6);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, "\xE2\x8F\xAE", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     r_prev, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, s_now.state == PS_PLAYING ? "\xE2\x8F\xB8" : "\xE2\x96\xB6",
                     fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     r_pp,   GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, "\xE2\x8F\xAD", fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     r_next, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Progress bar + times
  draw_progress_bar(ctx, bounds);

  char times[40];
  unsigned em = s_now.elapsed_s  / 60, es = s_now.elapsed_s  % 60;
  unsigned dm = s_now.duration_s / 60, ds = s_now.duration_s % 60;
  if (em > 999) em = 999;
  if (dm > 999) dm = 999;
  snprintf(times, sizeof(times), "%u:%02u / %u:%02u", em, es, dm, ds);
  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx, times,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14),
                     GRect(8, TIME_TOP, bounds.size.w - 16, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  // Shuffle / repeat row
  int16_t shuf_x = 24;
  int16_t rep_x  = bounds.size.w - 24 - ICON_BTN_W;
  GRect r_shuf = GRect(shuf_x, SHUFFLE_TOP, ICON_BTN_W, 28);
  GRect r_rep  = GRect(rep_x,  SHUFFLE_TOP, ICON_BTN_W, 28);

  graphics_context_set_text_color(ctx, s_now.shuffle ? GColorVividCerulean : GColorLightGray);
  graphics_draw_text(ctx, "\xF0\x9F\x94\x80", fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     r_shuf, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_context_set_text_color(ctx, s_now.repeat == RM_OFF ? GColorLightGray : GColorVividCerulean);
  graphics_draw_text(ctx, repeat_glyph(s_now.repeat), fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     r_rep,  GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

  // Status bar (player name + volume) — tap target
  GRect status = GRect(0, STATUSBAR_Y, bounds.size.w, STATUSBAR_H);
  graphics_context_set_fill_color(ctx, GColorOxfordBlue);
  graphics_fill_rect(ctx, status, 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, s_now.player_name,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(8, STATUSBAR_Y + 4, bounds.size.w / 2, STATUSBAR_H - 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);

  int16_t pip_x = bounds.size.w / 2 + 4;
  int16_t pip_w = bounds.size.w - pip_x - 8;
  draw_volume_pips(ctx, pip_x, STATUSBAR_Y + 10, pip_w, 8,
                   s_now.muted ? 0 : s_now.volume);

  // Error banner (one-line) if anything
  if (s_now.last_error[0]) {
    GRect eb = GRect(0, 0, bounds.size.w, 16);
    graphics_context_set_fill_color(ctx, GColorDarkCandyAppleRed);
    graphics_fill_rect(ctx, eb, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, s_now.last_error,
                       fonts_get_system_font(FONT_KEY_GOTHIC_14),
                       GRect(4, 0, bounds.size.w - 8, 16),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
}

// ─── Hit testing ──────────────────────────────────────────────────────────

typedef enum {
  HIT_NONE,
  HIT_PREV,
  HIT_PLAYPAUSE,
  HIT_NEXT,
  HIT_SHUFFLE,
  HIT_REPEAT,
  HIT_STATUSBAR,
  HIT_VOLUME_READOUT,   // tapping the pips toggles mute
} Hit;

static Hit hit_test_now(int16_t x, int16_t y) {
  if (y >= STATUSBAR_Y) {
    if (x > SCREEN_W / 2) return HIT_VOLUME_READOUT;
    return HIT_STATUSBAR;
  }
  if (y >= TRANSPORT_TOP && y < TRANSPORT_TOP + TRANSPORT_H) {
    int16_t group_w = TRANSPORT_BTN_W * 3 + TRANSPORT_GAP * 2;
    int16_t group_x = (SCREEN_W - group_w) / 2;
    if (x >= group_x && x < group_x + TRANSPORT_BTN_W) return HIT_PREV;
    if (x >= group_x + TRANSPORT_BTN_W + TRANSPORT_GAP &&
        x <  group_x + TRANSPORT_BTN_W * 2 + TRANSPORT_GAP) return HIT_PLAYPAUSE;
    if (x >= group_x + (TRANSPORT_BTN_W + TRANSPORT_GAP) * 2 &&
        x <  group_x + (TRANSPORT_BTN_W + TRANSPORT_GAP) * 2 + TRANSPORT_BTN_W) return HIT_NEXT;
  }
  if (y >= SHUFFLE_TOP && y < SHUFFLE_TOP + 28) {
    if (x < SCREEN_W / 2) return HIT_SHUFFLE;
    return HIT_REPEAT;
  }
  return HIT_NONE;
}

// ─── Touch handling ───────────────────────────────────────────────────────

static void players_window_push(void);

static void handle_tap_now(int16_t x, int16_t y) {
  Hit h = hit_test_now(x, y);
  LOGD("tap (%d,%d) -> hit=%d", x, y, h);
  switch (h) {
    case HIT_PREV:           send_simple_cmd(CMD_PREVIOUS);      break;
    case HIT_PLAYPAUSE:      send_simple_cmd(CMD_PLAY_PAUSE);    break;
    case HIT_NEXT:           send_simple_cmd(CMD_NEXT);          break;
    case HIT_SHUFFLE:        send_simple_cmd(CMD_SHUFFLE_TOGGLE);break;
    case HIT_REPEAT:         send_simple_cmd(CMD_REPEAT_CYCLE);  break;
    case HIT_VOLUME_READOUT: send_simple_cmd(CMD_MUTE_TOGGLE);   break;
    case HIT_STATUSBAR:      send_simple_cmd(CMD_REQUEST_PLAYERS);
                             players_window_push();              break;
    default: break;
  }
}

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

// ─── Button handlers (fallback / parity) ──────────────────────────────────

static void btn_select(ClickRecognizerRef r, void *c) { send_simple_cmd(CMD_PLAY_PAUSE);  }
static void btn_up(ClickRecognizerRef r, void *c)     { send_simple_cmd(CMD_VOLUME_UP);   }
static void btn_down(ClickRecognizerRef r, void *c)   { send_simple_cmd(CMD_VOLUME_DOWN); }
static void btn_up_long(ClickRecognizerRef r, void *c){ send_simple_cmd(CMD_MUTE_TOGGLE); }

static void now_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, btn_select);
  window_single_click_subscribe(BUTTON_ID_UP,     btn_up);
  window_single_click_subscribe(BUTTON_ID_DOWN,   btn_down);
  window_long_click_subscribe(BUTTON_ID_UP, 500, btn_up_long, NULL);
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

static uint16_t pl_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return s_players_count > 0 ? s_players_count : 1;
}

static int16_t pl_get_cell_height(MenuLayer *ml, MenuIndex *idx, void *ctx) { return 36; }

static void pl_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *c) {
  if (s_players_count == 0) {
    menu_cell_basic_draw(ctx, cell, "No players", "Pull to refresh", NULL);
    return;
  }
  const PlayerRow *p = &s_players[idx->row];
  const char *sub =
      p->state == PS_PLAYING ? "Playing" :
      p->state == PS_PAUSED  ? "Paused"  :
      p->state == PS_IDLE    ? "Idle"    : "—";
  menu_cell_basic_draw(ctx, cell, p->name, sub, NULL);
}

static void pl_select(MenuLayer *ml, MenuIndex *idx, void *c) {
  if (s_players_count == 0) return;
  const PlayerRow *p = &s_players[idx->row];
  LOGI("select player %s (%s)", p->name, p->id);
  send_select_player(p->id);
  window_stack_pop(true);
}

// Forward raw touch on the player list to MenuLayer.  Taps select the row
// under the finger; PositionUpdate scrolls by setting the selected index based
// on Y position to give immediate visual feedback.
static void touch_players_handler(const TouchEvent *e, void *ctx) {
  if (!s_players_menu || s_players_count == 0) return;
  // Crude per-row hit: divide screen height by cell height starting at top.
  // Works for short lists; refined in 0.2.0 with scroll offset.
  int row = e->y / 36;
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
      .get_num_rows    = pl_get_num_rows,
      .get_cell_height = pl_get_cell_height,
      .draw_row        = pl_draw_row,
      .select_click    = pl_select,
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
  // Errors
  Tuple *err = dict_find(iter, MESSAGE_KEY_ST_ERROR);
  if (err) {
    strncpy(s_now.last_error, err->value->cstring, sizeof(s_now.last_error) - 1);
    s_now.last_error[sizeof(s_now.last_error) - 1] = '\0';
    s_now.connected = false;
    LOGE("phone error: %s", s_now.last_error);
  }
  // Top-level OK clears the error banner.
  if (dict_find(iter, MESSAGE_KEY_ST_OK)) {
    s_now.connected = true;
    s_now.last_error[0] = '\0';
  }

  // Player-list chunks
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

  // Now-playing state snapshot
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

  // Kick the phone for an initial state sync (the JS side sends READY first
  // anyway, this is just defence in depth).
  send_simple_cmd(CMD_REQUEST_STATE);
}

static void deinit(void) {
  window_destroy(s_now_window);
  if (s_players_window) window_destroy(s_players_window);
}

int main(void) {
  init();
  LOGI("Music Assistant remote started.");
  app_event_loop();
  deinit();
  return 0;
}
