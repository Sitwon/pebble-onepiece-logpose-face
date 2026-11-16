#include <pebble.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// New World Log Pose — three glass orbs on a full-screen wooden dome.
//   top orb    : free needle. Shaking the wrist makes all three needles spin
//                for ~1s, wobble, then settle; the top needle settles pointing
//                north (compass platforms) or in a random direction.
//   bottom-left  : hour needle
//   bottom-right : minute needle
// ---------------------------------------------------------------------------

#define ORB_TOP    0
#define ORB_LEFT   1
#define ORB_RIGHT  2
#define ORB_COUNT  3

// Needle animation state machine: spin -> ease into target -> wobble -> track.
#define ANIM_FRAME_MS        33
#define SPIN_FRAMES          30   // ~1 s of full-speed whirling
#define SPIN_MIN_VEL_DEG     28   // per-frame spin speed range
#define SPIN_MAX_VEL_DEG     42
#define DECEL_FRAMES         12   // ease-out into the settled target
#define WOBBLE_FRAMES        36   // decaying sine around the target
#define WOBBLE_INTERVAL_MS   50
#define WOBBLE_AMPLITUDE_DEG 26
#define NEEDLE_REST_DEG      225  // rest heading before the first shake

#define TRACK_MS             10000 // compass keeps tracking 10 s after settle

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef ABS
#define ABS(a) ((a) < 0 ? -(a) : (a))
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

typedef enum {
  ANIM_IDLE,    // needles settled; minute ticks may re-wobble
  ANIM_SPIN,    // all needles whirling
  ANIM_DECEL,   // easing into each needle's target
  ANIM_WOBBLE,  // decaying wobble around the target
  ANIM_TRACK,   // top needle keeps tracking north for TRACK_MS, then freezes
} AnimState;

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

// Animation state
static AnimState s_anim_state = ANIM_IDLE;
static AppTimer *s_anim_timer;
static int s_anim_frame;                    // frame counter for SPIN/DECEL
static int s_wobble_frame;                  // frame counter for ANIM_WOBBLE
static uint8_t s_wobble_mask;               // bitmask: which needles wobble
static int32_t s_spin_deg[ORB_COUNT];       // free-spinning angles (trigangle)
static int32_t s_spin_vel_trig[ORB_COUNT];  // per-needle spin speed & direction
static int32_t s_decel_start[ORB_COUNT];    // angle when decel began
static int32_t s_decel_target[ORB_COUNT];   // settled target
static int32_t s_top_rest_trig;             // top needle's frozen rest angle

// Compass (only meaningful on PBL_COMPASS platforms)
static bool s_compass_active;
static bool s_heading_valid;
static int32_t s_heading_trig;  // on-screen rotation for "pointing north"

// Forward declarations (the animation loop and sensor handlers interleave)
static void prv_anim_tick(void *data);
static void prv_capture_heading(void *data);
static void prv_start_wobble(uint8_t mask);
static void prv_start_spin(void);
static void prv_tap_handler(AccelAxisType axis, int32_t direction);
static void prv_compass_handler(CompassHeadingData heading);

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
}

// Specular glass details drawn on top of the needles, so the reflections
// appear in front of what's inside the orb.
static void prv_draw_orb_glass_front(GContext *ctx, GPoint c, int r) {
  const GRect circle_rect = GRect(c.x - r, c.y - r, 2 * r, 2 * r);

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

// Settled target of a needle (trigangle, 0 = up, clockwise).
static int32_t prv_target_trig(int orb) {
  if (orb == ORB_TOP) {
    if (s_compass_active && s_heading_valid) {
      return s_heading_trig;
    }
    return s_top_rest_trig;
  }
  if (orb == ORB_LEFT) {
    return DEG_TO_TRIGANGLE(s_hour_angle_deg);
  }
  return DEG_TO_TRIGANGLE(s_min_angle_deg);
}

// Shortest signed trigangle difference from a to b.
static int32_t prv_ang_delta(int32_t from, int32_t to) {
  int32_t d = (to - from) % TRIG_MAX_ANGLE;
  if (d > TRIG_MAX_ANGLE / 2) {
    d -= TRIG_MAX_ANGLE;
  } else if (d < -TRIG_MAX_ANGLE / 2) {
    d += TRIG_MAX_ANGLE;
  }
  return d;
}

static void prv_fg_update_proc(Layer *layer, GContext *ctx) {
  if (!s_time_ready) {
    return;
  }

  for (int orb = 0; orb < ORB_COUNT; orb++) {
    int32_t angle;
    int32_t wob = 0;
    if (s_anim_state == ANIM_WOBBLE && (s_wobble_mask & (1 << orb))) {
      const int32_t amp = (int32_t)DEG_TO_TRIGANGLE(WOBBLE_AMPLITUDE_DEG) *
                          (WOBBLE_FRAMES - s_wobble_frame) / WOBBLE_FRAMES;
      wob = amp * sin_lookup(s_wobble_frame * 3300) / TRIG_MAX_RATIO;
    }
    switch (s_anim_state) {
      case ANIM_SPIN:
        angle = s_spin_deg[orb];
        break;
      case ANIM_DECEL: {
        // Ease-out from the spin angle into the target along the short arc.
        const int p = s_anim_frame * 100 / DECEL_FRAMES;   // 0..100
        const int e = p * (200 - p) / 100;                 // ease-out quad
        angle = s_decel_start[orb] +
                prv_ang_delta(s_decel_start[orb], s_decel_target[orb]) * e / 100;
        break;
      }
      case ANIM_TRACK:
        // Only the top needle tracks; hour/min stay settled.
        if (orb == ORB_TOP && s_heading_valid) {
          angle = s_heading_trig;
        } else {
          angle = prv_target_trig(orb);
        }
        break;
      default:  // ANIM_IDLE / ANIM_WOBBLE
        angle = prv_target_trig(orb) + wob;
        break;
    }
    prv_draw_needle(ctx, s_orb_center[orb], angle);
  }

  // Glass reflections render in front of the needles.
  for (int orb = 0; orb < ORB_COUNT; orb++) {
    prv_draw_orb_glass_front(ctx, s_orb_center[orb], s_orb_radius);
  }
}

// ---------------------------------------------------------------------------
// Time
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

static void prv_tick_handler(struct tm *tick_time, TimeUnits units) {
  prv_update_time();
  // Minute boundary: brief jiggle around the settled positions.
  prv_start_wobble(0x07);
}

// ---------------------------------------------------------------------------
// Needle animation: spin -> decel -> wobble -> (track north) -> idle
// ---------------------------------------------------------------------------

static void prv_anim_tick(void *data) {
  s_anim_timer = NULL;
  int next_ms = ANIM_FRAME_MS;

  switch (s_anim_state) {
    case ANIM_SPIN:
      for (int orb = 0; orb < ORB_COUNT; orb++) {
        s_spin_deg[orb] += s_spin_vel_trig[orb];
      }
      s_anim_frame++;
      if (s_anim_frame >= SPIN_FRAMES) {
        for (int orb = 0; orb < ORB_COUNT; orb++) {
          s_decel_start[orb] = s_spin_deg[orb];
          s_decel_target[orb] = prv_target_trig(orb);
        }
        s_anim_state = ANIM_DECEL;
        s_anim_frame = 0;
      }
      break;

    case ANIM_DECEL:
      s_anim_frame++;
      if (s_anim_frame >= DECEL_FRAMES) {
        s_anim_state = ANIM_WOBBLE;
        s_wobble_frame = 0;
        next_ms = WOBBLE_INTERVAL_MS;
      }
      break;

    case ANIM_WOBBLE:
      s_wobble_frame++;
      if (s_wobble_frame > WOBBLE_FRAMES) {
        if (s_compass_active) {
          // Keep tracking north for TRACK_MS, then freeze the heading.
          s_anim_state = ANIM_TRACK;
          s_anim_timer = app_timer_register(TRACK_MS, prv_capture_heading, NULL);
          return;
        }
        s_top_rest_trig = prv_target_trig(ORB_TOP);  // lock in the random rest
        s_anim_state = ANIM_IDLE;
        layer_mark_dirty(s_fg_layer);
        return;
      }
      break;

    default:
      return;
  }

  layer_mark_dirty(s_fg_layer);
  s_anim_timer = app_timer_register(next_ms, prv_anim_tick, NULL);
}

static void prv_capture_heading(void *data) {
  s_anim_timer = NULL;
  if (s_heading_valid) {
    s_top_rest_trig = s_heading_trig;
  }
  compass_service_unsubscribe();
  s_compass_active = false;
  s_heading_valid = false;
  s_anim_state = ANIM_IDLE;

  // Brief wobble on the top needle as it "locks in" its final position.
  prv_start_wobble(0x01);  // only ORB_TOP
  layer_mark_dirty(s_fg_layer);
}

// Minute-tick wobble: needles jiggle briefly around their settled positions.
static void prv_start_wobble(uint8_t mask) {
  if (s_anim_state != ANIM_IDLE) {
    return;
  }
  s_wobble_mask = mask;
  s_anim_state = ANIM_WOBBLE;
  s_wobble_frame = 0;
  s_anim_timer = app_timer_register(WOBBLE_INTERVAL_MS, prv_anim_tick, NULL);
}

// Full shake sequence: whirl ~1 s, wobble, then (on compass platforms) track.
static void prv_start_spin(void) {
  if (s_anim_state != ANIM_IDLE) {
    return;
  }

#ifndef PBL_COMPASS
  // No compass: every shake settles the top needle in a random direction.
  s_top_rest_trig = (int32_t)((uint32_t)rand() % TRIG_MAX_ANGLE);
#else
  // Fresh heading for the settle: subscribe the compass for the animation.
  s_heading_valid = false;
  s_compass_active = true;
  compass_service_subscribe(prv_compass_handler);
#endif

  s_spin_deg[ORB_TOP] = s_top_rest_trig;
  s_spin_deg[ORB_LEFT] = DEG_TO_TRIGANGLE(s_hour_angle_deg);
  s_spin_deg[ORB_RIGHT] = DEG_TO_TRIGANGLE(s_min_angle_deg);

  // Each needle whirls at a random speed in a random direction.
  for (int orb = 0; orb < ORB_COUNT; orb++) {
    const int vel_deg = SPIN_MIN_VEL_DEG +
                        rand() % (SPIN_MAX_VEL_DEG - SPIN_MIN_VEL_DEG + 1);
    const int dir = (rand() % 2) ? 1 : -1;
    s_spin_vel_trig[orb] = dir * vel_deg * TRIG_MAX_ANGLE / 360;
  }

  s_anim_state = ANIM_SPIN;
  s_anim_frame = 0;
  s_anim_timer = app_timer_register(ANIM_FRAME_MS, prv_anim_tick, NULL);
}

// ---------------------------------------------------------------------------
// Tap-to-spin: any tap while idle triggers the spin animation.
// ---------------------------------------------------------------------------

static void prv_tap_handler(AccelAxisType axis, int32_t direction) {
  if (s_anim_state != ANIM_IDLE) {
    return;
  }
  prv_start_spin();
}

static void prv_compass_handler(CompassHeadingData heading) {
  if (heading.compass_status == CompassStatusDataInvalid ||
      heading.compass_status == CompassStatusUnavailable) {
    return;
  }
  s_heading_trig = heading.magnetic_heading;  // needle rotation for north
  s_heading_valid = true;
  if (s_anim_state == ANIM_TRACK) {
    layer_mark_dirty(s_fg_layer);
  }
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
  s_top_rest_trig = DEG_TO_TRIGANGLE(NEEDLE_REST_DEG);
  s_heading_valid = false;
  srand((unsigned int)time(NULL));

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers) {
    .load = prv_window_load,
    .unload = prv_window_unload,
  });
  window_stack_push(s_window, true);

  prv_update_time();
  prv_start_wobble(0x07);  // settle animation on launch

  // Yellow backlight tint (emery/gabbro RGB backlight; no-op on monochrome).
  light_set_color_rgb888(0xFFF0A0);

  tick_timer_service_subscribe(MINUTE_UNIT, prv_tick_handler);
  accel_tap_service_subscribe(prv_tap_handler);

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Log Pose watchface initialized");
}

static void prv_deinit(void) {
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
  }
  if (s_compass_active) {
    compass_service_unsubscribe();
  }
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  light_set_system_color();  // restore white backlight
  window_destroy(s_window);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
