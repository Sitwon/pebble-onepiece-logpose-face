#include <pebble.h>

// ---------------------------------------------------------------------------
// New World Log Pose — three glass orbs on a full-screen wooden dome.
//   top orb    : free needle, wobbles briefly each minute / on tap, then settles
//   bottom-left  : hour needle
//   bottom-right : minute needle
// ---------------------------------------------------------------------------

#define ORB_TOP    0
#define ORB_LEFT   1
#define ORB_RIGHT  2
#define ORB_COUNT  3

#define WOBBLE_FRAMES        36   // 36 * 50 ms = ~1.8 s, then the needle settles
#define WOBBLE_INTERVAL_MS   50
#define WOBBLE_AMPLITUDE_DEG 26
#define NEEDLE_REST_DEG      225  // free needle at rest points down-left

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Palette — hex values chosen to land on pleasing Pebble 64-color entries
#define COLOR_WOOD_BASE     GColorFromHEX(0xFFAA55)  // sandy orange-tan
#define COLOR_WOOD_LIGHT    GColorFromHEX(0xFFAA00)  // subtle plank variation
#define COLOR_WOOD_GRAIN    GColorFromHEX(0x555500)  // dark olive grain
#define COLOR_WOOD_SHADE    GColorFromHEX(0xAA5500)  // warm brown dome edge
#define COLOR_ORB_RIM       GColorFromHEX(0x1D5C39)
#define COLOR_ORB_GLASS     GColorFromHEX(0x55AAAA)  // sea green
#define COLOR_ORB_GLOW      GColorFromHEX(0x55AA55)  // deeper lower half
#define COLOR_ORB_SHEEN     GColorFromHEX(0xAAFFAA)  // pale mint sheen
#define COLOR_NEEDLE_RED    GColorFromHEX(0x8E1F3F)

static Window *s_window;
static Layer *s_bg_layer;
static Layer *s_fg_layer;

static GPath *s_needle_front;  // maroon tip
static GPath *s_needle_back;   // white tail
static GPoint s_front_pts[3];
static GPoint s_back_pts[3];

static GPoint s_orb_center[ORB_COUNT];
static int s_orb_radius;
static int s_needle_front_len;
static int s_needle_back_len;
static int s_needle_half_w;
static int s_pivot_radius;

static int s_hour_angle_deg;
static int s_min_angle_deg;
static bool s_time_ready;

static AppTimer *s_wobble_timer;
static int s_wobble_frame;  // -1 while settled

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void prv_layout(Layer *root) {
  GRect bounds = layer_get_bounds(root);
  int min_dim = MIN(bounds.size.w, bounds.size.h);
  GPoint c = grect_center_point(&bounds);

  s_orb_radius = min_dim * 21 / 100;          // 42 px on emery, 54 px on gabbro
  // Equilateral triangle of orb centers, centroid at the face center:
  // circumradius Rc, top vertex at (0, -Rc), bottom two at (+-Rc*sqrt(3)/2, +Rc/2).
#ifdef PBL_ROUND
  const int circum = min_dim * 28 / 100;      // round screen: fit inside the rim
#else
  const int circum = min_dim * 31 / 100;      // 62 px on emery
#endif

#ifndef PBL_ROUND
  // On the taller rectangular screen, the top orb reaches circum+r above the
  // centroid but only circum/2+r below it. Drop the centroid by circum/4 so
  // the top and bottom margins balance and the trio feels vertically centered.
  c.y += circum / 4;
#endif

  s_orb_center[ORB_TOP]   = GPoint(c.x, c.y - circum);
  s_orb_center[ORB_LEFT]  = GPoint(c.x - circum * 866 / 1000, c.y + circum / 2);
  s_orb_center[ORB_RIGHT] = GPoint(c.x + circum * 866 / 1000, c.y + circum / 2);

  s_needle_front_len = s_orb_radius * 95 / 100;  // tips nearly touch the glass
  s_needle_back_len = s_needle_front_len;        // equal-length white side
  s_needle_half_w = MAX(3, s_orb_radius * 219 / 1000);
  s_pivot_radius = s_needle_half_w + 2;
}

// ---------------------------------------------------------------------------
// Wood dome background
// ---------------------------------------------------------------------------

static void prv_draw_wood(GContext *ctx, GRect bounds) {
  const int w = bounds.size.w;
  const int h = bounds.size.h;

  // Base tone.
  graphics_context_set_fill_color(ctx, COLOR_WOOD_BASE);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Alternating lighter plank bands with dark gap lines between them.
  const int plank_w = MAX(20, w / 5);
  graphics_context_set_fill_color(ctx, COLOR_WOOD_LIGHT);
  for (int x = 0; x + plank_w <= w; x += 2 * plank_w) {
    graphics_fill_rect(ctx, GRect(x, 0, plank_w, h), 0, GCornerNone);
  }
  graphics_context_set_stroke_color(ctx, COLOR_WOOD_GRAIN);
  for (int x = plank_w; x < w; x += plank_w) {
    graphics_draw_line(ctx, GPoint(x, 0), GPoint(x, h));
  }

  // Wavy vertical grain lines.
  const int n_grain = MAX(6, w / 14);
  const int amp = 3;
  const int freq = TRIG_MAX_ANGLE * 2 / MAX(1, h);  // ~2 waves per screen
  for (int i = 0; i < n_grain; i++) {
    const int gx = (i + 1) * w / (n_grain + 1);
    const int phase = i * 8000;
    GPoint prev = GPoint(gx, 0);
    for (int y = 6; y <= h + 6; y += 8) {
      int32_t a = (y * freq + phase) % TRIG_MAX_ANGLE;
      GPoint cur = GPoint(gx + (amp * sin_lookup(a)) / TRIG_MAX_RATIO, y);
      graphics_draw_line(ctx, prev, cur);
      prev = cur;
    }
  }

  // Edge shading so the flat screen reads as a rounded dome.
#ifdef PBL_ROUND
  GPoint c = grect_center_point(&bounds);
  const int r = w / 2;
  graphics_context_set_stroke_color(ctx, COLOR_WOOD_SHADE);
  graphics_context_set_stroke_width(ctx, 10);
  graphics_draw_arc(ctx, GRect(c.x - r + 4, c.y - r + 4, 2 * (r - 4), 2 * (r - 4)),
                    GOvalScaleModeFitCircle, 0, TRIG_MAX_ANGLE);
#else
  graphics_context_set_fill_color(ctx, COLOR_WOOD_SHADE);
  graphics_fill_rect(ctx, GRect(0, 0, w, 6), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(0, h - 6, w, 6), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(0, 0, 6, h), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(w - 6, 0, 6, h), 0, GCornerNone);
#endif
}

// ---------------------------------------------------------------------------
// Glass orbs
// ---------------------------------------------------------------------------

static void prv_draw_orb(GContext *ctx, GPoint c, int r) {
  // Dark rim ring.
  graphics_context_set_fill_color(ctx, COLOR_ORB_RIM);
  graphics_fill_circle(ctx, c, r);

  // Glass body.
  graphics_context_set_fill_color(ctx, COLOR_ORB_GLASS);
  graphics_fill_circle(ctx, c, r - 2);

  const GRect circle_rect = GRect(c.x - r, c.y - r, 2 * r, 2 * r);

  // Deeper green in the lower half: glass depth.
  graphics_context_set_fill_color(ctx, COLOR_ORB_GLOW);
  graphics_fill_radial(ctx, circle_rect, GOvalScaleModeFitCircle, 2,
                       DEG_TO_TRIGANGLE(95), DEG_TO_TRIGANGLE(265));

  // Light sheen band in the upper left.
  graphics_context_set_fill_color(ctx, COLOR_ORB_SHEEN);
  graphics_fill_radial(ctx, circle_rect, GOvalScaleModeFitCircle, 2,
                       DEG_TO_TRIGANGLE(280), DEG_TO_TRIGANGLE(355));

  // White glass highlight spot, upper left.
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, GPoint(c.x - r * 4 / 10, c.y - r * 4 / 10),
                       MAX(2, r * 15 / 100));
}

static void prv_bg_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  prv_draw_wood(ctx, bounds);
  for (int i = 0; i < ORB_COUNT; i++) {
    prv_draw_orb(ctx, s_orb_center[i], s_orb_radius);
  }
}

// ---------------------------------------------------------------------------
// Needles
// ---------------------------------------------------------------------------

static void prv_draw_needle(GContext *ctx, GPoint c, int32_t angle) {
  // White tail blade.
  gpath_rotate_to(s_needle_back, angle);
  gpath_move_to(s_needle_back, c);
  graphics_context_set_fill_color(ctx, GColorWhite);
  gpath_draw_filled(ctx, s_needle_back);

  // Maroon tip blade.
  gpath_rotate_to(s_needle_front, angle);
  gpath_move_to(s_needle_front, c);
  graphics_context_set_fill_color(ctx, COLOR_NEEDLE_RED);
  gpath_draw_filled(ctx, s_needle_front);

  // White pivot ball with a dark rim.
  graphics_context_set_fill_color(ctx, COLOR_ORB_RIM);
  graphics_fill_circle(ctx, c, s_pivot_radius);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, c, s_pivot_radius - 1);
}

static void prv_fg_update_proc(Layer *layer, GContext *ctx) {
  if (!s_time_ready) {
    return;
  }

  int32_t rest_angle = DEG_TO_TRIGANGLE(NEEDLE_REST_DEG);
  if (s_wobble_frame >= 0) {
    const int32_t amp = (int32_t)DEG_TO_TRIGANGLE(WOBBLE_AMPLITUDE_DEG) *
                        (WOBBLE_FRAMES - s_wobble_frame) / WOBBLE_FRAMES;
    const int32_t wobble = amp * sin_lookup(s_wobble_frame * 3300) / TRIG_MAX_RATIO;
    rest_angle += wobble;
  }

  prv_draw_needle(ctx, s_orb_center[ORB_TOP], rest_angle);
  prv_draw_needle(ctx, s_orb_center[ORB_LEFT], DEG_TO_TRIGANGLE(s_hour_angle_deg));
  prv_draw_needle(ctx, s_orb_center[ORB_RIGHT], DEG_TO_TRIGANGLE(s_min_angle_deg));
}

// ---------------------------------------------------------------------------
// Time & animation
// ---------------------------------------------------------------------------

static void prv_update_time(void) {
  time_t t = time(NULL);
  struct tm *now = localtime(&t);
  s_hour_angle_deg = (now->tm_hour % 12) * 30 + now->tm_min / 2;
  s_min_angle_deg = now->tm_min * 6;
  s_time_ready = true;
  if (s_fg_layer) {
    layer_mark_dirty(s_fg_layer);
  }
}

static void prv_wobble_tick(void *data) {
  s_wobble_timer = NULL;
  s_wobble_frame++;
  if (s_wobble_frame < WOBBLE_FRAMES) {
    layer_mark_dirty(s_fg_layer);
    s_wobble_timer = app_timer_register(WOBBLE_INTERVAL_MS, prv_wobble_tick, NULL);
  } else {
    s_wobble_frame = -1;
    layer_mark_dirty(s_fg_layer);
  }
}

static void prv_start_wobble(void) {
  if (s_wobble_timer != NULL) {
    return;
  }
  s_wobble_frame = 0;
  s_wobble_timer = app_timer_register(WOBBLE_INTERVAL_MS, prv_wobble_tick, NULL);
}

static void prv_tick_handler(struct tm *tick_time, TimeUnits units) {
  prv_update_time();
  prv_start_wobble();
}

static void prv_tap_handler(AccelAxisType axis, int32_t direction) {
  prv_start_wobble();
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------

static void prv_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);

  prv_layout(window_layer);

  s_bg_layer = layer_create(layer_get_bounds(window_layer));
  layer_set_update_proc(s_bg_layer, prv_bg_update_proc);
  layer_add_child(window_layer, s_bg_layer);

  s_fg_layer = layer_create(layer_get_bounds(window_layer));
  layer_set_update_proc(s_fg_layer, prv_fg_update_proc);
  layer_add_child(window_layer, s_fg_layer);

  // Needle blades point "up" (0 deg) before rotation.
  s_front_pts[0] = GPoint(0, -s_needle_front_len);
  s_front_pts[1] = GPoint(s_needle_half_w, 0);
  s_front_pts[2] = GPoint(-s_needle_half_w, 0);
  s_back_pts[0] = GPoint(s_needle_half_w, 0);
  s_back_pts[1] = GPoint(0, s_needle_back_len);
  s_back_pts[2] = GPoint(-s_needle_half_w, 0);

  GPathInfo front_info = { .num_points = 3, .points = s_front_pts };
  GPathInfo back_info = { .num_points = 3, .points = s_back_pts };
  s_needle_front = gpath_create(&front_info);
  s_needle_back = gpath_create(&back_info);
}

static void prv_window_unload(Window *window) {
  gpath_destroy(s_needle_front);
  gpath_destroy(s_needle_back);
  layer_destroy(s_fg_layer);
  layer_destroy(s_bg_layer);
}

static void prv_init(void) {
  s_wobble_frame = -1;

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  prv_update_time();
  prv_start_wobble();  // settle animation on launch

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  accel_tap_service_subscribe(prv_tap_handler);

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Log Pose watchface initialized");
}

static void prv_deinit(void) {
  if (s_wobble_timer) {
    app_timer_cancel(s_wobble_timer);
  }
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
