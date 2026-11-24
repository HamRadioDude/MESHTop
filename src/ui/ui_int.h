// Internal shared bits of the UI
#pragma once
#include <functional>
#include <string>

#include "lvgl.h"
#include "core/companion.h"
#include "ui.h"

namespace ui {

extern st::Model *M;
extern st::Platform *P;

// Colour palette: dark (night) or day. Switching theme rebuilds the UI with the other palette.
struct Palette {
  uint32_t bg, panel, panelAlt, btn, line, text, dim, dim2, acc, star, at;
  uint32_t rowAlt, listBg, listLine, sel, border2, btnBorder, pressed, badgeText, chipOn, atBg, starOnBg, atOnBg;
  uint32_t tabActive, tabPressed, bannerBg, fresh, obsLine, listPressed, areaFg, grid, labelFg, labelHi, ring, onAcc;
  uint32_t connBg[4], connFg[4];
  bool dark;
};
extern Palette T;
uint32_t gcol(st::Group g);  // packet-type colour tuned for the current theme
uint32_t rcol(st::Role r);   // node-role colour tuned for the current theme
bool dayMode();
void toggleTheme();  // map button: flip day/night (manual)

#define C_BG (T.bg)
#define C_PANEL (T.panel)
#define C_PANEL_ALT (T.panelAlt)
#define C_BTN (T.btn)
#define C_LINE (T.line)
#define C_TEXT (T.text)
#define C_DIM (T.dim)
#define C_DIM2 (T.dim2)
#define C_ACC (T.acc)
#define C_STAR (T.star)
#define C_AT (T.at)

constexpr int SCR_W = 800, SCR_H = 480, TOP_H = 40, TAB_H = 52, MAIN_H = SCR_H - TOP_H - TAB_H;  // 388

enum Screen { S_MAP = 0, S_FEED, S_CHAN, S_MINE, S_RADIO, S_STATUS, S_COUNT };
extern int curScreen;

lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *txt = "");
lv_obj_t *mkBox(lv_obj_t *parent, uint32_t bg, lv_opa_t opa = LV_OPA_COVER);
lv_obj_t *mkBtn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud = nullptr, uint32_t bg = C_BTN,
                uint32_t fg = C_TEXT, const lv_font_t *f = &lv_font_montserrat_16);
void setBtnText(lv_obj_t *btn, const char *txt);
void oneLine(lv_obj_t *label);  // truncate with ... on a single line
void setBtnColors(lv_obj_t *btn, uint32_t bg, uint32_t fg);
std::string ago(int64_t ts);
std::string fmtSnr(bool has, float v);
std::string feedDetail(const st::Route &r);
bool passes(const st::Route &r);
void showScreen(int s);
void touched();

// map (ui_map.cpp)
void mapCreate(lv_obj_t *parent);
void mapRoute(const st::Route &r);
void mapMarkDirty();
void mapTick();
void mapSetView(double lat, double lon, int z);
void mapRecenter();
void mapShownChanged(bool visible);
void mapRefreshCard();
void mapRenderFeed();
int mapNodesOnMap();
void mapSourceChanged();  // the map's credit line follows the data source
void mapShowNode(const std::string &key);
void mapResetObjects();  // before a theme rebuild
// A route from MESH pinned on the map until cleared: a message's route or a trace (a line through
// the stops in order), or the radios that heard you (star: lines from the first stop to each other).
// Stops without a known position are skipped in the drawing.
struct PathStop {
  std::string label;
  bool hasLoc = false;
  double lat = 0, lon = 0;
};
void mapShowPath(const std::string &title, const std::vector<PathStop> &stops, bool star);
void mapClearPath();

// MESH screen (ui_radio.cpp)
void radioBuild(lv_obj_t *screen, lv_obj_t *scr);
void radioDirty();
void radioTick(bool visible);
void radioOpenConv(const std::string &id);
int radioUnread();
bool radioComposeOpen();
bool radioViewing(const std::string &convId);
void radioToastCb(bool ok, const std::string &text);
void radioShowSection(const char *name);
// shared full-screen keyboard prompt (ui_radio.cpp); return false from cb to keep it open
void askText(const std::string &title, const std::string &hint, const std::string &initial, int maxLen, const char *okLabel,
             std::function<bool(const std::string &)> cb);  // chats | people | channels | radio | wizard

}  // namespace ui
