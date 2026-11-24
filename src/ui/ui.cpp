// ScopeThing panel UI: shell (top bar, tabs), FEED, CHANNELS, MY NODES, STATUS, Find node, Alerts.
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "ui_int.h"
#include "core/textfix.h"

namespace ui {
using namespace st;

Palette T;
static const Palette DARK = {0x0A0E13, 0x0F151D, 0x17212E, 0x16202C, 0x1C2633, 0xE6EDF3, 0x9FB0C2, 0x6F7D8E, 0x39D0FF, 0xFFB547, 0xF472B6,
                             0x0D131A, 0x0C1218, 0x151E29, 0x0F2230, 0x2A3A4D, 0x263445, 0x2A3A4D, 0x1A0612, 0x16283A, 0x2A1024, 0x3A2A0C, 0x3A1430,
                             0x0D141C, 0x101924, 0x101822, 0x1E4A5E, 0x182230, 0x16222F, 0x8FE4FF, 0x111923, 0xC3CFDB, 0xFFFFFF, 0xFFFFFF, 0x06202B,
                             {0x2A1E08, 0x10251C, 0x2C1212, 0x1A1428}, {0xFFB547, 0x3DDC97, 0xFF8A7A, 0xD9C6FF}, true};
static const Palette DAY = {0xF3F5F8, 0xFFFFFF, 0xE6EDF5, 0xFFFFFF, 0xD5DCE5, 0x0F1720, 0x4A5868, 0x7A8796, 0x0A7FBF, 0xB86E00, 0xC2367F,
                            0xEAEEF3, 0xE9EDF2, 0xD9DFE7, 0xD4E9F7, 0xB9C5D3, 0xC9D2DD, 0xDCE3EC, 0xFFFFFF, 0xDCEFFA, 0xFBE3EF, 0xFCEBD0, 0xF8D3E5,
                            0xFFFFFF, 0xE3E9F0, 0xFFFFFF, 0x8CC8E8, 0xE1E6EC, 0xD9E1EA, 0x0A6A9E, 0xDDE3EA, 0x24303C, 0x000000, 0x1A2430, 0xFFFFFF,
                            {0xFCEBD0, 0xD6F2E4, 0xFADBD7, 0xE9E1FA}, {0x9A5B00, 0x14834F, 0xB3261E, 0x5B3FA8}, false};
bool dayMode() { return !T.dark; }
uint32_t gcol(Group g) {
  if (T.dark) return groupColor(g);
  static const uint32_t c[] = {0xD98A00, 0x0A8FD6, 0x0E9E8A, 0x7B4FE0, 0x18A060, 0x6B7785};
  return g < G_COUNT ? c[g] : 0x7B4FE0;
}
uint32_t rcol(Role r) {
  switch (r) {
    case ROLE_REPEATER: return T.dark ? 0xFFB547 : 0xE08A00;
    case ROLE_ROOM: return T.dark ? 0xB78CFF : 0x8A5CE6;
    case ROLE_SENSOR: return T.dark ? 0x3DDC97 : 0x18A060;
    default: return T.dark ? 0x39D0FF : 0x0A8FD6;
  }
}

Model *M = nullptr;
Platform *P = nullptr;
int curScreen = S_MAP;
static bool idle = false;  // untouched for a while: back on the map, dimmed (at night, by default)

static const lv_font_t *F12 = &lv_font_montserrat_12, *F14 = &lv_font_montserrat_14, *F16 = &lv_font_montserrat_16,
                       *F18 = &lv_font_montserrat_18, *F20 = &lv_font_montserrat_20, *F24 = &lv_font_montserrat_24,
                       *F28 = &lv_font_montserrat_28;

// ================================================================ helpers
lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *txt) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, txt);
  return l;
}
lv_obj_t *mkBox(lv_obj_t *parent, uint32_t bg, lv_opa_t opa) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}
lv_obj_t *mkBtn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud, uint32_t bg, uint32_t fg, const lv_font_t *f) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 9, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(T.btnBorder), 0);
  lv_obj_set_style_pad_hor(b, 12, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(T.pressed), LV_STATE_PRESSED);
  lv_obj_set_height(b, 40);
  lv_obj_t *l = mkLabel(b, f, fg, txt);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}
void oneLine(lv_obj_t *l) {
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}
void setBtnText(lv_obj_t *btn, const char *txt) { lv_label_set_text(lv_obj_get_child(btn, 0), txt); }
void setBtnColors(lv_obj_t *btn, uint32_t bg, uint32_t fg) {
  lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_hex(fg), 0);
}
static void hide(lv_obj_t *o, bool h) {
  if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static lv_obj_t *mkScroll(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = mkBox(parent, C_BG, LV_OPA_TRANSP);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(o, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
  return o;
}
static void upper(std::string &s) {
  for (auto &c : s) c = (char)toupper((unsigned char)c);
}

std::string ago(int64_t ts) {
  if (!ts) return "-";
  int64_t s = (P->nowMs() - ts) / 1000;
  if (s < 0) s = 0;
  char b[24];
  if (s < 60) snprintf(b, sizeof b, "%ds", (int)s);
  else if (s < 3600) snprintf(b, sizeof b, "%dm", (int)(s / 60));
  else if (s < 86400) snprintf(b, sizeof b, "%dh", (int)(s / 3600));
  else snprintf(b, sizeof b, "%dd", (int)(s / 86400));
  return b;
}
std::string fmtSnr(bool has, float v) {
  if (!has) return "-";
  char b[16];
  snprintf(b, sizeof b, "%s%.2g", v > 0 ? "+" : "", std::round(v * 100) / 100);
  if (std::fabs(v) >= 10) snprintf(b, sizeof b, "%s%.1f", v > 0 ? "+" : "", v);
  return b;
}
std::string feedDetail(const Route &r) {
  std::string s;
  if (!r.txt.empty()) s = r.txt + "  |  ";
  s += std::to_string(r.hops) + (r.hops == 1 ? " hop" : " hops");
  if (r.hasSnr) s += "  |  SNR " + fmtSnr(true, r.snr);
  if (!r.obs.empty()) s += "  |  via " + r.obs;
  return s;
}
bool passes(const Route &r) { return M->groupOn[r.group]; }

// ================================================================ shell
static lv_obj_t *scr, *topBar, *tabBar, *mainArea;
static lv_obj_t *screens[S_COUNT];
static lv_obj_t *tabs[S_COUNT];
static lv_obj_t *meshBadge, *chanBadge, *bellCount, *connChip, *connDot, *connText, *areaChip, *ppmLbl, *clockLbl, *battLbl;
// Radio only: FEED and CHATTER come from CoreScope, so they're hidden and the other tabs share the bar
static bool radioOnlyMode() { return M->cfg.radioOnly() && !M->cfg.demo; }
static void layoutTabs() {
  int vis[S_COUNT], n = 0;
  for (int i = 0; i < S_COUNT; i++) {
    bool show = !(radioOnlyMode() && (i == S_FEED || i == S_CHAN));
    hide(tabs[i], !show);
    if (show) vis[n++] = i;
  }
  for (int k = 0; k < n; k++) {
    int x0 = k * SCR_W / n, x1 = (k + 1) * SCR_W / n;
    lv_obj_set_pos(tabs[vis[k]], x0, 1);
    lv_obj_set_size(tabs[vis[k]], x1 - x0, TAB_H - 1);
  }
}
bool tabShown(int i) { return i >= 0 && i < S_COUNT && tabs[i] && !lv_obj_has_flag(tabs[i], LV_OBJ_FLAG_HIDDEN); }
static int unseenAlerts = 0;
static bool setupShown = false;
static std::string setupSsidText;
static bool feedDirty = true, chanListDirty = true, msgDirty = true, mineDirty = true, obsDirty = true, mapFeedDirty = true;

// ---------------------------------------------------------------- top bar
static void logoDraw(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords((lv_obj_t *)lv_event_get_current_target(e), &a);
  lv_point_precise_t p[3] = {{(float)a.x1 + 11, (float)a.y1 + 3}, {(float)a.x1 + 3, (float)a.y1 + 18}, {(float)a.x1 + 19, (float)a.y1 + 18}};
  lv_draw_line_dsc_t ld;
  lv_draw_line_dsc_init(&ld);
  ld.color = lv_color_hex(C_ACC);
  ld.width = 2;
  for (int i = 0; i < 3; i++) {
    ld.p1 = p[i];
    ld.p2 = p[(i + 1) % 3];
    lv_draw_line(layer, &ld);
  }
  const uint32_t cols[3] = {C_ACC, C_STAR, C_ACC};
  for (int i = 0; i < 3; i++) {
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(cols[i]);
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t r = {(int32_t)p[i].x - 3, (int32_t)p[i].y - 3, (int32_t)p[i].x + 3, (int32_t)p[i].y + 3};
    lv_draw_rect(layer, &rd, &r);
  }
}

static void buildTopBar() {
  topBar = mkBox(scr, C_BG);
  lv_obj_set_size(topBar, SCR_W, TOP_H);
  lv_obj_set_style_border_side(topBar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(topBar, 1, 0);
  lv_obj_set_style_border_color(topBar, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_hor(topBar, 12, 0);
  lv_obj_set_style_pad_column(topBar, 10, 0);
  lv_obj_set_flex_flow(topBar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(topBar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *logo = lv_obj_create(topBar);
  lv_obj_remove_style_all(logo);
  lv_obj_set_size(logo, 22, 22);
  lv_obj_add_event_cb(logo, logoDraw, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_t *brand = lv_obj_create(topBar);
  lv_obj_remove_style_all(brand);
  lv_obj_set_size(brand, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
  mkLabel(brand, F20, C_TEXT, "MESH");
  mkLabel(brand, F20, C_ACC, "top");

  lv_obj_t *bell = mkBtn(topBar, LV_SYMBOL_BELL, [](lv_event_t *) { openAlerts(); }, nullptr, C_BG, C_TEXT, F18);
  lv_obj_set_size(bell, 48, 32);
  lv_obj_set_style_border_width(bell, 0, 0);
  bellCount = mkBox(bell, C_AT);
  lv_obj_set_size(bellCount, LV_SIZE_CONTENT, 16);
  lv_obj_set_style_radius(bellCount, 8, 0);
  lv_obj_set_style_pad_hor(bellCount, 5, 0);
  lv_obj_align(bellCount, LV_ALIGN_TOP_RIGHT, 6, -2);
  lv_obj_add_flag(bellCount, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_t *bc = mkLabel(bellCount, F12, T.badgeText, "0");
  lv_obj_center(bc);
  hide(bellCount, true);

  connChip = mkBox(topBar, T.connBg[0]);
  lv_obj_set_size(connChip, LV_SIZE_CONTENT, 24);
  lv_obj_set_style_radius(connChip, 12, 0);
  lv_obj_set_style_pad_hor(connChip, 10, 0);
  lv_obj_set_style_pad_column(connChip, 6, 0);
  lv_obj_set_flex_flow(connChip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(connChip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  connDot = mkBox(connChip, C_STAR);
  lv_obj_set_size(connDot, 8, 8);
  lv_obj_set_style_radius(connDot, 4, 0);
  connText = mkLabel(connChip, F12, C_STAR, "STARTING");

  areaChip = mkBox(topBar, T.sel);
  lv_obj_set_size(areaChip, LV_SIZE_CONTENT, 24);
  lv_obj_set_style_radius(areaChip, 12, 0);
  lv_obj_set_style_pad_hor(areaChip, 10, 0);
  lv_obj_t *al = mkLabel(areaChip, F12, T.areaFg, "-");
  lv_obj_center(al);

  lv_obj_t *spacer = lv_obj_create(topBar);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_height(spacer, 1);
  lv_obj_set_flex_grow(spacer, 1);
  battLbl = mkLabel(topBar, F14, C_DIM, "");  // your radio's battery, while it's connected
  hide(battLbl, true);
  ppmLbl = mkLabel(topBar, F14, C_DIM, "0 pkt/min");
  clockLbl = mkLabel(topBar, F16, C_TEXT, "--:--");
}

// A single Li-ion cell's voltage as a rough charge level (resting voltage; reads high while charging).
static int battPercent(int mv) {
  static const int pts[][2] = {{4200, 100}, {4100, 90}, {4000, 78}, {3900, 65}, {3800, 50},
                               {3700, 35}, {3600, 20}, {3500, 10}, {3400, 5},  {3300, 0}};
  if (mv >= pts[0][0]) return 100;
  for (int i = 1; i < 10; i++)
    if (mv >= pts[i][0]) return pts[i][1] + (mv - pts[i][0]) * (pts[i - 1][1] - pts[i][1]) / (pts[i - 1][0] - pts[i][0]);
  return 0;
}

static void updateTopBar() {
  int64_t now = P->nowMs();
  ConnState cs = P->conn();
  static const char *names[] = {"CONNECTING", "LIVE", "OFFLINE", "DEMO"};
  const char *chip = names[cs];
  if (radioOnlyMode()) {  // CoreScope is off: the chip is about your radio
    Companion *r = P->radio();
    bool ready = r && r->state == R_READY;
    cs = ready ? CONN_LIVE : CONN_WAIT;
    chip = ready ? "RADIO" : "NO RADIO";
  }
  const uint32_t *bg = T.connBg, *fg = T.connFg;
  lv_obj_set_style_bg_color(connChip, lv_color_hex(bg[cs]), 0);
  lv_obj_set_style_bg_color(connDot, lv_color_hex(fg[cs]), 0);
  lv_obj_set_style_text_color(connText, lv_color_hex(fg[cs]), 0);
  lv_label_set_text(connText, chip);
  std::string area = M->repeaterName.empty() ? M->areaLabel : "Repeater " + M->repeaterName;
  if (area.size() > 30) area = area.substr(0, 28) + "...";
  lv_label_set_text(lv_obj_get_child(areaChip, 0), area.c_str());
  char b[48];
  snprintf(b, sizeof b, "%d pkt/min", M->pktPerMin(now));
  lv_label_set_text(ppmLbl, b);
  {
    Companion *r = P->radio();
    int mv = r && r->state == R_READY ? r->battMv : -1;
    if (mv > 2500) {  // below that it isn't a battery reading (no battery fitted)
      int pc = battPercent(mv);
      const char *ic = pc >= 80 ? LV_SYMBOL_BATTERY_FULL : pc >= 55 ? LV_SYMBOL_BATTERY_3 : pc >= 30 ? LV_SYMBOL_BATTERY_2
                       : pc >= 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
      snprintf(b, sizeof b, "%s %d%%", ic, pc);
      lv_label_set_text(battLbl, b);
      lv_obj_set_style_text_color(battLbl, lv_color_hex(pc < 20 ? 0xFF5A5A : C_DIM), 0);
      hide(battLbl, false);
    } else hide(battLbl, true);
  }
  if (P->timeValid()) {
    int h, m, s;
    P->localClock(now, h, m, s);
    time_t sec = (time_t)(now / 1000);
    struct tm g;
    gmtime_r(&sec, &g);
    snprintf(b, sizeof b, "%d:%02d %s   %02d:%02dZ", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM", g.tm_hour, g.tm_min);
  } else snprintf(b, sizeof b, "--:--");
  lv_label_set_text(clockLbl, b);
  if (unseenAlerts > 0) {
    lv_label_set_text_fmt(lv_obj_get_child(bellCount, 0), "%d", std::min(unseenAlerts, 99));
    hide(bellCount, false);
  } else hide(bellCount, true);
  int unread = 0;
  for (auto &c : M->channels)
    if (!M->isMuted(c.name)) unread += c.unread;  // muted channels don't call for attention
  if (unread > 0 && curScreen != S_CHAN) {
    lv_label_set_text_fmt(lv_obj_get_child(chanBadge, 0), "%d", std::min(unread, 99));
    hide(chanBadge, false);
  } else hide(chanBadge, true);
  int mu = radioUnread();
  if (mu > 0 && curScreen != S_RADIO) {
    lv_label_set_text_fmt(lv_obj_get_child(meshBadge, 0), "%d", std::min(mu, 99));
    hide(meshBadge, false);
  } else hide(meshBadge, true);
}

// ---------------------------------------------------------------- tabs
static void onTab(lv_event_t *e) { showScreen((int)(intptr_t)lv_event_get_user_data(e)); }
static void buildTabs() {
  tabBar = mkBox(scr, C_BG);
  lv_obj_set_pos(tabBar, 0, SCR_H - TAB_H);
  lv_obj_set_size(tabBar, SCR_W, TAB_H);
  lv_obj_set_style_border_side(tabBar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(tabBar, 1, 0);
  lv_obj_set_style_border_color(tabBar, lv_color_hex(C_LINE), 0);
  static const char *names[] = {"MAP", "FEED", "CHATTER", "MY NODES", "MESH", "STATUS"};
  for (int i = 0; i < S_COUNT; i++) {
    lv_obj_t *t = lv_button_create(tabBar);
    lv_obj_remove_style_all(t);
    int x0 = i * SCR_W / S_COUNT, x1 = (i + 1) * SCR_W / S_COUNT;
    lv_obj_set_pos(t, x0, 1);
    lv_obj_set_size(t, x1 - x0, TAB_H - 1);
    lv_obj_set_style_bg_color(t, lv_color_hex(T.tabPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(C_ACC), 0);
    lv_obj_t *l = mkLabel(t, F16, C_DIM, names[i]);
    lv_obj_center(l);
    lv_obj_add_event_cb(t, onTab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    tabs[i] = t;
  }
  chanBadge = mkBox(tabs[S_CHAN], C_AT);
  lv_obj_set_size(chanBadge, LV_SIZE_CONTENT, 18);
  lv_obj_set_style_radius(chanBadge, 9, 0);
  lv_obj_set_style_pad_hor(chanBadge, 6, 0);
  lv_obj_align(chanBadge, LV_ALIGN_TOP_RIGHT, -14, 6);
  lv_obj_t *bl = mkLabel(chanBadge, F12, T.badgeText, "0");
  lv_obj_center(bl);
  hide(chanBadge, true);
  lv_obj_align(chanBadge, LV_ALIGN_TOP_RIGHT, -4, 6);
  meshBadge = mkBox(tabs[S_RADIO], C_AT);
  lv_obj_set_size(meshBadge, LV_SIZE_CONTENT, 18);
  lv_obj_set_style_radius(meshBadge, 9, 0);
  lv_obj_set_style_pad_hor(meshBadge, 6, 0);
  lv_obj_align(meshBadge, LV_ALIGN_TOP_RIGHT, -10, 6);
  lv_obj_center(mkLabel(meshBadge, F12, T.badgeText, "0"));
  hide(meshBadge, true);
  layoutTabs();
}
static void styleTabs() {
  for (int i = 0; i < S_COUNT; i++) {
    bool on = i == curScreen;
    lv_obj_set_style_border_width(tabs[i], on ? 3 : 0, 0);
    lv_obj_set_style_bg_opa(tabs[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(tabs[i], lv_color_hex(T.tabActive), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(tabs[i], 0), lv_color_hex(on ? C_TEXT : C_DIM), 0);
  }
}

// ================================================================ FEED
static lv_obj_t *feedList, *feedCount, *filterNote, *feedEmpty;
static lv_obj_t *chips[G_COUNT + 1];
struct FRow {
  lv_obj_t *box, *tagBox, *tag, *who, *det, *ago;
};
static std::vector<FRow> feedRows;

static void onChip(lv_event_t *e) {
  int g = (int)(intptr_t)lv_event_get_user_data(e);
  int on = 0;
  for (int i = 0; i < G_COUNT; i++) on += M->groupOn[i];
  if (g == G_COUNT) {  // ALL
    bool all = on == G_COUNT;
    for (int i = 0; i < G_COUNT; i++) M->groupOn[i] = all ? i != G_CONTROL : true;
  } else {
    M->groupOn[g] = !M->groupOn[g];
    on = 0;
    for (int i = 0; i < G_COUNT; i++) on += M->groupOn[i];
    if (!on) M->groupOn[g == G_ADVERT ? G_CHANNEL : G_ADVERT] = true;
  }
  P->saveFilter();
  feedDirty = mapFeedDirty = true;
}
static void renderChips() {
  int on = 0;
  for (int i = 0; i < G_COUNT; i++) on += M->groupOn[i];
  for (int i = 0; i <= G_COUNT; i++) {
    bool act = i == G_COUNT ? on == G_COUNT : M->groupOn[i];
    uint32_t c = i == G_COUNT ? C_ACC : gcol((Group)i);
    lv_obj_set_style_bg_color(chips[i], lv_color_hex(act ? T.chipOn : C_PANEL), 0);
    lv_obj_set_style_border_color(chips[i], lv_color_hex(act ? c : C_LINE), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(chips[i], -1), lv_color_hex(act ? C_TEXT : C_DIM2), 0);
  }
  if (on == G_COUNT) lv_label_set_text(filterNote, "");
  else if (on == 1) {
    for (int i = 0; i < G_COUNT; i++)
      if (M->groupOn[i]) lv_label_set_text_fmt(filterNote, "%s only", groupName((Group)i));
  } else lv_label_set_text_fmt(filterNote, "filtered (%d of 6)", on);
}
static void buildFeed(lv_obj_t *s) {
  lv_obj_t *h = mkLabel(s, F18, C_TEXT, "LIVE FEED");
  lv_obj_set_pos(h, 14, 10);
  filterNote = mkLabel(s, F14, C_DIM, "");
  lv_obj_set_pos(filterNote, 130, 13);
  feedCount = mkLabel(s, F14, C_DIM, "0 packets");
  lv_obj_align(feedCount, LV_ALIGN_TOP_RIGHT, -14, 13);
  feedList = mkScroll(s, 0, 40, SCR_W, MAIN_H - 40 - 60);
  feedEmpty = mkLabel(feedList, F16, C_DIM, "No packets yet. If this stays empty, check the STATUS screen.");
  lv_obj_set_style_pad_all(feedEmpty, 20, 0);

  lv_obj_t *bar = mkBox(s, C_BG);
  lv_obj_set_pos(bar, 0, MAIN_H - 60);
  lv_obj_set_size(bar, SCR_W, 60);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_color(bar, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_hor(bar, 10, 0);
  lv_obj_set_style_pad_column(bar, 8, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  const int order[] = {G_COUNT, G_ADVERT, G_CHANNEL, G_DM, G_TRACE, G_PATH, G_CONTROL};
  for (int g : order) {
    lv_obj_t *c = mkBtn(bar, "", onChip, (void *)(intptr_t)g, C_PANEL, C_TEXT, F14);
    lv_obj_delete(lv_obj_get_child(c, 0));
    lv_obj_set_height(c, 42);
    lv_obj_set_style_radius(c, 21, 0);
    lv_obj_set_style_pad_hor(c, 12, 0);
    lv_obj_set_style_pad_column(c, 6, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (g != G_COUNT) {
      lv_obj_t *dot = mkBox(c, gcol((Group)g));
      lv_obj_set_size(dot, 9, 9);
      lv_obj_set_style_radius(dot, 2, 0);
    }
    mkLabel(c, F14, C_TEXT, g == G_COUNT ? "ALL" : groupName((Group)g));
    chips[g] = c;
  }
}
static FRow &feedRow(size_t i) {
  while (feedRows.size() <= i) {
    FRow r;
    r.box = mkBox(feedList, (feedRows.size() & 1) ? T.rowAlt : C_BG);
    lv_obj_set_size(r.box, SCR_W, 60);
    lv_obj_set_style_border_side(r.box, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(r.box, 0, 0);
    r.tagBox = mkBox(r.box, C_PANEL);
    lv_obj_set_pos(r.tagBox, 12, 16);
    lv_obj_set_size(r.tagBox, 96, 28);
    lv_obj_set_style_radius(r.tagBox, 6, 0);
    lv_obj_set_style_border_width(r.tagBox, 1, 0);
    r.tag = mkLabel(r.tagBox, F12, C_TEXT);
    lv_obj_center(r.tag);
    r.who = mkLabel(r.box, F18, C_TEXT);
    lv_obj_set_pos(r.who, 122, 8);
    lv_obj_set_width(r.who, 590);
    oneLine(r.who);
    r.det = mkLabel(r.box, F14, C_DIM);
    lv_obj_set_pos(r.det, 122, 33);
    lv_obj_set_width(r.det, 600);
    oneLine(r.det);
    r.ago = mkLabel(r.box, F14, C_DIM2);
    lv_obj_align(r.ago, LV_ALIGN_RIGHT_MID, -14, 0);
    feedRows.push_back(r);
  }
  return feedRows[i];
}
static void renderFeed() {
  renderChips();
  size_t n = 0;
  for (auto &r : M->feed) {
    if (!passes(r)) continue;
    FRow &fr = feedRow(n++);
    uint32_t c = gcol(r.group);
    lv_label_set_text(fr.tag, r.label.c_str());
    lv_obj_set_style_text_color(fr.tag, lv_color_hex(c), 0);
    lv_obj_set_style_border_color(fr.tagBox, lv_color_hex(c), 0);
    lv_obj_set_style_border_opa(fr.tagBox, 120, 0);
    lv_label_set_text(fr.who, r.who.c_str());
    lv_label_set_text(fr.det, feedDetail(r).c_str());
    lv_label_set_text(fr.ago, ago(r.ts).c_str());
    lv_obj_set_style_border_width(fr.box, r.mine ? 4 : 0, 0);
    lv_obj_set_style_border_color(fr.box, lv_color_hex(T.ring), 0);
    hide(fr.box, false);
  }
  for (size_t i = n; i < feedRows.size(); i++) hide(feedRows[i].box, true);
  hide(feedEmpty, n > 0);
  lv_label_set_text(feedEmpty, M->feed.empty() ? "No packets yet. If this stays empty, check the STATUS screen."
                                               : "Nothing matches the filter yet.");
  lv_label_set_text_fmt(feedCount, "%d packets", (int)n);
}

// ================================================================ CHANNELS
static lv_obj_t *chanList, *chanName, *chanSub, *msgList, *msgEmpty, *chanMuteBtn;
static std::string curChan;  // channel name as listed
struct CRow {
  lv_obj_t *box, *name, *last, *ago, *badge;
};
static std::vector<CRow> chanRows;
struct MRow {
  lv_obj_t *box, *who, *ago, *text, *meta;
};
static std::vector<MRow> msgRows;

static void selectChannel(const std::string &name) {
  curChan = name;
  for (auto &c : M->channels)
    if (Model::chKey(c.name) == Model::chKey(name)) {
      c.unread = 0;
      P->requestHistory(c.name, c.hash);
    }
  chanListDirty = msgDirty = true;
}
static void onChanRow(lv_event_t *e) {
  size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
  if (i < M->channels.size()) selectChannel(M->channels[i].name);
  touched();
}
static void buildChannels(lv_obj_t *s) {
  chanList = mkScroll(s, 0, 0, 270, MAIN_H);
  lv_obj_set_style_bg_color(chanList, lv_color_hex(T.listBg), 0);
  lv_obj_set_style_bg_opa(chanList, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(chanList, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(chanList, 1, 0);
  lv_obj_set_style_border_color(chanList, lv_color_hex(C_LINE), 0);
  lv_obj_t *pane = mkBox(s, C_BG);
  lv_obj_set_pos(pane, 270, 0);
  lv_obj_set_size(pane, SCR_W - 270, MAIN_H);
  chanName = mkLabel(pane, F20, C_TEXT, "Channels");
  lv_obj_set_pos(chanName, 16, 8);
  lv_obj_set_width(chanName, SCR_W - 270 - 16 - 150);
  oneLine(chanName);
  chanSub = mkLabel(pane, F12, C_DIM, "Pick a channel");
  lv_obj_set_pos(chanSub, 16, 34);
  lv_obj_set_width(chanSub, SCR_W - 270 - 16 - 150);
  oneLine(chanSub);
  // Mute: no alerts or sounds from this channel (keywords and @mentions included), here and on the radio
  chanMuteBtn = mkBtn(pane, LV_SYMBOL_BELL "  Mute", [](lv_event_t *) {
    if (curChan.empty()) return;
    M->setMuted(curChan, !M->isMuted(curChan));
    P->saveSettings();
    chanListDirty = msgDirty = true;
    radioDirty();
    touched();
  }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_size(chanMuteBtn, 120, 38);
  lv_obj_align(chanMuteBtn, LV_ALIGN_TOP_RIGHT, -12, 8);
  msgList = mkScroll(pane, 0, 56, SCR_W - 270, MAIN_H - 56);
  lv_obj_set_style_pad_all(msgList, 10, 0);
  lv_obj_set_style_pad_row(msgList, 8, 0);
  lv_obj_set_style_border_side(msgList, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(msgList, 1, 0);
  lv_obj_set_style_border_color(msgList, lv_color_hex(C_LINE), 0);
  msgEmpty = mkLabel(msgList, F16, C_DIM, "");
}
static CRow &chanRow(size_t i) {
  while (chanRows.size() <= i) {
    CRow r;
    r.box = mkBox(chanList, T.listBg);
    lv_obj_set_size(r.box, 270, 66);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r.box, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_side(r.box, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r.box, 1, 0);
    lv_obj_set_style_border_color(r.box, lv_color_hex(T.listLine), 0);
    lv_obj_add_event_cb(r.box, onChanRow, LV_EVENT_CLICKED, (void *)(intptr_t)chanRows.size());
    r.name = mkLabel(r.box, F16, C_TEXT);
    lv_obj_set_pos(r.name, 14, 10);
    lv_obj_set_width(r.name, 170);
    oneLine(r.name);
    r.last = mkLabel(r.box, F12, C_DIM);
    lv_obj_set_pos(r.last, 14, 36);
    lv_obj_set_width(r.last, 240);
    oneLine(r.last);
    r.ago = mkLabel(r.box, F12, C_DIM2);
    lv_obj_align(r.ago, LV_ALIGN_TOP_RIGHT, -12, 12);
    r.badge = mkBox(r.box, C_AT);
    lv_obj_set_size(r.badge, LV_SIZE_CONTENT, 18);
    lv_obj_set_style_radius(r.badge, 9, 0);
    lv_obj_set_style_pad_hor(r.badge, 6, 0);
    lv_obj_align(r.badge, LV_ALIGN_TOP_RIGHT, -46, 10);
    lv_obj_t *bl = mkLabel(r.badge, F12, T.badgeText, "0");
    lv_obj_center(bl);
    chanRows.push_back(r);
  }
  return chanRows[i];
}
static void renderChanList() {
  size_t i = 0;
  for (; i < M->channels.size(); i++) {
    auto &c = M->channels[i];
    CRow &r = chanRow(i);
    bool sel = Model::chKey(c.name) == Model::chKey(curChan);
    lv_obj_set_style_bg_color(r.box, lv_color_hex(sel ? T.sel : T.listBg), 0);
    lv_obj_set_style_border_side(r.box, sel ? (lv_border_side_t)(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM) : LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(r.box, lv_color_hex(sel ? C_ACC : T.listLine), 0);
    lv_obj_set_style_border_width(r.box, sel ? 3 : 1, 0);
    bool muted = M->isMuted(c.name);
    lv_label_set_text(r.name, muted ? (LV_SYMBOL_MUTE "  " + c.name).c_str() : c.name.c_str());
    lv_obj_set_style_text_color(r.name, lv_color_hex(muted ? C_DIM : C_TEXT), 0);
    lv_obj_set_style_bg_color(r.badge, lv_color_hex(muted ? C_DIM2 : C_AT), 0);
    std::string last = c.who.empty() ? c.last : c.who + ": " + c.last;
    lv_label_set_text(r.last, last.empty() ? "-" : st::displayText(last).c_str());
    lv_label_set_text(r.ago, ago(c.ts).c_str());
    if (c.unread > 0) {
      lv_label_set_text_fmt(lv_obj_get_child(r.badge, 0), "%d", std::min(c.unread, 99));
      hide(r.badge, false);
    } else hide(r.badge, true);
    hide(r.box, false);
  }
  for (; i < chanRows.size(); i++) hide(chanRows[i].box, true);
}
static MRow &msgRow(size_t i) {
  while (msgRows.size() <= i) {
    MRow r;
    r.box = mkBox(msgList, C_PANEL);
    lv_obj_set_size(r.box, SCR_W - 270 - 24, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(r.box, 10, 0);
    lv_obj_set_style_pad_all(r.box, 10, 0);
    lv_obj_set_style_pad_row(r.box, 3, 0);
    lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(r.box, 1, 0);
    lv_obj_set_style_border_color(r.box, lv_color_hex(C_LINE), 0);
    lv_obj_t *top = lv_obj_create(r.box);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    r.who = mkLabel(top, F14, C_ACC);
    r.ago = mkLabel(top, F12, C_DIM2);
    lv_obj_align(r.ago, LV_ALIGN_TOP_RIGHT, 0, 1);
    r.text = mkLabel(r.box, F16, C_TEXT);
    lv_obj_set_width(r.text, lv_pct(100));
    lv_label_set_long_mode(r.text, LV_LABEL_LONG_WRAP);
    r.meta = mkLabel(r.box, F12, C_DIM2);
    lv_obj_set_width(r.meta, lv_pct(100));
    oneLine(r.meta);
    msgRows.push_back(r);
  }
  return msgRows[i];
}
static void renderMessages(bool toEnd) {
  if (curChan.empty() && !M->channels.empty()) curChan = M->channels[0].name;
  size_t n = 0;
  if (!curChan.empty()) {
    lv_label_set_text(chanName, curChan.c_str());
    int count = 0;
    for (auto &c : M->channels)
      if (Model::chKey(c.name) == Model::chKey(curChan)) count = c.count;
    auto it = M->messages.find(Model::chKey(curChan));
    if (it != M->messages.end()) {
      auto &list = it->second;
      for (auto &m : list) {
        MRow &r = msgRow(n++);
        lv_label_set_text(r.who, m.who.c_str());
        lv_label_set_text(r.ago, ago(m.ts).c_str());
        lv_label_set_text(r.text, st::displayText(m.text).c_str());
        std::string meta;
        if (m.rep > 1) meta += "heard x" + std::to_string(m.rep);
        if (m.hops >= 0) meta += std::string(meta.empty() ? "" : "  |  ") + std::to_string(m.hops) + (m.hops == 1 ? " hop" : " hops");
        if (m.hasSnr) meta += "  |  SNR " + fmtSnr(true, m.snr);
        if (!m.obs.empty()) {
          meta += "  |  via " + m.obs[0];
          if (m.obs.size() > 1) meta += " +" + std::to_string(m.obs.size() - 1);
        }
        lv_label_set_text(r.meta, meta.c_str());
        lv_obj_set_style_border_color(r.box, lv_color_hex(m.fresh ? T.fresh : C_LINE), 0);
        hide(r.box, false);
      }
    }
    bool muted = M->isMuted(curChan);
    if (muted) lv_label_set_text(chanSub, "Muted: no alerts or sounds from this channel");
    else lv_label_set_text_fmt(chanSub, "%d messages on the server  |  live messages appear here", count);
    setBtnText(chanMuteBtn, muted ? LV_SYMBOL_MUTE "  Muted" : LV_SYMBOL_BELL "  Mute");
    setBtnColors(chanMuteBtn, muted ? 0xFFB547 : C_BTN, muted ? 0x0A0E13 : C_TEXT);
  }
  hide(chanMuteBtn, curChan.empty());
  for (size_t i = n; i < msgRows.size(); i++) hide(msgRows[i].box, true);
  lv_label_set_text(msgEmpty, curChan.empty() ? "Channels appear once CoreScope sends them." : "No recent messages on this channel.");
  hide(msgEmpty, n > 0);
  if (toEnd) {
    lv_obj_update_layout(msgList);
    lv_obj_scroll_to_y(msgList, LV_COORD_MAX, LV_ANIM_OFF);
  }
}

// ================================================================ MY NODES
static lv_obj_t *mineList, *mineEmpty, *mineCount;
struct NCard {
  lv_obj_t *box, *dot, *name, *role, *status, *v[3], *btnMap, *btnRm;
};
static std::vector<NCard> ncards;
static void onCardMap(lv_event_t *e) {
  size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
  if (i >= M->myCards.size()) return;
  std::string k = M->myCards[i].key;
  showScreen(S_MAP);
  mapShowNode(k);
}
static void onCardRemove(lv_event_t *e) {
  size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
  if (i >= M->myCards.size()) return;
  auto c = M->myCards[i];
  M->setMyNode(c.key, c.name, false);
  mineDirty = true;
  mapMarkDirty();
}
static void buildMine(lv_obj_t *s) {
  lv_obj_t *h = mkLabel(s, F18, C_TEXT, "MY NODES");
  lv_obj_set_pos(h, 14, 16);
  mineCount = mkLabel(s, F14, C_DIM, "");
  lv_obj_set_pos(mineCount, 120, 19);
  lv_obj_t *find = mkBtn(s, "@  Find node / mentions", [](lv_event_t *) { openFind(""); }, nullptr, T.atBg, C_AT);
  lv_obj_align(find, LV_ALIGN_TOP_RIGHT, -12, 8);
  lv_obj_set_height(find, 44);
  lv_obj_t *rf = mkBtn(s, LV_SYMBOL_REFRESH "  Refresh", [](lv_event_t *) { P->requestMyNodes(); }, nullptr);
  lv_obj_align_to(rf, find, LV_ALIGN_OUT_LEFT_MID, -10, 0);
  lv_obj_set_height(rf, 44);
  mineList = mkBox(s, C_BG);
  lv_obj_set_pos(mineList, 0, 60);
  lv_obj_set_size(mineList, SCR_W, MAIN_H - 60);
  lv_obj_add_flag(mineList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(mineList, LV_DIR_VER);
  lv_obj_set_flex_flow(mineList, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_hor(mineList, 12, 0);
  lv_obj_set_style_pad_bottom(mineList, 12, 0);
  lv_obj_set_style_pad_row(mineList, 10, 0);
  lv_obj_set_style_pad_column(mineList, 10, 0);
  mineEmpty = mkLabel(mineList, F16, C_DIM,
                      "No nodes yet.\n\nTap a node on the map and press  + My Nodes,\nuse  Find node  above, or list names on the setup page.");
  lv_obj_set_style_pad_all(mineEmpty, 20, 0);
}
static NCard &ncard(size_t i) {
  while (ncards.size() <= i) {
    NCard c;
    size_t idx = ncards.size();
    c.box = mkBox(mineList, C_PANEL);
    lv_obj_set_size(c.box, 382, 160);
    lv_obj_set_style_radius(c.box, 12, 0);
    lv_obj_set_style_border_width(c.box, 1, 0);
    lv_obj_set_style_border_color(c.box, lv_color_hex(C_LINE), 0);
    c.dot = mkBox(c.box, 0x3DDC97);
    lv_obj_set_pos(c.dot, 14, 19);
    lv_obj_set_size(c.dot, 11, 11);
    lv_obj_set_style_radius(c.dot, 6, 0);
    c.name = mkLabel(c.box, F20, C_TEXT);
    lv_obj_set_pos(c.name, 34, 12);
    lv_obj_set_width(c.name, 240);
    oneLine(c.name);
    c.role = mkLabel(c.box, F12, C_DIM);
    lv_obj_align(c.role, LV_ALIGN_TOP_RIGHT, -14, 17);
    c.status = mkLabel(c.box, F14, C_DIM);
    lv_obj_set_pos(c.status, 34, 38);
    static const char *lbl[] = {"AVG SNR", "PKTS TODAY", "OBSERVERS"};
    for (int k = 0; k < 3; k++) {
      c.v[k] = mkLabel(c.box, F20, C_TEXT, "-");
      lv_obj_set_pos(c.v[k], 34 + k * 112, 64);
      lv_obj_t *l = mkLabel(c.box, F12, C_DIM2, lbl[k]);
      lv_obj_set_pos(l, 34 + k * 112, 88);
    }
    c.btnMap = mkBtn(c.box, LV_SYMBOL_GPS "  Show on map", onCardMap, (void *)(intptr_t)idx, C_BTN, C_TEXT, F14);
    lv_obj_set_pos(c.btnMap, 14, 112);
    lv_obj_set_height(c.btnMap, 38);
    c.btnRm = mkBtn(c.box, "Remove", onCardRemove, (void *)(intptr_t)idx, C_BTN, C_DIM, F14);
    lv_obj_align(c.btnRm, LV_ALIGN_TOP_RIGHT, -14, 112);
    lv_obj_set_height(c.btnRm, 38);
    ncards.push_back(c);
  }
  return ncards[i];
}
static void renderMine() {
  static const uint32_t stc[] = {0x3DDC97, 0xFFB547, 0xFF7A59};
  static const char *stn[] = {"Active", "Degraded", "Silent"};
  size_t n = M->myCards.size();
  for (size_t i = 0; i < n; i++) {
    auto &m = M->myCards[i];
    NCard &c = ncard(i);
    // checkerboard so neighbouring cards are easy to tell apart
    bool alt = (((i >> 1) + (i & 1)) & 1) != 0;
    lv_obj_set_style_bg_color(c.box, lv_color_hex(alt ? C_PANEL_ALT : C_PANEL), 0);
    lv_obj_set_style_border_color(c.box, lv_color_hex(alt ? T.border2 : C_LINE), 0);
    lv_obj_set_style_bg_color(c.dot, lv_color_hex(stc[m.status]), 0);
    lv_label_set_text(c.name, m.name.c_str());
    std::string r = roleName(m.role);
    upper(r);
    lv_label_set_text(c.role, r.c_str());
    std::string st = std::string(stn[m.status]) + "  |  heard " + (m.last ? ago(m.last) + " ago" : "never");
    lv_label_set_text(c.status, st.c_str());
    lv_label_set_text(c.v[0], fmtSnr(m.hasSnr, m.avgSnr).c_str());
    lv_label_set_text(c.v[1], m.today < 0 ? "-" : std::to_string(m.today).c_str());
    lv_label_set_text(c.v[2], m.observers < 0 ? "-" : std::to_string(m.observers).c_str());
    hide(c.btnMap, !m.hasLoc);
    hide(c.box, false);
  }
  for (size_t i = n; i < ncards.size(); i++) hide(ncards[i].box, true);
  hide(mineEmpty, n > 0);
  lv_label_set_text_fmt(mineCount, n ? "%d watched" : "", (int)n);
}

// ================================================================ STATUS
static lv_obj_t *stVal[4], *stInfo[7], *obsList, *obsCount, *qrSetup, *setupUrl, *obsHead, *radioBox, *dsPanel;
static std::vector<lv_obj_t *> obsRows;
static std::string lastSetupUrl;
static void buildStatus(lv_obj_t *s) {
  static const char *tl[] = {"CONNECTION", "PACKETS / MIN", "NODES KNOWN", "ON MAP"};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *t = mkBox(s, C_PANEL);
    lv_obj_set_pos(t, 12 + i * 196, 10);
    lv_obj_set_size(t, 188, 66);
    lv_obj_set_style_radius(t, 10, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(C_LINE), 0);
    lv_obj_t *l = mkLabel(t, F12, C_DIM, tl[i]);
    lv_obj_set_pos(l, 12, 8);
    stVal[i] = mkLabel(t, F24, C_TEXT, "-");
    lv_obj_set_pos(stVal[i], 12, 28);
  }
  lv_obj_t *info = mkBox(s, C_PANEL);
  lv_obj_set_pos(info, 12, 86);
  lv_obj_set_size(info, 470, MAIN_H - 96);
  lv_obj_set_style_radius(info, 10, 0);
  lv_obj_set_style_border_width(info, 1, 0);
  lv_obj_set_style_border_color(info, lv_color_hex(C_LINE), 0);
  static const char *il[] = {"Data", "Watching", "Radius", "Status", "Network", "Map card", "Version"};
  for (int i = 0; i < 7; i++) {
    lv_obj_t *l = mkLabel(info, F14, C_DIM, il[i]);
    lv_obj_set_pos(l, 14, 10 + i * 23);
    stInfo[i] = mkLabel(info, F14, C_TEXT, "-");
    lv_obj_set_pos(stInfo[i], 100, 10 + i * 23);
    lv_obj_set_width(stInfo[i], i == 0 || i == 6 ? 262 : 356);
    oneLine(stInfo[i]);
  }
  lv_obj_t *dsBtn = mkBtn(info, "Change", [](lv_event_t *) { showDataSource(); }, nullptr, C_BTN, C_ACC, F14);
  lv_obj_set_size(dsBtn, 86, 30);
  lv_obj_set_pos(dsBtn, 370, 4);
  // screen dimming when left alone (also on the settings page)
  lv_obj_t *dimBtn = mkBtn(info, "Dimming", [](lv_event_t *) { showDimChoice(); }, nullptr, C_BTN, C_ACC, F14);
  lv_obj_set_size(dimBtn, 86, 30);
  lv_obj_set_pos(dimBtn, 370, 142);
  // long status / error text scrolls so the reason is always readable
  for (int i : {3, 5}) {
    lv_label_set_long_mode(stInfo[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(stInfo[i], 9000, 0);
  }
  // QR codes: setup page + coffee
  qrSetup = lv_qrcode_create(info);
  lv_qrcode_set_size(qrSetup, 84);
  lv_qrcode_set_dark_color(qrSetup, lv_color_hex(0x0A0E13));
  lv_qrcode_set_light_color(qrSetup, lv_color_white());
  lv_obj_set_style_border_color(qrSetup, lv_color_white(), 0);
  lv_obj_set_style_border_width(qrSetup, 4, 0);
  lv_obj_set_pos(qrSetup, 14, 180);
  setupUrl = mkLabel(info, F12, C_DIM, "Settings\n-");
  lv_obj_set_pos(setupUrl, 112, 188);
  lv_obj_set_width(setupUrl, 124);
  lv_label_set_long_mode(setupUrl, LV_LABEL_LONG_WRAP);

  lv_obj_t *qrc = lv_qrcode_create(info);
  lv_qrcode_set_size(qrc, 84);
  lv_qrcode_set_dark_color(qrc, lv_color_hex(0x0A0E13));
  lv_qrcode_set_light_color(qrc, lv_color_white());
  lv_obj_set_style_border_color(qrc, lv_color_white(), 0);
  lv_obj_set_style_border_width(qrc, 4, 0);
  const char *coffee = "https://buymeacoffee.com/hamradiodude";
  lv_qrcode_update(qrc, coffee, (uint32_t)strlen(coffee));
  lv_obj_set_pos(qrc, 238, 180);
  lv_obj_t *ct = mkLabel(info, F12, C_DIM, "Enjoying MESHtop?\nScan to buy\nDudeTested a coffee.");
  lv_obj_set_pos(ct, 336, 188);

  lv_obj_t *op = mkBox(s, C_PANEL);
  lv_obj_set_pos(op, 492, 86);
  lv_obj_set_size(op, 296, MAIN_H - 96);
  lv_obj_set_style_radius(op, 10, 0);
  lv_obj_set_style_border_width(op, 1, 0);
  lv_obj_set_style_border_color(op, lv_color_hex(C_LINE), 0);
  obsHead = mkLabel(op, F14, C_TEXT, "OBSERVERS");
  lv_obj_set_pos(obsHead, 12, 10);
  obsCount = mkLabel(op, F14, C_DIM, "0");
  lv_obj_align(obsCount, LV_ALIGN_TOP_RIGHT, -12, 10);
  obsList = mkScroll(op, 0, 34, 296, MAIN_H - 96 - 36);
  lv_obj_set_style_pad_hor(obsList, 12, 0);
  // radio only: what your radio is doing, in place of CoreScope's observers
  radioBox = mkLabel(op, F14, C_TEXT, "");
  lv_obj_set_pos(radioBox, 12, 40);
  lv_obj_set_width(radioBox, 272);
  lv_label_set_long_mode(radioBox, LV_LABEL_LONG_WRAP);
  hide(radioBox, true);

  // where live data comes from: three choices over the STATUS page
  dsPanel = mkBox(s, C_BG, 235);
  lv_obj_set_size(dsPanel, SCR_W, MAIN_H);
  lv_obj_set_pos(dsPanel, 0, 0);
  lv_obj_add_flag(dsPanel, LV_OBJ_FLAG_CLICKABLE);  // swallow taps behind it
  hide(dsPanel, true);
}
static void updateDim();
void showDimChoice();
static void chooseDataSource(const char *mode) {
  hide(dsPanel, true);
  P->setDataSource(mode);
  obsDirty = true;
}
// A chooser card on dsPanel: title, options (value, title, description), the current value, what to do on a pick
struct Choice {
  const char *value, *title, *sub;
};
static void showChooser(const char *heading, const Choice *opts, int n, const std::string &cur, lv_event_cb_t pick) {
  lv_obj_clean(dsPanel);
  lv_obj_t *card = mkBox(dsPanel, C_PANEL);
  lv_obj_set_size(card, 520, LV_SIZE_CONTENT);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_all(card, 16, 0);
  lv_obj_set_style_pad_row(card, 10, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  mkLabel(card, F18, C_TEXT, heading);
  for (int i = 0; i < n; i++) {
    const Choice &o = opts[i];
    bool on = cur == o.value;
    lv_obj_t *b = mkBox(card, on ? T.sel : C_BG);
    lv_obj_set_size(b, 488, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_border_width(b, on ? 2 : 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(on ? C_ACC : C_LINE), 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, 2, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
    lv_obj_t *t = mkLabel(b, F16, on ? C_ACC : C_TEXT, (std::string(on ? LV_SYMBOL_OK "  " : "") + o.title).c_str());
    lv_obj_t *d = mkLabel(b, F12, C_DIM, o.sub);
    lv_obj_set_width(d, 464);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, pick, LV_EVENT_CLICKED, (void *)o.value);
  }
  lv_obj_t *cancel = mkBtn(card, "Cancel", [](lv_event_t *) { hide(dsPanel, true); }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_size(cancel, 120, 38);
  hide(dsPanel, false);
  lv_obj_move_foreground(dsPanel);
}
void closeChooser() { hide(dsPanel, true); }
void showDimChoice() {
  static const Choice opts[] = {
      {"auto", "At night", "When left alone between sunset and sunrise, the screen dims. A touch brings it back."},
      {"always", "Always", "The screen dims whenever it's left alone, day or night."},
      {"off", "Never", "Always full brightness."},
  };
  showChooser("Dim the screen when it's left alone", opts, 3, M->cfg.dimMode, [](lv_event_t *e) {
    hide(dsPanel, true);
    P->setDimMode((const char *)lv_event_get_user_data(e));
    updateDim();
  });
}
void showDataSource() {
  lv_obj_clean(dsPanel);
  lv_obj_t *card = mkBox(dsPanel, C_PANEL);
  lv_obj_set_size(card, 520, LV_SIZE_CONTENT);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_all(card, 16, 0);
  lv_obj_set_style_pad_row(card, 10, 0);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  mkLabel(card, F18, C_TEXT, "Show live data from");
  std::string cur = M->cfg.radioOnly() ? "radio" : M->cfg.radioOn ? "both" : "corescope";
  struct Opt {
    const char *mode, *title, *sub;
  };
  static const Opt opts[] = {
      {"both", "CoreScope and my radio", "The full picture: the region's live map and chatter, plus your own radio."},
      {"corescope", "CoreScope only", "No radio connected. Everything comes from CoreScope."},
      {"radio", "My radio only", "CoreScope is never contacted. The map shows your radio's contacts and what it hears."},
  };
  for (auto &o : opts) {
    bool on = cur == o.mode;
    lv_obj_t *b = mkBox(card, on ? T.sel : C_BG);
    lv_obj_set_size(b, 488, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_border_width(b, on ? 2 : 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(on ? C_ACC : C_LINE), 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, 2, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
    lv_obj_t *t = mkLabel(b, F16, on ? C_ACC : C_TEXT, (std::string(on ? LV_SYMBOL_OK "  " : "") + o.title).c_str());
    lv_obj_t *d = mkLabel(b, F12, C_DIM, o.sub);
    lv_obj_set_width(d, 464);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, [](lv_event_t *e) { chooseDataSource((const char *)lv_event_get_user_data(e)); }, LV_EVENT_CLICKED,
                        (void *)o.mode);
  }
  lv_obj_t *cancel = mkBtn(card, "Cancel", [](lv_event_t *) { hide(dsPanel, true); }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_size(cancel, 120, 38);
  hide(dsPanel, false);
  lv_obj_move_foreground(dsPanel);
}
static void renderStatus() {
  int64_t now = P->nowMs();
  static const char *cn[] = {"Connecting", "LIVE", "Offline", "Demo"};
  bool ro = radioOnlyMode();
  Companion *rad = P->radio();
  if (ro) lv_label_set_text(stVal[0], rad && rad->state == R_READY ? "Radio" : "No radio");
  else lv_label_set_text(stVal[0], cn[P->conn()]);
  lv_label_set_text_fmt(stVal[1], "%d", M->pktPerMin(now));
  lv_label_set_text_fmt(stVal[2], "%d", (int)M->nodes.size());
  lv_label_set_text_fmt(stVal[3], "%d", mapNodesOnMap());
  std::string host = M->cfg.server;
  if (host.rfind("://") != std::string::npos) host = host.substr(host.rfind("://") + 3);
  std::string data = M->cfg.demo        ? "Demo mode"
                     : ro               ? "Your radio only (CoreScope off)"
                     : M->cfg.radioOn ? "CoreScope + radio  |  " + host
                                        : "CoreScope only  |  " + host;
  lv_label_set_text(stInfo[0], data.c_str());
  // the right-hand panel: CoreScope's observers, or your radio when CoreScope is off
  lv_label_set_text(obsHead, ro ? "YOUR RADIO" : "OBSERVERS");
  hide(obsCount, ro);
  hide(obsList, ro);
  hide(radioBox, !ro);
  if (ro) {
    std::string t;
    if (!rad || rad->state == R_OFF) t = "Your radio is off.";
    else if (rad->state != R_READY) t = std::string(radioStateName(rad->state)) + "\n" + rad->status;
    else {
      t = (rad->selfName.empty() ? std::string("Connected") : rad->selfName) + "\n";
      if (rad->battMv > 0) t += "Battery " + std::to_string(rad->battMv / 1000) + "." + std::to_string(rad->battMv % 1000 / 100) + " V\n";
      t += std::to_string(rad->contacts.size()) + " contacts, " + std::to_string(mapNodesOnMap()) + " on the map\n";
      t += std::to_string(M->pktPerMin(now)) + " packets heard in the last minute";
    }
    t += "\n\nCoreScope is off: nothing is fetched from it or sent to it. FEED and CHATTER return when you switch it back on.";
    lv_label_set_text(radioBox, t.c_str());
  }
  std::string area = M->repeaterName.empty() ? M->areaLabel : "Repeater " + M->repeaterName;
  lv_label_set_text(stInfo[1], area.c_str());
  if (M->repeaterName.empty()) lv_label_set_text_fmt(stInfo[2], "%d mi", M->cfg.radiusMi);
  else lv_label_set_text(stInfo[2], "n/a (repeater mode)");
  lv_label_set_text(stInfo[3], P->connMsg().c_str());
  lv_label_set_text(stInfo[4], P->netInfo().c_str());
  std::string card = P->cardInfo();
  if (card != lv_label_get_text(stInfo[5])) lv_label_set_text(stInfo[5], card.c_str());  // same text: keep it scrolling
  lv_label_set_text(stInfo[6], ("MESHtop " + P->version() + " for CrowPanel 7\"").c_str());
  std::string url = P->settingsUrl();
  if (url != lastSetupUrl) {
    lastSetupUrl = url;
    lv_qrcode_update(qrSetup, url.c_str(), (uint32_t)url.size());
    std::string shown = url.rfind("http://", 0) == 0 ? url.substr(7) : url;
    lv_label_set_text(setupUrl, ("Settings page:\nscan, or open\n" + shown).c_str());
  }
}
static void renderObservers() {
  if (radioOnlyMode()) return;
  auto obs = M->observers;
  std::sort(obs.begin(), obs.end(), [](const Observer &a, const Observer &b) { return a.pkh > b.pkh; });
  lv_label_set_text_fmt(obsCount, "%d", (int)obs.size());
  size_t n = std::min<size_t>(obs.size(), 40);
  int64_t now = P->nowMs();
  for (size_t i = 0; i < n; i++) {
    while (obsRows.size() <= i) {
      lv_obj_t *r = mkBox(obsList, C_PANEL);
      lv_obj_set_size(r, 272, 40);
      lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(r, 1, 0);
      lv_obj_set_style_border_color(r, lv_color_hex(T.obsLine), 0);
      lv_obj_t *d = mkBox(r, 0x3DDC97);
      lv_obj_set_size(d, 8, 8);
      lv_obj_set_style_radius(d, 4, 0);
      lv_obj_align(d, LV_ALIGN_LEFT_MID, 0, 0);
      lv_obj_t *nm = mkLabel(r, F14, C_TEXT);
      lv_obj_set_pos(nm, 16, 3);
      lv_obj_set_width(nm, 180);
      oneLine(nm);
      lv_obj_t *sub = mkLabel(r, F12, C_DIM2);
      lv_obj_set_pos(sub, 16, 22);
      lv_obj_t *pk = mkLabel(r, F14, C_DIM);
      lv_obj_align(pk, LV_ALIGN_RIGHT_MID, 0, 0);
      obsRows.push_back(r);
    }
    lv_obj_t *r = obsRows[i];
    auto &o = obs[i];
    bool fresh = o.last && now - o.last < 15 * 60000;
    lv_obj_set_style_bg_color(lv_obj_get_child(r, 0), lv_color_hex(fresh ? 0x3DDC97 : 0x5A6675), 0);
    lv_label_set_text(lv_obj_get_child(r, 1), o.name.empty() ? o.id.substr(0, 8).c_str() : o.name.c_str());
    std::string sub = (o.iata.empty() ? std::string("") : o.iata + "  |  ") + "seen " + ago(o.last) + " ago";
    lv_label_set_text(lv_obj_get_child(r, 2), sub.c_str());
    lv_label_set_text_fmt(lv_obj_get_child(r, 3), "%d/h", o.pkh);
    hide(r, false);
  }
  for (size_t i = n; i < obsRows.size(); i++) hide(obsRows[i], true);
}

// ================================================================ FIND NODE + @MENTIONS
static lv_obj_t *findPanel, *findTa, *findKb, *findRes, *findWatch, *findHint;
static std::vector<Node *> findList;
static std::vector<std::string> findKeys;
static lv_timer_t *findTimer;
struct RRow {
  lv_obj_t *box, *name, *sub, *star, *at, *map;
};
static std::vector<RRow> resRows;

static void renderFind();
static void onResStar(lv_event_t *e) {
  size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
  if (i >= findKeys.size()) return;
  Node *n = M->findKey(findKeys[i]);
  if (!n) return;
  M->setMyNode(n->key, n->name, !M->isMine(n->key));
  P->requestMyNodes();
  mineDirty = true;
  mapMarkDirty();
  renderFind();
}
static void onResAt(lv_event_t *e) {
  size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
  if (i >= findKeys.size()) return;
  Node *n = M->findKey(findKeys[i]);
  if (!n) return;
  const Watch *w = M->watchFor(n->key, n->name);
  if (w && w->src != 0) return;
  M->setMention(n->key, n->name, !w);
  renderFind();
}
static void onResMap(lv_event_t *e) {
  size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
  if (i >= findKeys.size()) return;
  std::string k = findKeys[i];
  hide(findPanel, true);
  showScreen(S_MAP);
  mapShowNode(k);
}
static void onUnwatch(lv_event_t *e) {
  auto *w = (Watch *)lv_event_get_user_data(e);
  (void)w;
}
static void runFind() {
  std::string q = lv_textarea_get_text(findTa);
  findList = M->search(q, 20);
  findKeys.clear();
  for (auto *n : findList) findKeys.push_back(n->key);
  renderFind();
}
static void findTimerCb(lv_timer_t *) {
  lv_timer_pause(findTimer);
  runFind();
  std::string q = lv_textarea_get_text(findTa);
  if (q.size() >= 2 && findList.size() < 10) P->requestSearch(q);
}
static void renderFind() {
  // watched names
  lv_obj_clean(findWatch);
  auto ws = M->watches();
  if (ws.empty()) {
    mkLabel(findWatch, F14, C_DIM2, "Not watching any @mentions yet. Tap  @ Watch  on a node.");
  }
  for (auto &w : ws) {
    std::string t = "@" + w.name + (w.src == 0 ? "  " LV_SYMBOL_CLOSE : (w.src == 2 ? "  (My Node)" : "  (settings)"));
    lv_obj_t *c = mkBtn(findWatch, t.c_str(), nullptr, nullptr, w.src == 0 ? T.atBg : C_PANEL, w.src == 0 ? C_AT : C_DIM2, F14);
    lv_obj_set_height(c, 32);
    lv_obj_set_style_radius(c, 16, 0);
    if (w.src == 0) {
      std::string *keyName = new std::string(w.key + "\n" + w.name);
      lv_obj_add_event_cb(
          c,
          [](lv_event_t *e) {
            auto *s = (std::string *)lv_event_get_user_data(e);
            size_t nl = s->find('\n');
            M->setMention(s->substr(0, nl), s->substr(nl + 1), false);
            lv_async_call([](void *) { renderFind(); }, nullptr);
          },
          LV_EVENT_CLICKED, keyName);
      lv_obj_add_event_cb(
          c, [](lv_event_t *e) { delete (std::string *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, keyName);
    }
  }
  // results
  std::string q = lv_textarea_get_text(findTa);
  size_t n = findKeys.size();
  for (size_t i = 0; i < n; i++) {
    while (resRows.size() <= i) {
      RRow r;
      size_t idx = resRows.size();
      r.box = mkBox(findRes, (idx & 1) ? T.rowAlt : C_BG);
      lv_obj_set_size(r.box, 776, 54);
      r.name = mkLabel(r.box, F18, C_TEXT);
      lv_obj_set_pos(r.name, 12, 6);
      lv_obj_set_width(r.name, 430);
      oneLine(r.name);
      r.sub = mkLabel(r.box, F12, C_DIM);
      lv_obj_set_pos(r.sub, 12, 31);
      r.map = mkBtn(r.box, "Map", onResMap, (void *)(intptr_t)idx, C_BTN, C_TEXT, F14);
      lv_obj_set_size(r.map, 70, 42);
      lv_obj_align(r.map, LV_ALIGN_RIGHT_MID, -8, 0);
      r.at = mkBtn(r.box, "@ Watch", onResAt, (void *)(intptr_t)idx, C_BTN, C_AT, F14);
      lv_obj_set_size(r.at, 100, 42);
      lv_obj_align(r.at, LV_ALIGN_RIGHT_MID, -86, 0);
      r.star = mkBtn(r.box, LV_SYMBOL_PLUS " My Nodes", onResStar, (void *)(intptr_t)idx, C_BTN, C_STAR, F14);
      lv_obj_set_size(r.star, 128, 42);
      lv_obj_align(r.star, LV_ALIGN_RIGHT_MID, -194, 0);
      resRows.push_back(r);
    }
    RRow &r = resRows[i];
    Node *nd = M->findKey(findKeys[i]);
    if (!nd) {
      hide(r.box, true);
      continue;
    }
    lv_label_set_text(r.name, nd->name.c_str());
    std::string role = roleName(nd->role);
    upper(role);
    std::string sub = role + "  |  " + nd->key.substr(0, 8) + (nd->last ? "  |  heard " + ago(nd->last) + " ago" : "");
    lv_label_set_text(r.sub, sub.c_str());
    bool mine = M->isMine(nd->key);
    setBtnText(r.star, mine ? LV_SYMBOL_OK " Mine" : LV_SYMBOL_PLUS " My Nodes");
    setBtnColors(r.star, mine ? T.starOnBg : C_BTN, C_STAR);
    const Watch *w = M->watchFor(nd->key, nd->name);
    setBtnText(r.at, w ? "@ On" : "@ Watch");
    setBtnColors(r.at, w ? T.atOnBg : C_BTN, C_AT);
    hide(r.map, !nd->hasLoc);
    hide(r.box, false);
  }
  for (size_t i = n; i < resRows.size(); i++) hide(resRows[i].box, true);
  lv_label_set_text(findHint, q.empty() ? "Start typing to find a node by name."
                                        : (n ? "" : ("No nodes match \"" + q + "\" yet.").c_str()));
  hide(findHint, n > 0);
}
static void buildFind() {
  findPanel = mkBox(scr, C_BG);
  lv_obj_set_size(findPanel, SCR_W, SCR_H);
  lv_obj_add_flag(findPanel, LV_OBJ_FLAG_CLICKABLE);
  findTa = lv_textarea_create(findPanel);
  lv_textarea_set_one_line(findTa, true);
  lv_textarea_set_placeholder_text(findTa, "Type a node name...");
  lv_textarea_set_max_length(findTa, 30);
  lv_obj_set_pos(findTa, 10, 8);
  lv_obj_set_size(findTa, 650, 46);
  lv_obj_set_style_text_font(findTa, F20, 0);
  lv_obj_set_style_bg_color(findTa, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_border_color(findTa, lv_color_hex(C_ACC), LV_STATE_FOCUSED);
  lv_obj_add_state(findTa, LV_STATE_FOCUSED);
  lv_obj_add_event_cb(
      findTa,
      [](lv_event_t *) {
        lv_timer_reset(findTimer);
        lv_timer_resume(findTimer);
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t *done = mkBtn(findPanel, "Done", [](lv_event_t *) { hide(findPanel, true); mineDirty = true; }, nullptr, C_BTN, C_TEXT, F18);
  lv_obj_set_pos(done, 672, 8);
  lv_obj_set_size(done, 118, 46);
  findWatch = mkBox(findPanel, C_BG);
  lv_obj_set_pos(findWatch, 10, 60);
  lv_obj_set_size(findWatch, 780, 38);
  lv_obj_add_flag(findWatch, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(findWatch, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(findWatch, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(findWatch, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(findWatch, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(findWatch, 8, 0);
  findRes = mkScroll(findPanel, 12, 102, 776, 164);
  findHint = mkLabel(findRes, F16, C_DIM, "");
  lv_obj_set_style_pad_all(findHint, 16, 0);
  findKb = lv_keyboard_create(findPanel);
  lv_obj_set_size(findKb, SCR_W, 210);
  lv_obj_align(findKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(findKb, findTa);
  lv_obj_set_style_text_font(findKb, F18, 0);
  lv_obj_add_event_cb(findKb, [](lv_event_t *) { hide(findPanel, true); mineDirty = true; }, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(findKb, [](lv_event_t *) { hide(findPanel, true); mineDirty = true; }, LV_EVENT_CANCEL, nullptr);
  if (!findTimer) {
    findTimer = lv_timer_create(findTimerCb, 300, nullptr);
    lv_timer_pause(findTimer);
  }
  hide(findPanel, true);
  (void)onUnwatch;
}
void openFind(const char *q) {
  lv_textarea_set_text(findTa, q ? q : "");
  hide(findPanel, false);
  lv_obj_move_foreground(findPanel);
  runFind();
}

// ================================================================ ALERTS
static lv_obj_t *alertsPanel, *alertsList, *alertsEmpty;
static std::vector<lv_obj_t *> alertRows;
static lv_obj_t *banner, *bannerBar, *bannerTitle, *bannerText, *flash;
static Alert bannerAlert;
static lv_timer_t *bannerTimer;

static void openAlert(const Alert &a) {
  hide(alertsPanel, true);
  hide(banner, true);
  if (!a.radioConv.empty()) {
    showScreen(S_RADIO);
    radioOpenConv(a.radioConv);
  } else if (!a.channel.empty()) {
    showScreen(S_CHAN);
    selectChannel(a.channel);
  } else if (!a.key.empty() && M->findKey(a.key)) {
    showScreen(S_MAP);
    mapShowNode(a.key);
  } else if (a.hasLoc) {
    showScreen(S_MAP);
    mapSetView(a.lat, a.lon, 12);
  }
}
static void onAlertRow(lv_event_t *e) {
  size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
  if (i < M->alerts.size()) openAlert(M->alerts[i]);
}
static void renderAlerts() {
  size_t n = M->alerts.size();
  for (size_t i = 0; i < n; i++) {
    while (alertRows.size() <= i) {
      lv_obj_t *r = mkBox(alertsList, (alertRows.size() & 1) ? T.rowAlt : C_BG);
      lv_obj_set_size(r, SCR_W, 58);
      lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(r, onAlertRow, LV_EVENT_CLICKED, (void *)(intptr_t)alertRows.size());
      lv_obj_t *bar = mkBox(r, C_ACC);
      lv_obj_set_pos(bar, 12, 10);
      lv_obj_set_size(bar, 5, 38);
      lv_obj_set_style_radius(bar, 2, 0);
      lv_obj_t *t = mkLabel(r, F18, C_TEXT);
      lv_obj_set_pos(t, 30, 7);
      lv_obj_set_width(t, 660);
      oneLine(t);
      lv_obj_t *b = mkLabel(r, F14, C_DIM);
      lv_obj_set_pos(b, 30, 32);
      lv_obj_set_width(b, 680);
      oneLine(b);
      lv_obj_t *ag = mkLabel(r, F14, C_DIM2);
      lv_obj_align(ag, LV_ALIGN_RIGHT_MID, -14, 0);
      alertRows.push_back(r);
    }
    auto &a = M->alerts[i];
    lv_obj_t *r = alertRows[i];
    lv_obj_set_style_bg_color(lv_obj_get_child(r, 0), lv_color_hex(a.color), 0);
    lv_label_set_text(lv_obj_get_child(r, 1), a.title.c_str());
    lv_label_set_text(lv_obj_get_child(r, 2), a.body.c_str());
    lv_label_set_text(lv_obj_get_child(r, 3), ago(a.ts).c_str());
    hide(r, false);
  }
  for (size_t i = n; i < alertRows.size(); i++) hide(alertRows[i], true);
  hide(alertsEmpty, n > 0);
}
static void buildAlerts() {
  alertsPanel = mkBox(scr, C_BG);
  lv_obj_set_pos(alertsPanel, 0, TOP_H);
  lv_obj_set_size(alertsPanel, SCR_W, MAIN_H);
  lv_obj_add_flag(alertsPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *h = mkLabel(alertsPanel, F18, C_TEXT, "ALERTS");
  lv_obj_set_pos(h, 14, 16);
  lv_obj_t *cl = mkBtn(alertsPanel, "Close", [](lv_event_t *) { hide(alertsPanel, true); }, nullptr);
  lv_obj_align(cl, LV_ALIGN_TOP_RIGHT, -12, 8);
  lv_obj_set_height(cl, 44);
  lv_obj_t *clr = mkBtn(alertsPanel, "Clear", [](lv_event_t *) { M->alerts.clear(); renderAlerts(); }, nullptr);
  lv_obj_align_to(clr, cl, LV_ALIGN_OUT_LEFT_MID, -10, 0);
  lv_obj_set_height(clr, 44);
  lv_obj_t *ts = mkBtn(alertsPanel, "Test alert", [](lv_event_t *) { M->testAlert(); }, nullptr);
  lv_obj_align_to(ts, clr, LV_ALIGN_OUT_LEFT_MID, -10, 0);
  lv_obj_set_height(ts, 44);
  alertsList = mkScroll(alertsPanel, 0, 60, SCR_W, MAIN_H - 60);
  alertsEmpty = mkLabel(alertsList, F16, C_DIM,
                        "No alerts yet.\n\nAlerts cover: My Nodes back on air / gone silent, keywords,\nnew nodes nearby and @mentions of nodes you watch.");
  lv_obj_set_style_pad_all(alertsEmpty, 20, 0);
  hide(alertsPanel, true);
}
void openAlerts() {
  unseenAlerts = 0;
  hide(findPanel, true);
  renderAlerts();
  hide(alertsPanel, false);
  lv_obj_move_foreground(alertsPanel);
  updateTopBar();
}

static void flashOpa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static void buildBanner() {
  lv_obj_t *top = lv_layer_top();
  banner = mkBox(top, T.bannerBg, 245);
  lv_obj_set_size(banner, 560, LV_SIZE_CONTENT);
  lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, 46);
  lv_obj_set_style_radius(banner, 12, 0);
  lv_obj_set_style_border_width(banner, 1, 0);
  lv_obj_set_style_border_color(banner, lv_color_hex(T.border2), 0);
  lv_obj_set_style_pad_left(banner, 24, 0);
  lv_obj_set_style_pad_right(banner, 14, 0);
  lv_obj_set_style_pad_ver(banner, 10, 0);
  lv_obj_set_style_shadow_width(banner, 24, 0);
  lv_obj_set_style_shadow_color(banner, lv_color_black(), 0);
  lv_obj_set_style_shadow_opa(banner, 160, 0);
  lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(banner, 2, 0);
  lv_obj_add_flag(banner, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(banner, [](lv_event_t *) { openAlert(bannerAlert); }, LV_EVENT_CLICKED, nullptr);
  bannerBar = mkBox(banner, C_ACC);
  lv_obj_add_flag(bannerBar, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_set_pos(bannerBar, -14, 0);
  lv_obj_set_size(bannerBar, 6, 44);
  lv_obj_set_style_radius(bannerBar, 3, 0);
  bannerTitle = mkLabel(banner, F20, C_TEXT);
  lv_obj_set_width(bannerTitle, 510);
  oneLine(bannerTitle);
  bannerText = mkLabel(banner, F14, C_DIM);
  lv_obj_set_width(bannerText, 510);
  lv_label_set_long_mode(bannerText, LV_LABEL_LONG_WRAP);
  hide(banner, true);
  if (!bannerTimer) {
    bannerTimer = lv_timer_create([](lv_timer_t *t) { hide(banner, true); lv_timer_pause(t); }, 9000, nullptr);
    lv_timer_pause(bannerTimer);
  }

  flash = mkBox(top, C_ACC, LV_OPA_TRANSP);
  lv_obj_set_size(flash, SCR_W, SCR_H);
  lv_obj_set_style_border_width(flash, 10, 0);
  lv_obj_set_style_border_color(flash, lv_color_hex(C_ACC), 0);
  lv_obj_set_style_border_opa(flash, LV_OPA_COVER, 0);
  lv_obj_set_style_opa(flash, 0, 0);
  lv_obj_remove_flag(flash, LV_OBJ_FLAG_CLICKABLE);
}

// ================================================================ first-run Wi-Fi setup
// Pick a network right on the panel (tap + type the password), or use the phone hotspot as plan B.
static lv_obj_t *setupPanel, *setupQr, *setupSsid, *setupStatus, *setupList, *setupScanBtn;
static std::vector<WifiItem> wifiItems;
static bool wifiTrying = false;
static void wifiAsk(const std::string &ssid, bool secure) {
  if (!secure) {
    P->wifiConnect(ssid, "");
    return;
  }
  askText("Password for " + ssid, "Capital letters count. Tap the arrow key for symbols and numbers.", "", 63, "Connect",
          [ssid](const std::string &pw) {
            if (pw.size() < 8) {
              radioToastCb(false, "Wi-Fi passwords are at least 8 characters");
              return false;
            }
            P->wifiConnect(ssid, pw);
            return true;
          });
}
static void renderWifiList() {
  lv_obj_clean(setupList);
  if (wifiItems.empty()) {
    lv_obj_t *l = mkLabel(setupList, F16, C_DIM, wifiTrying ? "" : "Looking for networks...");
    lv_obj_set_style_pad_all(l, 12, 0);
  }
  for (size_t i = 0; i < wifiItems.size(); i++) {
    auto &w = wifiItems[i];
    lv_obj_t *row = mkBox(setupList, (i & 1) ? T.rowAlt : C_PANEL);
    lv_obj_set_size(row, lv_pct(100), 52);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, [](lv_event_t *e) {
      size_t k = (size_t)(intptr_t)lv_event_get_user_data(e);
      if (k < wifiItems.size() && !wifiTrying) wifiAsk(wifiItems[k].ssid, wifiItems[k].secure);
    }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *nm = mkLabel(row, F18, C_TEXT, w.ssid.c_str());
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_width(nm, 290);
    oneLine(nm);
    int bars = w.rssi > -55 ? 4 : w.rssi > -67 ? 3 : w.rssi > -75 ? 2 : 1;
    lv_obj_t *bx = lv_obj_create(row);
    lv_obj_remove_style_all(bx);
    lv_obj_set_size(bx, 22, 16);
    lv_obj_align(bx, LV_ALIGN_RIGHT_MID, -14, 0);
    for (int k = 0; k < 4; k++) {
      lv_obj_t *bar = mkBox(bx, k < bars ? (bars >= 3 ? 0x3DDC97 : bars == 2 ? 0xFFB547 : 0xFF7A59) : T.btnBorder);
      lv_obj_set_size(bar, 4, 4 + k * 4);
      lv_obj_set_pos(bar, k * 6, 12 - k * 4);
    }
    if (w.secure) {
      lv_obj_t *lk = mkLabel(row, F14, C_DIM2, LV_SYMBOL_EYE_CLOSE);
      lv_obj_align(lk, LV_ALIGN_RIGHT_MID, -46, 0);
    }
  }
}
static void buildSetup() {
  setupPanel = mkBox(scr, C_BG);
  lv_obj_set_size(setupPanel, SCR_W, SCR_H);
  lv_obj_add_flag(setupPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *t1 = mkLabel(setupPanel, F28, C_TEXT, "Welcome to MESHtop");
  lv_obj_set_pos(t1, 24, 16);
  lv_obj_t *t2 = mkLabel(setupPanel, F16, C_DIM, "Tap your Wi-Fi network and type its password.");
  lv_obj_set_pos(t2, 24, 54);
  setupStatus = mkLabel(setupPanel, F14, C_STAR, "");
  lv_obj_set_pos(setupStatus, 24, 80);
  lv_obj_set_width(setupStatus, 470);
  lv_label_set_long_mode(setupStatus, LV_LABEL_LONG_WRAP);
  setupList = mkBox(setupPanel, C_PANEL);
  lv_obj_set_pos(setupList, 24, 122);
  lv_obj_set_size(setupList, 470, 290);
  lv_obj_set_style_radius(setupList, 12, 0);
  lv_obj_set_style_border_width(setupList, 1, 0);
  lv_obj_set_style_border_color(setupList, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_clip_corner(setupList, true, 0);
  lv_obj_add_flag(setupList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(setupList, LV_DIR_VER);
  lv_obj_set_flex_flow(setupList, LV_FLEX_FLOW_COLUMN);
  setupScanBtn = mkBtn(setupPanel, LV_SYMBOL_REFRESH "  Scan again", [](lv_event_t *) {
    if (wifiTrying) return;
    wifiItems.clear();
    renderWifiList();
    P->wifiScan();
  }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_pos(setupScanBtn, 24, 422);
  lv_obj_set_height(setupScanBtn, 44);
  lv_obj_t *hidden = mkBtn(setupPanel, "Hidden network...", [](lv_event_t *) {
    askText("Network name", "Type the exact Wi-Fi name (capital letters count).", "", 32, "Next", [](const std::string &ssid) {
      if (ssid.empty()) return false;
      std::string *sp = new std::string(ssid);
      lv_async_call([](void *p) {
        std::string *s2 = (std::string *)p;
        wifiAsk(*s2, true);
        delete s2;
      }, sp);
      return true;
    });
  }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_pos(hidden, 180, 422);
  lv_obj_set_height(hidden, 44);

  // plan B: the phone hotspot
  lv_obj_t *pb = mkLabel(setupPanel, F16, C_TEXT, "Or use your phone");
  lv_obj_set_pos(pb, 540, 92);
  setupQr = lv_qrcode_create(setupPanel);
  lv_qrcode_set_size(setupQr, 170);
  lv_qrcode_set_dark_color(setupQr, lv_color_hex(0x0A0E13));
  lv_qrcode_set_light_color(setupQr, lv_color_white());
  lv_obj_set_style_border_color(setupQr, lv_color_white(), 0);
  lv_obj_set_style_border_width(setupQr, 6, 0);
  lv_obj_set_pos(setupQr, 548, 122);
  setupSsid = mkLabel(setupPanel, F12, C_DIM, "");
  lv_obj_set_pos(setupSsid, 520, 312);
  lv_obj_set_width(setupSsid, 250);
  lv_label_set_long_mode(setupSsid, LV_LABEL_LONG_WRAP);
  lv_obj_t *ft = mkLabel(setupPanel, F12, C_DIM2, "The panel needs 2.4 GHz Wi-Fi.\nTo change Wi-Fi later: settings page > Reset Wi-Fi.");
  lv_obj_set_pos(ft, 520, 412);
  lv_obj_set_width(ft, 262);
  lv_label_set_long_mode(ft, LV_LABEL_LONG_WRAP);
  hide(setupPanel, true);
}
void showSetup(bool on, const char *ssid) {
  bool wasShown = setupShown;
  setupShown = on;
  if (ssid) setupSsidText = ssid;
  if (on) {
    std::string s = setupSsidText.empty() ? "MESHtop-Setup" : setupSsidText;
    std::string t = "Scan to join \"" + s + "\", then pick your Wi-Fi on the page that opens (or browse to 192.168.4.1).";
    lv_label_set_text(setupSsid, t.c_str());
    std::string wifi = "WIFI:T:nopass;S:" + s + ";;";
    lv_qrcode_update(setupQr, wifi.c_str(), (uint32_t)wifi.size());
    hide(setupPanel, false);
    lv_obj_move_foreground(setupPanel);
    renderWifiList();
    if (!wasShown && wifiItems.empty()) P->wifiScan();
  } else {
    hide(setupPanel, true);
  }
}
void wifiScanResult(const std::vector<WifiItem> &items) {
  wifiItems = items;
  if (setupShown) renderWifiList();
}
void wifiStatus(const std::string &text, bool ok, bool trying) {
  wifiTrying = trying;
  if (!setupStatus) return;
  lv_label_set_text(setupStatus, ((trying ? LV_SYMBOL_REFRESH "  " : ok ? LV_SYMBOL_OK "  " : LV_SYMBOL_WARNING "  ") + text).c_str());
  lv_obj_set_style_text_color(setupStatus, lv_color_hex(ok ? 0x3DDC97 : trying ? C_ACC : 0xFF7A59), 0);
  if (ok) radioToastCb(true, text);
}

// ================================================================ screens, idle, dimming
void showScreen(int s) {
  if (s < 0 || s >= S_COUNT) return;
  hide(alertsPanel, true);
  hide(findPanel, true);
  curScreen = s;
  for (int i = 1; i < S_COUNT; i++) hide(screens[i], i != s);
  hide(screens[S_MAP], s != S_MAP);
  mapShownChanged(s == S_MAP);
  styleTabs();
  if (s == S_FEED) feedDirty = true;
  if (s == S_CHAN) {
    if (M->channels.empty()) P->requestChannels();
    if (curChan.empty() && !M->channels.empty()) selectChannel(M->channels[0].name);
    else if (!curChan.empty()) selectChannel(curChan);
    chanListDirty = true;
    msgDirty = true;
  }
  if (s == S_MINE) mineDirty = true;
  if (s == S_STATUS) obsDirty = true;
  if (s == S_RADIO) radioDirty();
  updateTopBar();
}
void showScreenByName(const char *n) {
  static const char *names[] = {"map", "feed", "chan", "mine", "mesh", "status"};
  for (int i = 0; i < S_COUNT; i++)
    if (!strcmp(n, names[i])) showScreen(i);
}

static double sunAltitude(double lat, double lon, int64_t ms) {
  const double rad = M_PI / 180;
  double d = ms / 86400000.0 + 2440587.5 - 2451545.0;
  double g = std::fmod(357.529 + 0.98560028 * d, 360) * rad;
  double q = std::fmod(280.459 + 0.98564736 * d, 360);
  double L = (q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * rad;
  double e = (23.439 - 0.00000036 * d) * rad;
  double ra = std::atan2(std::cos(e) * std::sin(L), std::cos(L));
  double dec = std::asin(std::sin(e) * std::sin(L));
  double gmst = std::fmod(18.697374558 + 24.06570982441908 * d, 24.0);
  double ha = (gmst * 15 + lon) * rad - ra;
  return std::asin(std::sin(lat * rad) * std::sin(dec) + std::cos(lat * rad) * std::cos(dec) * std::cos(ha)) / rad;
}
static int lastBacklight = -1;
static void updateDim() {
  int target = 100;
  const std::string &mode = M->cfg.dimMode;
  if (idle && mode != "off") {
    bool night = P->timeValid() && sunAltitude(M->center.lat, M->center.lon, P->nowMs()) < -6;
    if (mode == "always" || night) target = 22;
  }
  if (target != lastBacklight) {
    lastBacklight = target;
    P->setBacklight(target);
  }
}
void touched() {}

// Untouched for a while (Settings: idle minutes): go back to the live map and dim (at night by default).
// The first touch brings the brightness back and works as normal; there is no separate screensaver.
static void goIdle() {
  if (idle) return;
  if (curScreen != S_MAP) showScreen(S_MAP);
  hide(alertsPanel, true);
  hide(findPanel, true);
  idle = true;
  updateDim();
}
static void wake() {
  if (!idle) return;
  idle = false;
  updateDim();
}
void goIdleNow() { goIdle(); }
bool isIdle() { return idle; }

// ================================================================ timers
static bool wantDay();
static void applyTheme(bool day);
static void fastTimer(lv_timer_t *) {  // 250 ms
  if (idle && lv_display_get_inactive_time(nullptr) < 1000) wake();  // touched: full brightness again
  if (curScreen == S_MAP) {
    mapTick();
    if (mapFeedDirty) {
      mapFeedDirty = false;
      mapRenderFeed();
    }
  }
  if (feedDirty && curScreen == S_FEED && lv_obj_has_flag(findPanel, LV_OBJ_FLAG_HIDDEN)) {
    feedDirty = false;
    renderFeed();
  }
  if (curScreen == S_CHAN) {
    if (chanListDirty) {
      chanListDirty = false;
      renderChanList();
    }
    if (msgDirty) {
      msgDirty = false;
      renderMessages(true);
    }
  }
  radioTick(curScreen == S_RADIO && !radioComposeOpen());
  if (mineDirty && curScreen == S_MINE) {
    mineDirty = false;
    renderMine();
  }
}
static void secondTimer(lv_timer_t *) {
  static int tick = 0;
  tick++;
  updateTopBar();
  if (curScreen == S_MAP) mapFeedDirty = true;  // refresh the "ago" times
  if (curScreen == S_STATUS) {
    renderStatus();
    if (obsDirty || tick % 10 == 0) {
      obsDirty = false;
      renderObservers();
    }
  }
  if (curScreen == S_FEED && tick % 5 == 0) feedDirty = true;
  if (curScreen == S_CHAN && tick % 15 == 0) chanListDirty = true;
  if (tick % 60 == 0) M->checkSilent();
  if (tick % 20 == 0) updateDim();
  if (tick % 30 == 5) applyTheme(wantDay());
  int idleMin = M->cfg.idleMin;
  if (!idle && idleMin > 0 && lv_display_get_inactive_time(nullptr) > (uint32_t)idleMin * 60000 &&
      lv_obj_has_flag(findPanel, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(setupPanel, LV_OBJ_FLAG_HIDDEN) &&
      !radioComposeOpen())
    goIdle();
}

// ================================================================ public API
static void buildAll() {
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  buildTopBar();
  mainArea = mkBox(scr, C_BG);
  lv_obj_set_pos(mainArea, 0, TOP_H);
  lv_obj_set_size(mainArea, SCR_W, MAIN_H);
  for (int i = 1; i < S_COUNT; i++) {
    screens[i] = mkBox(mainArea, C_BG);
    lv_obj_set_size(screens[i], SCR_W, MAIN_H);
  }
  buildFeed(screens[S_FEED]);
  buildChannels(screens[S_CHAN]);
  buildMine(screens[S_MINE]);
  buildStatus(screens[S_STATUS]);
  radioBuild(screens[S_RADIO], scr);
  mapCreate(scr);
  screens[S_MAP] = lv_obj_get_child(scr, -1);
  buildTabs();
  buildAlerts();
  buildFind();
  buildBanner();
  buildSetup();
}

static void setPalette(bool day) {
  T = day ? DAY : DARK;
  lv_display_t *d = lv_display_get_default();
  lv_theme_t *th = lv_theme_default_init(d, lv_color_hex(T.acc), lv_color_hex(T.star), T.dark, LV_FONT_DEFAULT);
  lv_display_set_theme(d, th);
}

static bool wantDay() {
  const std::string &t = M->cfg.theme;
  if (t == "day") return true;
  if (t == "dark" || !P->timeValid()) return false;
  return sunAltitude(M->center.lat, M->center.lon, P->nowMs()) > -4;  // auto: day from dawn to dusk
}

static void applyTheme(bool day) {
  if (day == dayMode()) return;
  int was = curScreen;
  setPalette(day);
  mapResetObjects();
  lv_obj_clean(lv_layer_top());
  lv_obj_clean(scr);
  feedRows.clear();
  chanRows.clear();
  msgRows.clear();
  ncards.clear();
  obsRows.clear();
  resRows.clear();
  alertRows.clear();
  lastSetupUrl.clear();
  buildAll();
  showScreen(was);
  if (setupShown) showSetup(true, setupSsidText.c_str());
  feedDirty = chanListDirty = msgDirty = mineDirty = obsDirty = mapFeedDirty = true;
  mapMarkDirty();
  updateTopBar();
}

void toggleTheme() {
  bool day = !dayMode();
  M->cfg.theme = day ? "day" : "dark";
  P->saveSettings();
  applyTheme(day);
}
void themeChanged() { applyTheme(wantDay()); }

void begin(Model *m, Platform *p) {
  M = m;
  P = p;
  scr = lv_screen_active();
  setPalette(wantDay());
  buildAll();
  lv_timer_create(fastTimer, 250, nullptr);
  lv_timer_create(secondTimer, 1000, nullptr);
  showScreen(S_MAP);
  updateDim();
}

void onRoute(const Route &r) {
  if (curScreen == S_MAP && passes(r)) mapRoute(r);
  feedDirty = true;
  mapFeedDirty = true;
}
void onAlert(const Alert &a) {
  bannerAlert = a;
  unseenAlerts++;
  lv_obj_set_style_bg_color(bannerBar, lv_color_hex(a.color), 0);
  lv_label_set_text(bannerTitle, a.title.c_str());
  lv_label_set_text(bannerText, a.body.c_str());
  hide(bannerText, a.body.empty());
  hide(banner, false);
  lv_timer_reset(bannerTimer);
  lv_timer_resume(bannerTimer);
  // edge flash in the alert colour
  lv_obj_set_style_border_color(flash, lv_color_hex(a.color), 0);
  lv_anim_t an;
  lv_anim_init(&an);
  lv_anim_set_var(&an, flash);
  lv_anim_set_exec_cb(&an, flashOpa);
  lv_anim_set_values(&an, 255, 0);
  lv_anim_set_duration(&an, 1400);
  lv_anim_set_path_cb(&an, lv_anim_path_ease_in);
  lv_anim_start(&an);
  if (M->cfg.alertSound) P->beep(a.kind == A_MENTION ? 220 : 140);
  if (!lv_obj_has_flag(alertsPanel, LV_OBJ_FLAG_HIDDEN)) renderAlerts();
  updateTopBar();
}
void onChatMsg(const std::string &channel, const ChatMsg &) {
  chanListDirty = true;
  if (Model::chKey(channel.empty() ? "public" : channel) == Model::chKey(curChan) && curScreen == S_CHAN) {
    for (auto &c : M->channels)
      if (Model::chKey(c.name) == Model::chKey(curChan)) c.unread = 0;
    msgDirty = true;
  }
}
void mapChanged() {
  mapMarkDirty();
  mapRefreshCard();
}
void configChanged() {
  layoutTabs();
  mapSourceChanged();
  if (radioOnlyMode() && (curScreen == S_FEED || curScreen == S_CHAN)) showScreen(S_MAP);
  obsDirty = true;
  mapRecenter();
  feedDirty = mapFeedDirty = true;
  updateTopBar();
}
void channelsChanged() {
  chanListDirty = true;
  if (curScreen == S_CHAN && curChan.empty() && !M->channels.empty()) selectChannel(M->channels[0].name);
}
void historyChanged(const std::string &channel) {
  if (Model::chKey(channel) == Model::chKey(curChan)) msgDirty = true;
}
void myNodesChanged() {
  mineDirty = true;
  mapMarkDirty();
}
void observersChanged() { obsDirty = true; }
void radioChanged() { radioDirty(); }
void radioResult(bool ok, const std::string &text) { radioToastCb(ok, text); }
void showRadioSection(const char *name) {
  showScreen(S_RADIO);
  radioShowSection(name);
}
void onRadioMessage(const Conv &c, const RMsg &m) {
  radioDirty();
  if (m.mine || radioViewing(c.id)) return;  // already looking at it
  if (c.isChannel && M->isMuted(c.title)) return;  // muted: no alerts or sounds
  Companion *r = P->radio();
  bool mention = r && !r->selfName.empty() && (mentionMatch(m.text, r->selfName));
  if (!c.isChannel || mention) {
    Alert a;
    a.kind = A_MENTION;
    a.title = c.isChannel ? "@" + r->selfName + " in " + c.title + " (radio)" : "Message from " + c.title;
    a.body = (c.isChannel && !m.who.empty() ? m.who + ": " : "") + m.text;
    a.radioConv = c.id;
    M->raiseAlert(a, "radio:" + c.id + ":" + m.text.substr(0, 30));
  }
}
void searchChanged() {
  if (!lv_obj_has_flag(findPanel, LV_OBJ_FLAG_HIDDEN)) runFind();
}

}  // namespace ui
