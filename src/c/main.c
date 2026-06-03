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
static int  s_steps = 0;
static char s_water_buf[16] = "-- OZ";
static int  s_battery = 0;
static bool s_connected = true;
#define STEP_GOAL 10000

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

// Outlined progress bar with a solid fill (same style as the battery gauge).
static void draw_bar(GContext *ctx, GRect r, int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  graphics_context_set_stroke_color(ctx, COL_FG);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_rect(ctx, r);
  int fw = ((r.size.w - 4) * percent) / 100;
  graphics_context_set_fill_color(ctx, COL_FG);
  graphics_fill_rect(ctx, GRect(r.origin.x + 2, r.origin.y + 2, fw, r.size.h - 4),
                     0, GCornerNone);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static void canvas_update_proc(Layer *layer, GContext *ctx) {
  const int L = 7, R = 192;   // inner content edges

  // ---- Frame + section panels (matches the reference's layered structure) ----
  // Rounding is ONLY on the 4 outer screen corners (the black silhouette is a
  // rounded rect matching the display); the white interior and every section
  // inside it are square. Corner triangles are the black window background, so
  // there are no white corner specks.
  graphics_context_set_fill_color(ctx, COL_FG);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 12, GCornersAll);
  graphics_context_set_fill_color(ctx, COL_BG);
  graphics_fill_rect(ctx, GRect(5, 5, 190, 218), 0, GCornerNone);

  // Each content section is its own 1px-bordered box, 1px inside the outer
  // border, so a 1px gap shows between each box and the bold frame. The boxes
  // are square; the rounded white interior clips their frame-side corners into
  // a rounded look (the border emerges from the rounded corner, as in the mock).
  graphics_context_set_stroke_color(ctx, COL_FG);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_rect(ctx, GRect(6, 6,   188, 27));   // STEPS   box y6..32
  graphics_draw_rect(ctx, GRect(6, 38,  188, 27));   // WATER   box y38..64
  graphics_draw_rect(ctx, GRect(6, 70,  188, 27));   // BATTERY box y70..96
  graphics_draw_rect(ctx, GRect(6, 102, 188, 83));   // CLOCK   box y102..184
  graphics_draw_rect(ctx, GRect(6, 184, 188, 38));   // BOTTOM  box y184..221

  // Each stat panel is separated by a full-width "large black line" rail (edge
  // to edge, merging with the outer frame), with a 1px gap to each box border.
  graphics_context_set_fill_color(ctx, COL_FG);
  graphics_fill_rect(ctx, GRect(0, 34, 200, 3), 0, GCornerNone);   // STEPS <-> WATER
  graphics_fill_rect(ctx, GRect(0, 66, 200, 3), 0, GCornerNone);   // WATER <-> BATTERY
  graphics_fill_rect(ctx, GRect(0, 98, 200, 3), 0, GCornerNone);   // BATTERY <-> CLOCK

  graphics_context_set_text_color(ctx, COL_FG);

  // ===================================================================
  // TOP SECTION  (y 8..84):  STEPS / value  -- rule --  DISTANCE / value
  // ===================================================================
  // Three identical stats (STEPS, WATER, BATTERY): label + value on one line,
  // with a full-width progress bar below. STEPS + WATER share the top box.
  draw_vf(ctx, "STEPS", GRect(11, 10, 50, 18), s_lbl_font, GTextAlignmentLeft);
  draw_vf(ctx, s_steps_buf, GRect(52, 10, 56, 18), s_lbl_font, GTextAlignmentRight);
  draw_bar(ctx, GRect(116, 15, 72, 8), (s_steps * 100) / STEP_GOAL);

  draw_vf(ctx, "WATER", GRect(11, 42, 50, 18), s_lbl_font, GTextAlignmentLeft);
  draw_vf(ctx, s_water_buf, GRect(52, 42, 56, 18), s_lbl_font, GTextAlignmentRight);
  int water_pct = (s_water_today > 0 && s_water_goal > 0)
                    ? (s_water_today * 100) / s_water_goal : 0;
  draw_bar(ctx, GRect(116, 47, 72, 8), water_pct);

  // ===================================================================
  // BATTERY SECTION  (box y92..110)
  // ===================================================================
  char batt_buf[8];
  snprintf(batt_buf, sizeof(batt_buf), "%d%%", s_battery);
  draw_vf(ctx, "BATTERY", GRect(11, 74, 70, 18), s_lbl_font, GTextAlignmentLeft);
  draw_vf(ctx, batt_buf, GRect(58, 74, 50, 18), s_lbl_font, GTextAlignmentRight);
  draw_bar(ctx, GRect(116, 79, 72, 8), s_battery);

  // ===================================================================
  // CLOCK SECTION  (box y104..184)  -- digits drawn into framebuffer below
  // ===================================================================
  // AM/PM indicator + bluetooth (normal graphics, right side)
  if (!s_h24) {
    draw_vf(ctx, s_pm ? "PM" : "AM", GRect(R - 28, 108, 26, 18), s_lbl_font, GTextAlignmentRight);
  }
  if (s_connected) {
    int bx = R - 12, by = 160;
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
  draw_vf(ctx, "DAY",  GRect(L + 3, 186, 60, 18),  s_lbl_font, GTextAlignmentLeft);
  draw_vf(ctx, "DATE", GRect(40, 186, 120, 18),    s_lbl_font, GTextAlignmentCenter);
  draw_vf(ctx, "YEAR", GRect(R - 63, 186, 60, 18), s_lbl_font, GTextAlignmentRight);
  draw_vf(ctx, s_day_buf,  GRect(L + 3, 199, 60, 22),  s_val_font, GTextAlignmentLeft);
  draw_vf(ctx, s_date_buf, GRect(40, 199, 120, 22),    s_val_font, GTextAlignmentCenter);
  draw_vf(ctx, s_year_buf, GRect(R - 63, 199, 60, 22), s_val_font, GTextAlignmentRight);

  // ---- framebuffer pass: 7-segment clock digits + colon ----
  int h = s_hour;
  int d0 = h / 10, d1 = h % 10, d2 = s_min / 10, d3 = s_min % 10;
  if (!s_h24 && d0 == 0) d0 = -1;   // blank leading zero in 12h
  const int CY = 116;
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (fb) {
    draw_digit(fb, 11,  CY, d0);
    draw_digit(fb, 49,  CY, d1);
    draw_digit(fb, 97,  CY, d2);
    draw_digit(fb, 135, CY, d3);
    // colon: two solid blocks
    uint8_t on = COL_FG.argb;
    for (int yy = 134; yy <= 140; yy++) for (int xx = 85; xx <= 91; xx++) fb_px(fb, xx, yy, on);
    for (int yy = 151; yy <= 157; yy++) for (int xx = 85; xx <= 91; xx++) fb_px(fb, xx, yy, on);
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
  if (health_service_metric_accessible(HealthMetricStepCount, s, e) & HealthServiceAccessibilityMaskAvailable) {
    s_steps = (int)health_service_sum_today(HealthMetricStepCount);
    format_commas(s_steps, s_steps_buf, sizeof(s_steps_buf));
  }
#endif
}

// Format the water buffer from the latest synced values.
static void update_water_buf(void) {
  // Compact: today's oz (the goal is shown by the bar fill).
  if (s_water_today < 0)
    snprintf(s_water_buf, sizeof(s_water_buf), "-- OZ");
  else
    snprintf(s_water_buf, sizeof(s_water_buf), "%d OZ", s_water_today);
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
