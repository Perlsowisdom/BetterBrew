#include <pebble.h>

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

typedef enum { KIND_POUR, KIND_PRESS, KIND_LOOSE, KIND_BAG, KIND_MATCHA } Kind;

typedef struct {
  const char *title;
  Kind kind;
  uint16_t bloom;   // seconds, 0 = no bloom stage
  uint16_t brew;    // seconds
  uint8_t temp_f;
  uint8_t temp_c;
  uint8_t size;     // brew size in fl oz of water
} Profile;

#define NUM_PROFILES 11

// Defaults. Index layout: 0-1 coffee, 2-9 tea (loose, bag pairs), 10 matcha.
static const Profile DEFAULTS[NUM_PROFILES] = {
  { "Pour-over",       KIND_POUR,   30, 150, 200,  93, 12 },
  { "French press",    KIND_PRESS,  30, 240, 200,  93, 16 },
  { "Black - Loose",   KIND_LOOSE,   0, 240, 212, 100,  8 },
  { "Black - Bag",     KIND_BAG,     0, 180, 212, 100,  8 },
  { "Green - Loose",   KIND_LOOSE,   0, 150, 175,  80,  8 },
  { "Green - Bag",     KIND_BAG,     0, 120, 175,  80,  8 },
  { "Oolong - Loose",  KIND_LOOSE,   0, 240, 195,  90,  8 },
  { "Oolong - Bag",    KIND_BAG,     0, 180, 195,  90,  8 },
  { "Herbal - Loose",  KIND_LOOSE,   0, 360, 212, 100,  8 },
  { "Herbal - Bag",    KIND_BAG,     0, 300, 212, 100,  8 },
  { "Matcha",          KIND_MATCHA,  0,  30, 175,  80,  4 },
};

static const char *TEA_NAMES[] = { "Black tea", "Green tea", "Oolong", "Herbal tea", "Matcha" };
static const uint8_t TEA_BASE[] = { 2, 4, 6, 8, 10 };
#define NUM_TEAS 5
static const char *COFFEE_NAMES[] = { "Pour-over", "French press" };

// User-editable values, persisted per profile.
typedef struct {
  uint16_t bloom;
  uint16_t brew;
  uint8_t size;
} Cfg;

static Cfg s_cfg[NUM_PROFILES];
#define PERSIST_BASE 1

#define BLOOM_MIN 10
#define BLOOM_MAX 300
#define BREW_MIN 10
#define BREW_MAX 1200
#define OZ_MIN 4    // half a cup
#define OZ_MAX 64   // 8 cups
#define OZ_PER_CUP 8

static bool has_bloom(int i) { return DEFAULTS[i].kind <= KIND_PRESS; }

static void cfg_load(void) {
  for (int i = 0; i < NUM_PROFILES; i++) {
    s_cfg[i] = (Cfg){ DEFAULTS[i].bloom, DEFAULTS[i].brew, DEFAULTS[i].size };
    if (persist_exists(PERSIST_BASE + i)) {
      Cfg tmp;
      if (persist_read_data(PERSIST_BASE + i, &tmp, sizeof(tmp)) == (int)sizeof(tmp)) {
        s_cfg[i] = tmp;
      }
    }
  }
}

static void cfg_save(int i) {
  persist_write_data(PERSIST_BASE + i, &s_cfg[i], sizeof(Cfg));
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void fmt_time(char *buf, size_t n, int secs) {
  snprintf(buf, n, "%d:%02d", secs / 60, secs % 60);
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

static void draw_text(GContext *ctx, const char *s, const char *font_key, GRect r,
                      GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, s, fonts_get_system_font(font_key), r,
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}

static GColor header_color(Kind k) {
  switch (k) {
    case KIND_POUR:
    case KIND_PRESS: return PBL_IF_COLOR_ELSE(GColorWindsorTan, GColorBlack);
    case KIND_MATCHA: return PBL_IF_COLOR_ELSE(GColorDarkGreen, GColorBlack);
    default: return PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack);
  }
}

// ---------------------------------------------------------------------------
// Timer window
// ---------------------------------------------------------------------------

typedef enum { PH_BLOOM, PH_BREW, PH_DONE } Phase;

static Window *s_timer_win;
static Layer *s_timer_layer;
static int s_run_idx;
static Phase s_phase;
static int s_remaining;
static bool s_paused;

static void timer_enter_phase(Phase ph) {
  s_phase = ph;
  if (ph == PH_BLOOM) {
    s_remaining = s_cfg[s_run_idx].bloom;
  } else if (ph == PH_BREW) {
    s_remaining = s_cfg[s_run_idx].brew;
  } else {
    s_remaining = 0;
  }
}

static void timer_advance(void) {
  light_enable_interaction();
  if (s_phase == PH_BLOOM) {
    timer_enter_phase(PH_BREW);
    vibes_long_pulse();
  } else {
    timer_enter_phase(PH_DONE);
    static const uint32_t segs[] = { 700, 250, 700, 250, 700 };
    VibePattern pat = { .durations = segs, .num_segments = ARRAY_LENGTH(segs) };
    vibes_enqueue_custom_pattern(pat);
  }
}

static void timer_tick(struct tm *t, TimeUnits units) {
  if (s_paused || s_phase == PH_DONE) return;
  s_remaining--;
  if (s_remaining <= 0) timer_advance();
  layer_mark_dirty(s_timer_layer);
}

static void timer_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w;
  const Profile *p = &DEFAULTS[s_run_idx];

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  const char *label;
  GColor hc;
  if (s_phase == PH_DONE) {
    label = "DONE";
    hc = PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack);
  } else if (s_paused) {
    label = "PAUSED";
    hc = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  } else if (s_phase == PH_BLOOM) {
    label = "BLOOM";
    hc = PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack);
  } else {
    label = "BREW";
    hc = PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
  }
  graphics_context_set_fill_color(ctx, hc);
  graphics_fill_rect(ctx, GRect(0, 0, w, 30), 0, GCornerNone);
  draw_text(ctx, label, FONT_KEY_GOTHIC_24_BOLD, GRect(0, 0, w, 30), GTextAlignmentCenter,
            GColorWhite);

  draw_text(ctx, p->title, FONT_KEY_GOTHIC_18, GRect(0, 32, w, 22), GTextAlignmentCenter,
            GColorBlack);

  char buf[32];
  fmt_time(buf, sizeof(buf), s_remaining);
  draw_text(ctx, buf, FONT_KEY_BITHAM_42_BOLD, GRect(0, 54, w, 50), GTextAlignmentCenter,
            GColorBlack);

  snprintf(buf, sizeof(buf), "%d\302\260F / %d\302\260C", p->temp_f, p->temp_c);
  draw_text(ctx, buf, FONT_KEY_GOTHIC_24_BOLD, GRect(0, 104, w, 30), GTextAlignmentCenter,
            GColorBlack);

  if (s_phase == PH_BLOOM) {
    char t[16];
    fmt_time(t, sizeof(t), s_cfg[s_run_idx].brew);
    snprintf(buf, sizeof(buf), "Then brew %s", t);
    draw_text(ctx, buf, FONT_KEY_GOTHIC_18, GRect(0, 132, w, 22), GTextAlignmentCenter,
              GColorBlack);
  }

  const char *hint = (s_phase == PH_DONE) ? "Select: close" : "Select: pause  Back: end";
  draw_text(ctx, hint, FONT_KEY_GOTHIC_14, GRect(0, 150, w, 18), GTextAlignmentCenter,
            GColorDarkGray);
}

static void timer_select(ClickRecognizerRef rec, void *ctx) {
  if (s_phase == PH_DONE) {
    window_stack_pop(true);
    return;
  }
  s_paused = !s_paused;
  layer_mark_dirty(s_timer_layer);
}

static void timer_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, timer_select);
}

static void timer_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_timer_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_timer_layer, timer_draw);
  layer_add_child(root, s_timer_layer);
  tick_timer_service_subscribe(SECOND_UNIT, timer_tick);
}

static void timer_unload(Window *w) {
  tick_timer_service_unsubscribe();
  vibes_cancel();
  layer_destroy(s_timer_layer);
}

static void timer_start(int idx) {
  s_run_idx = idx;
  s_paused = false;
  timer_enter_phase((has_bloom(idx) && s_cfg[idx].bloom > 0) ? PH_BLOOM : PH_BREW);
  window_stack_push(s_timer_win, true);
}

// ---------------------------------------------------------------------------
// Setup window (temperature, size, bloom/brew editing, amounts)
// ---------------------------------------------------------------------------

typedef enum { ROW_SIZE, ROW_BLOOM, ROW_BREW, ROW_START } RowType;

static Window *s_setup_win;
static Layer *s_setup_layer;
static int s_cur;
static int s_focus;
static bool s_editing;
static RowType s_rows[4];
static int s_nrows;

static void setup_build_rows(void) {
  s_nrows = 0;
  s_rows[s_nrows++] = ROW_SIZE;
  if (has_bloom(s_cur)) s_rows[s_nrows++] = ROW_BLOOM;
  s_rows[s_nrows++] = ROW_BREW;
  s_rows[s_nrows++] = ROW_START;
}

// Amount of beans / tea / matcha for the chosen size. Integer math only.
static void amount_lines(int idx, char *a, size_t an, char *b, size_t bn) {
  const Profile *p = &DEFAULTS[idx];
  int size = s_cfg[idx].size;
  switch (p->kind) {
    case KIND_POUR:
    case KIND_PRESS: {
      // 1:16 (pour-over) or 1:14 (French press) coffee to water by weight.
      int ratio = (p->kind == KIND_POUR) ? 16 : 14;
      int rate = (p->kind == KIND_POUR) ? 1848 : 2112;  // g per oz x1000
      int g10 = size * rate / 100;
      int oz100 = g10 * 200 / 567;
      snprintf(a, an, "Beans %d.%02d oz", oz100 / 100, oz100 % 100);
      snprintf(b, bn, "%d g, 1:%d, %d oz water", (g10 + 5) / 10, ratio, size);
      break;
    }
    case KIND_LOOSE: {
      int g10 = size * 313 / 100;   // ~2.5 g per 8 oz
      int tsp10 = g10 * 10 / 25;    // 2.5 g per tsp
      snprintf(a, an, "Tea %d.%d tsp", tsp10 / 10, tsp10 % 10);
      snprintf(b, bn, "%d.%d g, %d oz water", g10 / 10, g10 % 10, size);
      break;
    }
    case KIND_BAG: {
      int bags = clampi((size + OZ_PER_CUP - 1) / OZ_PER_CUP, 1, 9);  // 1 bag per cup, rounded up
      snprintf(a, an, "Tea bags: %d", bags);
      snprintf(b, bn, "1 per cup, %d oz water", size);
      break;
    }
    default: {
      int g10 = size * 250 / 100;   // 2 g per 8 oz
      int tsp10 = g10 * 10 / 20;    // 2 g per tsp
      snprintf(a, an, "Matcha %d.%d tsp", tsp10 / 10, tsp10 % 10);
      snprintf(b, bn, "%d.%d g, %d oz water", g10 / 10, g10 % 10, size);
      break;
    }
  }
}

static void setup_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w;
  const Profile *p = &DEFAULTS[s_cur];
  const Cfg *c = &s_cfg[s_cur];

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, header_color(p->kind));
  graphics_fill_rect(ctx, GRect(0, 0, w, 24), 0, GCornerNone);
  draw_text(ctx, p->title, FONT_KEY_GOTHIC_18_BOLD, GRect(0, -2, w, 24), GTextAlignmentCenter,
            GColorWhite);

  char buf[40];
  snprintf(buf, sizeof(buf), "%d\302\260F / %d\302\260C", p->temp_f, p->temp_c);
  draw_text(ctx, buf, FONT_KEY_GOTHIC_18_BOLD, GRect(0, 22, w, 22), GTextAlignmentCenter,
            GColorBlack);

  for (int i = 0; i < s_nrows; i++) {
    GRect r = GRect(4, 46 + i * 22, w - 8, 21);
    bool f = (i == s_focus);
    if (f) {
      GColor hl = s_editing ? PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack)
                            : PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
      graphics_context_set_fill_color(ctx, hl);
      graphics_fill_rect(ctx, r, 4, GCornersAll);
    }
    GColor fg = f ? GColorWhite : GColorBlack;
    bool ed = f && s_editing;   // show < value > while editing
    GRect lr = GRect(r.origin.x + 6, r.origin.y - 3, 70, 24);
    GRect vr = GRect(r.origin.x + 40, r.origin.y - 3, r.size.w - 46, 24);
    switch (s_rows[i]) {
      case ROW_SIZE:
        draw_text(ctx, "Cups", FONT_KEY_GOTHIC_18, lr, GTextAlignmentLeft, fg);
        snprintf(buf, sizeof(buf), (c->size % OZ_PER_CUP) ? "%s%d.5%s" : "%s%d%s",
                 ed ? "< " : "", c->size / OZ_PER_CUP, ed ? " >" : "");
        draw_text(ctx, buf, FONT_KEY_GOTHIC_18_BOLD, vr, GTextAlignmentRight, fg);
        break;
      case ROW_BLOOM:
        draw_text(ctx, "Bloom", FONT_KEY_GOTHIC_18, lr, GTextAlignmentLeft, fg);
        fmt_time(buf, sizeof(buf), c->bloom);
        if (ed) {
          char t[48];
          snprintf(t, sizeof(t), "< %s >", buf);
          draw_text(ctx, t, FONT_KEY_GOTHIC_18_BOLD, vr, GTextAlignmentRight, fg);
        } else {
          draw_text(ctx, buf, FONT_KEY_GOTHIC_18_BOLD, vr, GTextAlignmentRight, fg);
        }
        break;
      case ROW_BREW:
        draw_text(ctx, (p->kind == KIND_MATCHA) ? "Timer" : "Brew", FONT_KEY_GOTHIC_18, lr,
                  GTextAlignmentLeft, fg);
        fmt_time(buf, sizeof(buf), c->brew);
        if (ed) {
          char t[48];
          snprintf(t, sizeof(t), "< %s >", buf);
          draw_text(ctx, t, FONT_KEY_GOTHIC_18_BOLD, vr, GTextAlignmentRight, fg);
        } else {
          draw_text(ctx, buf, FONT_KEY_GOTHIC_18_BOLD, vr, GTextAlignmentRight, fg);
        }
        break;
      case ROW_START:
        draw_text(ctx, "Start", FONT_KEY_GOTHIC_18_BOLD,
                  GRect(r.origin.x, r.origin.y - 3, r.size.w, 24), GTextAlignmentCenter, fg);
        break;
    }
  }

  char a[24], d[32];
  amount_lines(s_cur, a, sizeof(a), d, sizeof(d));
  int y = 46 + s_nrows * 22 + 1;
  draw_text(ctx, a, FONT_KEY_GOTHIC_18_BOLD, GRect(0, y - 3, w, 22), GTextAlignmentCenter,
            GColorBlack);
  draw_text(ctx, d, FONT_KEY_GOTHIC_14, GRect(0, y + 16, w, 18), GTextAlignmentCenter,
            GColorDarkGray);
}

// Up/Down move between rows; while editing a row they change its value.
// n is the repeat count, so holding a button speeds things up.
static void setup_adjust(int dir, int n) {
  Cfg *c = &s_cfg[s_cur];
  if (!s_editing) {
    s_focus = clampi(s_focus - dir, 0, s_nrows - 1);   // Up (+1) moves focus up
    layer_mark_dirty(s_setup_layer);
    return;
  }
  int tstep = n < 10 ? 5 : (n < 25 ? 10 : 30);
  switch (s_rows[s_focus]) {
    case ROW_SIZE:
      c->size = clampi(c->size + dir * (n < 12 ? OZ_PER_CUP / 2 : OZ_PER_CUP), OZ_MIN, OZ_MAX);
      break;
    case ROW_BLOOM:
      c->bloom = clampi(c->bloom + dir * tstep, BLOOM_MIN, BLOOM_MAX);
      break;
    case ROW_BREW:
      c->brew = clampi(c->brew + dir * tstep, BREW_MIN, BREW_MAX);
      break;
    case ROW_START:
      break;
  }
  layer_mark_dirty(s_setup_layer);
}

static void setup_up(ClickRecognizerRef rec, void *ctx) {
  setup_adjust(+1, click_number_of_clicks_counted(rec));
}

static void setup_down(ClickRecognizerRef rec, void *ctx) {
  setup_adjust(-1, click_number_of_clicks_counted(rec));
}

static void setup_start(ClickRecognizerRef rec, void *ctx) {
  cfg_save(s_cur);
  timer_start(s_cur);
}

static void setup_select(ClickRecognizerRef rec, void *ctx) {
  if (s_rows[s_focus] == ROW_START) {
    setup_start(rec, ctx);
  } else {
    s_editing = !s_editing;
    layer_mark_dirty(s_setup_layer);
  }
}

static void setup_back(ClickRecognizerRef rec, void *ctx) {
  if (s_editing) {
    s_editing = false;
    layer_mark_dirty(s_setup_layer);
  } else {
    window_stack_pop(true);
  }
}

static void setup_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 120, setup_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 120, setup_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, setup_select);
  window_single_click_subscribe(BUTTON_ID_BACK, setup_back);
  window_long_click_subscribe(BUTTON_ID_SELECT, 600, setup_start, NULL);
}

static void setup_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_setup_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_setup_layer, setup_draw);
  layer_add_child(root, s_setup_layer);
  setup_build_rows();
  s_focus = s_nrows - 1;  // start on "Start" so one press begins brewing
  s_editing = false;
}

static void setup_unload(Window *w) {
  cfg_save(s_cur);
  layer_destroy(s_setup_layer);
}

static void setup_open(int idx) {
  s_cur = idx;
  window_stack_push(s_setup_win, true);
}

// ---------------------------------------------------------------------------
// Form window (loose leaf / teabag)
// ---------------------------------------------------------------------------

static Window *s_form_win;
static MenuLayer *s_form_menu;
static int s_form_base;

static uint16_t form_rows(MenuLayer *m, uint16_t s, void *d) { return 2; }

static void form_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *d) {
  int i = s_form_base + idx->row;
  char sub[32], t[16];
  fmt_time(t, sizeof(t), s_cfg[i].brew);
  snprintf(sub, sizeof(sub), "%s, %d\302\260F", t, DEFAULTS[i].temp_f);
  menu_cell_basic_draw(ctx, cell, idx->row == 0 ? "Loose leaf" : "Teabag", sub, NULL);
}

static void form_select(MenuLayer *m, MenuIndex *idx, void *d) {
  setup_open(s_form_base + idx->row);
}

static void form_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_form_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_form_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = form_rows,
    .draw_row = form_draw_row,
    .select_click = form_select,
  });
#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_form_menu, GColorCobaltBlue, GColorWhite);
#endif
  menu_layer_set_click_config_onto_window(s_form_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_form_menu));
}

static void form_unload(Window *w) {
  menu_layer_destroy(s_form_menu);
}

// ---------------------------------------------------------------------------
// Main menu
// ---------------------------------------------------------------------------

static Window *s_main_win;
static MenuLayer *s_main_menu;

static uint16_t main_sections(MenuLayer *m, void *d) { return 2; }

static uint16_t main_rows(MenuLayer *m, uint16_t s, void *d) {
  return s == 0 ? 2 : NUM_TEAS;
}

static int16_t main_header_h(MenuLayer *m, uint16_t s, void *d) {
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void main_draw_header(GContext *ctx, const Layer *cell, uint16_t s, void *d) {
  menu_cell_basic_header_draw(ctx, cell, s == 0 ? "Coffee" : "Tea");
}

static void main_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *d) {
  char sub[32];
  const char *title;
  int p;
  if (idx->section == 0) {
    title = COFFEE_NAMES[idx->row];
    p = idx->row;
  } else {
    title = TEA_NAMES[idx->row];
    p = TEA_BASE[idx->row];
  }
  snprintf(sub, sizeof(sub), "%d\302\260F / %d\302\260C", DEFAULTS[p].temp_f, DEFAULTS[p].temp_c);
  menu_cell_basic_draw(ctx, cell, title, sub, NULL);
}

static void main_select(MenuLayer *m, MenuIndex *idx, void *d) {
  if (idx->section == 0) {
    setup_open(idx->row);
  } else if (DEFAULTS[TEA_BASE[idx->row]].kind == KIND_MATCHA) {
    setup_open(TEA_BASE[idx->row]);
  } else {
    s_form_base = TEA_BASE[idx->row];
    window_stack_push(s_form_win, true);
  }
}

static void main_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_main_menu = menu_layer_create(layer_get_bounds(root));
  menu_layer_set_callbacks(s_main_menu, NULL, (MenuLayerCallbacks){
    .get_num_sections = main_sections,
    .get_num_rows = main_rows,
    .get_header_height = main_header_h,
    .draw_header = main_draw_header,
    .draw_row = main_draw_row,
    .select_click = main_select,
  });
#ifdef PBL_COLOR
  menu_layer_set_highlight_colors(s_main_menu, GColorCobaltBlue, GColorWhite);
#endif
  menu_layer_set_click_config_onto_window(s_main_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_main_menu));
}

static void main_unload(Window *w) {
  menu_layer_destroy(s_main_menu);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static void init(void) {
  cfg_load();

  s_main_win = window_create();
  window_set_window_handlers(s_main_win, (WindowHandlers){ .load = main_load, .unload = main_unload });

  s_form_win = window_create();
  window_set_window_handlers(s_form_win, (WindowHandlers){ .load = form_load, .unload = form_unload });

  s_setup_win = window_create();
  window_set_background_color(s_setup_win, GColorWhite);
  window_set_click_config_provider(s_setup_win, setup_click_config);
  window_set_window_handlers(s_setup_win, (WindowHandlers){ .load = setup_load, .unload = setup_unload });

  s_timer_win = window_create();
  window_set_background_color(s_timer_win, GColorWhite);
  window_set_click_config_provider(s_timer_win, timer_click_config);
  window_set_window_handlers(s_timer_win, (WindowHandlers){ .load = timer_load, .unload = timer_unload });

  window_stack_push(s_main_win, true);
}

static void deinit(void) {
  window_destroy(s_timer_win);
  window_destroy(s_setup_win);
  window_destroy(s_form_win);
  window_destroy(s_main_win);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
