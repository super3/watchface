#include <pebble.h>
#include <ctype.h>

// ===========================================================================
// Casio-style watchface for the Pebble Time 2 (emery, 200x228, color)
// Pixel-accurate reconstruction of the reference design.
// ===========================================================================

static Window *s_window;
static Layer  *s_canvas_layer;
static GFont   s_lbl_font;       // small label font (Gothic 14 Bold)
static GFont   s_val_font;       // value font for dates (Gothic 18 Bold)
static GFont   s_big_font;       // step count (Gothic 28 Bold)

static int  s_hour = 0, s_min = 0;
static bool s_pm = false, s_h24 = false;
static char s_day_buf[8]    = "---";
static char s_date_buf[12]  = "--- --";
static char s_year_buf[8]   = "----";
static char s_steps_buf[12] = "0";
static char s_water_buf[16] = "-- OZ";
static int  s_battery = 0;
static bool s_connected = true;

// Water intake (oz), synced from the TapSip companion (com.watertracker.widget)
static int  s_water_today = -1;   // -1 = not yet known
static int  s_water_goal  = 64;
#define PKEY_WATER_TODAY 100
#define PKEY_WATER_GOAL  101

#define COL_BG    GColorWhite
#define COL_FG    GColorBlack

// ---------------------------------------------------------------------------
// Low-level framebuffer pixel access (emery = 8-bit color)
// ---------------------------------------------------------------------------
static inline void fb_px(GBitmap *fb, int x, int y, uint8_t argb) {
  GRect b = gbitmap_get_bounds(fb);
  if (x < b.origin.x || y < b.origin.y ||
      x >= b.origin.x + b.size.w || y >= b.origin.y + b.size.h) return;
  GBitmapDataRowInfo info = gbitmap_get_data_row_info(fb, y);
  if (x < info.min_x || x > info.max_x) return;
  info.data[x] = argb;
}

// ---------------------------------------------------------------------------
// 7-segment renderer (chamfered segments, dithered "off" ghost segments)
//   segment bits: a=1 b=2 c=4 d=8 e=16 f=32 g=64
// ---------------------------------------------------------------------------
static const uint8_t SEG7[10] = {
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

#define SEG_HT 2          // half thickness (segment is 2*HT+1 px thick)

static void plot(GBitmap *fb, int x, int y, bool lit, uint8_t on, uint8_t ghost) {
  if (lit) fb_px(fb, x, y, on);
  else if (((x + y) & 1) == 0) fb_px(fb, x, y, ghost);  // 50% dither
}

// horizontal segment: ends chamfer to a point at the vertical centre
static void seg_h(GBitmap *fb, int ox, int oy, int xl, int xr, int cy,
                  bool lit, uint8_t on, uint8_t ghost) {
  for (int dy = -SEG_HT; dy <= SEG_HT; dy++) {
    int a = ox + xl + abs(dy), b = ox + xr - abs(dy), y = oy + cy + dy;
    for (int x = a; x <= b; x++) plot(fb, x, y, lit, on, ghost);
  }
}

// vertical segment: ends chamfer to a point at the horizontal centre
static void seg_v(GBitmap *fb, int ox, int oy, int cx, int yt, int yb,
                  bool lit, uint8_t on, uint8_t ghost) {
  for (int y = yt; y <= yb; y++) {
    int de = (y - yt) < (yb - y) ? (y - yt) : (yb - y);
    int hw = de < SEG_HT ? de : SEG_HT;
    for (int dx = -hw; dx <= hw; dx++)
      plot(fb, ox + cx + dx, oy + y, lit, on, ghost);
  }
}

// Draw one digit cell (29 wide x 54 tall). val<0 => blank (all ghost).
static void draw_digit(GBitmap *fb, int ox, int oy, int val) {
  uint8_t m = (val < 0) ? 0 : SEG7[val];
  uint8_t on = COL_FG.argb, gh = COL_FG.argb;
  const int xl = 3, xr = 26, yt = 3, ym = 27, yb = 51;
  seg_h(fb, ox, oy, xl, xr, yt, m & 0x01, on, gh);           // a
  seg_v(fb, ox, oy, xr, yt, ym, m & 0x02, on, gh);           // b
  seg_v(fb, ox, oy, xr, ym, yb, m & 0x04, on, gh);           // c
  seg_h(fb, ox, oy, xl, xr, yb, m & 0x08, on, gh);           // d
  seg_v(fb, ox, oy, xl, ym, yb, m & 0x10, on, gh);           // e
  seg_v(fb, ox, oy, xl, yt, ym, m & 0x20, on, gh);           // f
  seg_h(fb, ox, oy, xl, xr, ym, m & 0x40, on, gh);           // g
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void draw_vf(GContext *ctx, const char *t, GRect r, GFont f,
                    GTextAlignment a) {
  graphics_draw_text(ctx, t, f, r, GTextOverflowModeTrailingEllipsis, a, NULL);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  const int L = 7, R = 192;   // inner content edges

  // ---- Frame + section panels (matches the reference's layered structure) ----
  // Black is filled all the way to the screen edge (no white corner specks),
  // and the white interior is rounded so the bold frame reads as rounded.
  graphics_context_set_fill_color(ctx, COL_FG);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, COL_BG);
  graphics_fill_rect(ctx, GRect(5, 5, 190, 218), 7, GCornersAll);

  // Each content section is its own 1px-bordered box, 1px inside the outer
  // border, so a 1px gap shows between each box and the bold frame. The boxes
  // are square; the rounded white interior clips their frame-side corners into
  // a rounded look (the border emerges from the rounded corner, as in the mock).
  graphics_context_set_stroke_color(ctx, COL_FG);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_rect(ctx, GRect(6, 6,   188, 81));   // top     box y6..86
  graphics_draw_rect(ctx, GRect(6, 92,  188, 19));   // battery box y92..110
  graphics_draw_rect(ctx, GRect(6, 116, 188, 70));   // clock   box y116..185
  graphics_draw_rect(ctx, GRect(6, 185, 188, 37));   // bottom  box y185..221

  // The "large black border" rails around the battery run full width (edge to
  // edge, merging with the outer frame); a 1px gap separates them from each box.
  graphics_context_set_fill_color(ctx, COL_FG);
  graphics_fill_rect(ctx, GRect(0, 88,  200, 3), 0, GCornerNone);   // top <-> battery
  graphics_fill_rect(ctx, GRect(0, 112, 200, 3), 0, GCornerNone);   // battery <-> clock

  graphics_context_set_text_color(ctx, COL_FG);

  // ===================================================================
  // TOP SECTION  (y 8..84):  STEPS / value  -- rule --  DISTANCE / value
  // ===================================================================
  // Two stacked stats: STEPS (top) and WATER (bottom)
  draw_vf(ctx, "STEPS", GRect(L + 4, 9, 120, 18), s_lbl_font, GTextAlignmentLeft);
  draw_vf(ctx, s_steps_buf, GRect(L + 4, 23, R - L - 8, 24), s_val_font, GTextAlignmentLeft);
  graphics_draw_line(ctx, GPoint(L + 3, 48), GPoint(R - 3, 48));
  draw_vf(ctx, "WATER", GRect(L + 4, 50, 120, 18), s_lbl_font, GTextAlignmentLeft);
  draw_vf(ctx, s_water_buf, GRect(L + 4, 64, R - L - 8, 24), s_val_font, GTextAlignmentLeft);

  // ===================================================================
  // BATTERY SECTION  (box y92..110)
  // ===================================================================
  draw_vf(ctx, "BATTERY", GRect(L + 4, 93, 90, 18), s_lbl_font, GTextAlignmentLeft);
  GRect gauge = GRect(82, 96, R - 82 - 2, 12);
  graphics_draw_rect(ctx, gauge);
  int fw = ((gauge.size.w - 4) * s_battery) / 100;
  if (fw < 0) fw = 0;
  graphics_context_set_fill_color(ctx, COL_FG);
  graphics_fill_rect(ctx, GRect(gauge.origin.x + 2, gauge.origin.y + 2, fw, gauge.size.h - 4), 0, GCornerNone);

  // ===================================================================
  // CLOCK SECTION  (box y116..185)  -- digits drawn into framebuffer below
  // ===================================================================
  // AM/PM indicator + bluetooth (normal graphics, right side)
  if (!s_h24) {
    draw_vf(ctx, s_pm ? "PM" : "AM", GRect(R - 28, 118, 26, 18), s_lbl_font, GTextAlignmentRight);
  }
  if (s_connected) {
    int bx = R - 12, by = 165;
    graphics_context_set_stroke_color(ctx, COL_FG);
    graphics_context_set_stroke_width(ctx, 2);
    graphics_draw_line(ctx, GPoint(bx, by - 8), GPoint(bx, by + 8));
    graphics_draw_line(ctx, GPoint(bx, by - 8), GPoint(bx + 5, by - 3));
    graphics_draw_line(ctx, GPoint(bx + 5, by - 3), GPoint(bx - 5, by + 3));
    graphics_draw_line(ctx, GPoint(bx - 5, by - 3), GPoint(bx + 5, by + 3));
    graphics_draw_line(ctx, GPoint(bx + 5, by + 3), GPoint(bx, by + 8));
  }

  // ===================================================================
  // BOTTOM SECTION  (box y185..221):  DAY / DATE / YEAR
  // ===================================================================
  // three columns: DAY (left)  DATE (center)  YEAR (right)
  draw_vf(ctx, "DAY",  GRect(L + 3, 188, 60, 18),  s_lbl_font, GTextAlignmentLeft);
  draw_vf(ctx, "DATE", GRect(40, 188, 120, 18),    s_lbl_font, GTextAlignmentCenter);
  draw_vf(ctx, "YEAR", GRect(R - 63, 188, 60, 18), s_lbl_font, GTextAlignmentRight);
  draw_vf(ctx, s_day_buf,  GRect(L + 3, 200, 60, 22),  s_val_font, GTextAlignmentLeft);
  draw_vf(ctx, s_date_buf, GRect(40, 200, 120, 22),    s_val_font, GTextAlignmentCenter);
  draw_vf(ctx, s_year_buf, GRect(R - 63, 200, 60, 22), s_val_font, GTextAlignmentRight);

  // ---- framebuffer pass: 7-segment clock digits + colon ----
  int h = s_hour;
  int d0 = h / 10, d1 = h % 10, d2 = s_min / 10, d3 = s_min % 10;
  if (!s_h24 && d0 == 0) d0 = -1;   // blank leading zero in 12h
  const int CY = 123;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (fb) {
    draw_digit(fb, 11,  CY, d0);
    draw_digit(fb, 49,  CY, d1);
    draw_digit(fb, 97,  CY, d2);
    draw_digit(fb, 135, CY, d3);
    // colon: two solid blocks
    uint8_t on = COL_FG.argb;
    for (int yy = 141; yy <= 147; yy++) for (int xx = 85; xx <= 91; xx++) fb_px(fb, xx, yy, on);
    for (int yy = 158; yy <= 164; yy++) for (int xx = 85; xx <= 91; xx++) fb_px(fb, xx, yy, on);
    graphics_release_frame_buffer(ctx, fb);
  }
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
static void format_commas(int v, char *out, size_t n) {
  char tmp[12]; snprintf(tmp, sizeof(tmp), "%d", v);
  int len = strlen(tmp), commas = (len - 1) / 3, nl = len + commas;
  if (nl >= (int)n) { snprintf(out, n, "%d", v); return; }
  out[nl] = '\0'; int oi = nl - 1;
  for (int i = len - 1, c = 0; i >= 0; i--) {
    out[oi--] = tmp[i];
    if (++c % 3 == 0 && i != 0) out[oi--] = ',';
  }
}

static void update_health(void) {
#if defined(PBL_HEALTH)
  time_t s = time_start_of_today(), e = time(NULL);
  if (health_service_metric_accessible(HealthMetricStepCount, s, e) & HealthServiceAccessibilityMaskAvailable)
    format_commas((int)health_service_sum_today(HealthMetricStepCount), s_steps_buf, sizeof(s_steps_buf));
#endif
}

// Format the water buffer from the latest synced values.
static void update_water_buf(void) {
  if (s_water_today < 0) {
    snprintf(s_water_buf, sizeof(s_water_buf), "-- OZ");
  } else if (s_water_goal > 0) {
    snprintf(s_water_buf, sizeof(s_water_buf), "%d/%d OZ", s_water_today, s_water_goal);
  } else {
    snprintf(s_water_buf, sizeof(s_water_buf), "%d OZ", s_water_today);
  }
}

static void update_time(void) {
  time_t now = time(NULL); struct tm *t = localtime(&now);
  s_h24 = clock_is_24h_style();
  s_min = t->tm_min;
  s_pm  = t->tm_hour >= 12;
  if (s_h24) s_hour = t->tm_hour;
  else { s_hour = t->tm_hour % 12; if (s_hour == 0) s_hour = 12; }
  strftime(s_day_buf,  sizeof(s_day_buf),  "%a", t);
  strftime(s_date_buf, sizeof(s_date_buf), "%b %d", t);
  strftime(s_year_buf, sizeof(s_year_buf), "%Y", t);
  for (char *p = s_day_buf;  *p; ++p) *p = toupper((int)*p);
  for (char *p = s_date_buf; *p; ++p) *p = toupper((int)*p);
}

// ---- Water sync (AppMessage to the TapSip Android companion) --------------
static void request_water_sync(void) {
  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) == APP_MSG_OK) {
    dict_write_uint8(it, MESSAGE_KEY_RequestSync, 1);
    app_message_outbox_send();
  }
}
static void inbox_received_handler(DictionaryIterator *it, void *ctx) {
  bool changed = false;
  Tuple *today = dict_find(it, MESSAGE_KEY_TodayOz);
  if (today) {
    s_water_today = today->value->int32;
    persist_write_int(PKEY_WATER_TODAY, s_water_today);
    changed = true;
  }
  Tuple *goal = dict_find(it, MESSAGE_KEY_GoalOz);
  if (goal) {
    s_water_goal = goal->value->int32;
    persist_write_int(PKEY_WATER_GOAL, s_water_goal);
    changed = true;
  }
  if (changed) {
    update_water_buf();
    layer_mark_dirty(s_canvas_layer);
  }
}

static void tick_handler(struct tm *t, TimeUnits u) {
  update_time(); update_health(); layer_mark_dirty(s_canvas_layer);
  if (t->tm_min % 5 == 0) request_water_sync();   // periodic refresh
}
static void battery_handler(BatteryChargeState c) {
  s_battery = c.charge_percent; layer_mark_dirty(s_canvas_layer);
}
static void connection_handler(bool c) {
  s_connected = c;
  if (c) request_water_sync();                    // refresh on reconnect
  layer_mark_dirty(s_canvas_layer);
}
#if defined(PBL_HEALTH)
static void health_handler(HealthEventType e, void *ctx) {
  update_health(); layer_mark_dirty(s_canvas_layer);
}
#endif

// ---------------------------------------------------------------------------
static void window_load(Window *window) {
  s_lbl_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  s_val_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  s_big_font = fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD);
  Layer *root = window_get_root_layer(window);
  s_canvas_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas_layer, canvas_update_proc);
  layer_add_child(root, s_canvas_layer);
  update_time(); update_health();
  BatteryChargeState b = battery_state_service_peek();
  s_battery = b.charge_percent;
  s_connected = connection_service_peek_pebble_app_connection();
}
static void window_unload(Window *window) {
  layer_destroy(s_canvas_layer);
}

static void init(void) {
  s_window = window_create();
  window_set_background_color(s_window, COL_FG);
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  connection_service_subscribe((ConnectionHandlers){ .pebble_app_connection_handler = connection_handler });
#if defined(PBL_HEALTH)
  health_service_events_subscribe(health_handler, NULL);
#endif

  // restore last-known water values, then open AppMessage and ask the phone
  if (persist_exists(PKEY_WATER_TODAY)) s_water_today = persist_read_int(PKEY_WATER_TODAY);
  if (persist_exists(PKEY_WATER_GOAL))  s_water_goal  = persist_read_int(PKEY_WATER_GOAL);
  update_water_buf();
  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());
  request_water_sync();
}
static void deinit(void) { window_destroy(s_window); }

int main(void) { init(); app_event_loop(); deinit(); }
