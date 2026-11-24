// MESH (MeshThing): a friendly MeshCore companion on the panel.
// Sections: CHATS, PEOPLE, CHANNELS, MY RADIO, plus a first-connect setup wizard, help pop-ups and toasts.
// Talks to your own MeshCore companion radio (e.g. XIAO nRF52840 + Wio-SX1262) over Bluetooth via st::Companion.
#include <strings.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "ui_int.h"
#include "core/textfix.h"

namespace ui {
using namespace st;

static const lv_font_t *F12 = &lv_font_montserrat_12, *F14 = &lv_font_montserrat_14, *F16 = &lv_font_montserrat_16,
                       *F18 = &lv_font_montserrat_18, *F20 = &lv_font_montserrat_20, *F24 = &lv_font_montserrat_24;

static Companion *R() { return P->radio(); }
static bool ready() { return R() && R()->state == R_READY; }
static void hide(lv_obj_t *o, bool h) {
  if (!o) return;
  if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}
static constexpr int RAIL_W = 96, CW = SCR_W - RAIL_W, CH = MAIN_H;  // content area 704 x 388
enum Sec { SEC_CHATS = 0, SEC_PEOPLE, SEC_CHANNELS, SEC_RADIO, SEC_COUNT };
static int sec = SEC_CHATS;
static bool dirty[SEC_COUNT] = {true, true, true, true};
static lv_obj_t *scrRoot;  // the whole LVGL screen (for overlays)

// ================================================================ small shared widgets
static uint32_t avatarColor(const std::string &s) {
  static const uint32_t c[] = {0x39D0FF, 0xFFB547, 0x3DDC97, 0xB78CFF, 0xF472B6, 0xFF7A59, 0x5EEAD4, 0xA3E635};
  uint32_t h = 2166136261u;
  for (unsigned char ch : s) h = (h ^ ch) * 16777619u;
  return c[h % 8];
}
static std::string initials(const std::string &name) {
  std::string out;
  bool next = true;
  for (unsigned char ch : name) {
    if (isalnum(ch)) {
      if (next && out.size() < 2) out += (char)toupper(ch);
      next = false;
    } else next = true;
  }
  if (out.empty()) out = "?";
  return out;
}
static lv_obj_t *mkAvatar(lv_obj_t *parent, int size) {
  lv_obj_t *a = mkBox(parent, C_ACC);
  lv_obj_set_size(a, size, size);
  lv_obj_set_style_radius(a, size / 2, 0);
  lv_obj_t *l = mkLabel(a, size >= 56 ? F24 : (size >= 40 ? F16 : F12), 0x0A0E13, "?");
  lv_obj_center(l);
  return a;
}
static void setAvatar(lv_obj_t *a, const std::string &name, uint8_t type) {
  uint32_t col = type == CT_REPEATER ? 0xFFB547 : type == CT_ROOM ? 0xB78CFF : type == CT_SENSOR ? 0x3DDC97 : avatarColor(name);
  lv_obj_set_style_bg_color(a, lv_color_hex(col), 0);
  std::string t = type == CT_REPEATER ? "R" : type == CT_ROOM ? "#" : initials(name);
  lv_label_set_text(lv_obj_get_child(a, 0), t.c_str());
}
// four little signal bars
static lv_obj_t *mkBars(lv_obj_t *parent) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, 22, 14);
  for (int i = 0; i < 4; i++) {
    lv_obj_t *bar = mkBox(b, C_DIM2);
    lv_obj_set_size(bar, 4, 4 + i * 3);
    lv_obj_set_pos(bar, i * 6, 10 - i * 3);
    lv_obj_set_style_radius(bar, 1, 0);
  }
  return b;
}
static int snrBars(float snr) { return snr >= 8 ? 4 : snr >= 3 ? 3 : snr >= -3 ? 2 : snr >= -9 ? 1 : 0; }
static void setBars(lv_obj_t *b, int level) {
  uint32_t on = level >= 3 ? 0x3DDC97 : level == 2 ? 0xFFB547 : 0xFF7A59;
  for (int i = 0; i < 4; i++) lv_obj_set_style_bg_color(lv_obj_get_child(b, i), lv_color_hex(i < level ? on : T.btnBorder), 0);
}
static const char *typeName(uint8_t t) {
  switch (t) {
    case CT_CHAT: return "Person";
    case CT_REPEATER: return "Repeater";
    case CT_ROOM: return "Room";
    case CT_SENSOR: return "Sensor";
    default: return "Node";
  }
}
static std::string routeText(int pathLen) {
  if (pathLen < 0) return "no route yet (searches the whole mesh)";
  if (pathLen == 0) return "direct, in radio range";
  return "via " + std::to_string(pathLen) + (pathLen == 1 ? " repeater" : " repeaters");
}
static std::string agoLong(int64_t ts) {
  if (!ts) return "never";
  int64_t s = (P->nowMs() - ts) / 1000;
  if (s < 0) s = 0;
  if (s < 90) return "just now";
  if (s < 3600) return std::to_string(s / 60) + " min ago";
  if (s < 86400) return std::to_string(s / 3600) + (s / 3600 == 1 ? " hour ago" : " hours ago");
  return std::to_string(s / 86400) + (s / 86400 == 1 ? " day ago" : " days ago");
}
static LatLon myPlace() {
  Companion *r = R();
  if (r && r->hasSelfLoc) return {r->selfLat, r->selfLon};
  return M->center;
}
static lv_obj_t *mkCard(lv_obj_t *parent, int w) {
  lv_obj_t *c = mkBox(parent, C_PANEL);
  lv_obj_set_size(c, w, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(c, 12, 0);
  lv_obj_set_style_border_width(c, 1, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_pad_all(c, 14, 0);
  lv_obj_set_style_pad_row(c, 8, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  return c;
}
static lv_obj_t *mkRow(lv_obj_t *parent) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_remove_style_all(r);
  lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 8, 0);
  lv_obj_set_style_pad_row(r, 8, 0);
  return r;
}
static lv_obj_t *wrapLabel(lv_obj_t *parent, const lv_font_t *f, uint32_t col, const std::string &t) {
  lv_obj_t *l = mkLabel(parent, f, col, t.c_str());
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}

// ================================================================ toast, modal, prompt, help
static lv_obj_t *toastBox, *toastLbl;
static lv_timer_t *toastTimer;
void radioToast(bool ok, const std::string &text) {
  if (!toastBox || text.empty()) return;
  lv_obj_set_style_bg_color(toastBox, lv_color_hex(ok ? (T.dark ? 0x10251C : 0xD6F2E4) : (T.dark ? 0x2C1212 : 0xFADBD7)), 0);
  lv_obj_set_style_border_color(toastBox, lv_color_hex(ok ? 0x3DDC97 : 0xFF7A59), 0);
  lv_label_set_text(toastLbl, ((ok ? LV_SYMBOL_OK "  " : LV_SYMBOL_WARNING "  ") + text).c_str());
  hide(toastBox, false);
  lv_obj_move_foreground(toastBox);
  lv_timer_reset(toastTimer);
  lv_timer_resume(toastTimer);
}

static lv_obj_t *modalBack, *modalTitle, *modalText, *modalOk, *modalCancel, *modalAlt;
static std::function<void()> modalAltCb;  // the optional middle button (e.g. Copy)
static lv_obj_t *rtBack, *rtTitle, *rtSub, *rtList, *rtMapBtn;  // sheet: a message's route, or "Who can hear me?"
static void discoveryOnMap();
static std::function<void()> modalCb;
static bool traceOpen = false;  // the pop-up is showing a live trace (see refreshTrace)
static void modal(const std::string &title, const std::string &text, const char *okLabel, std::function<void()> onOk, bool danger = false,
                  const char *cancelLabel = "Cancel") {
  traceOpen = false;  // whoever opens the pop-up now owns it; startTrace sets this again after
  if (modalAlt) hide(modalAlt, true);
  modalAltCb = nullptr;
  lv_label_set_text(modalTitle, title.c_str());
  lv_label_set_text(modalText, text.c_str());
  modalCb = std::move(onOk);
  setBtnText(modalOk, okLabel);
  setBtnColors(modalOk, danger ? 0xE5484D : C_ACC, danger ? 0xFFFFFF : T.onAcc);
  hide(modalCancel, cancelLabel == nullptr);
  if (cancelLabel) setBtnText(modalCancel, cancelLabel);
  hide(modalBack, false);
  lv_obj_move_foreground(modalBack);
}
static void help(const std::string &title, const std::string &text) { modal(title, text, "Got it", nullptr, false, nullptr); }
static void modalExtra(const char *label, std::function<void()> cb) {  // after modal(): a third button
  setBtnText(modalAlt, label);
  modalAltCb = std::move(cb);
  hide(modalAlt, false);
}

static lv_obj_t *promptPanel, *promptTitle, *promptHint, *promptTa, *promptKb, *promptOk, *promptChips;
static std::function<bool(const std::string &)> promptCb;
static void promptClose() { hide(promptPanel, true); }
static void promptSubmit() {
  std::string t = lv_textarea_get_text(promptTa);
  if (promptCb && !promptCb(t)) return;  // callback can refuse (stays open)
  promptClose();
}
static void prompt(const std::string &title, const std::string &hint, const std::string &initial, int maxLen, const char *okLabel,
                   std::function<bool(const std::string &)> cb, const std::vector<std::string> &chips = {}) {
  lv_label_set_text(promptTitle, title.c_str());
  lv_label_set_text(promptHint, hint.c_str());
  lv_textarea_set_max_length(promptTa, maxLen);
  lv_textarea_set_text(promptTa, initial.c_str());
  setBtnText(promptOk, okLabel);
  promptCb = std::move(cb);
  lv_obj_clean(promptChips);
  for (auto &c : chips) {
    lv_obj_t *b = mkBtn(promptChips, c.c_str(), [](lv_event_t *e) {
      lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
      std::string t = lv_label_get_text(lv_obj_get_child(btn, 0));
      std::string cur = lv_textarea_get_text(promptTa);
      if (!t.empty() && t[0] == '@') lv_textarea_add_text(promptTa, ((cur.empty() || cur.back() == ' ' ? "" : " ") + t + " ").c_str());
      else lv_textarea_set_text(promptTa, t.c_str());
    }, nullptr, C_PANEL, C_TEXT, F14);
    lv_obj_set_height(b, 34);
    lv_obj_set_style_radius(b, 17, 0);
  }
  hide(promptChips, chips.empty());
  hide(promptPanel, false);
  lv_obj_move_foreground(promptPanel);
}

void askText(const std::string &title, const std::string &hint, const std::string &initial, int maxLen, const char *okLabel,
             std::function<bool(const std::string &)> cb) {
  prompt(title, hint, initial, maxLen, okLabel, std::move(cb));
}

// Bluetooth PIN, editable right on the panel (radios with a screen show a random PIN; others use
// 123456 unless it was changed in the MeshCore app)
static lv_obj_t *offPinBtn = nullptr;
static void askRadioPin() {
  prompt("Radio Bluetooth PIN", "The 6-digit PIN your radio shows on its screen, or the one set in the MeshCore app. Default is 123456.",
         P->radioPin(), 6, "Use this PIN", [](const std::string &v) {
           if (v.empty() || v.size() > 6 || v.find_first_not_of("0123456789") != std::string::npos) {
             radioToast(false, "The PIN is up to 6 digits");
             return false;
           }
           P->setRadioPin((uint32_t)std::stoul(v));
           radioToast(true, "PIN saved, reconnecting");
           return true;
         });
}

static void buildOverlays(lv_obj_t *scr) {
  // prompt (full screen keyboard)
  promptPanel = mkBox(scr, C_BG);
  lv_obj_set_size(promptPanel, SCR_W, SCR_H);
  lv_obj_add_flag(promptPanel, LV_OBJ_FLAG_CLICKABLE);
  promptTitle = mkLabel(promptPanel, F20, C_TEXT, "");
  lv_obj_set_pos(promptTitle, 14, 10);
  promptHint = mkLabel(promptPanel, F12, C_DIM, "");
  lv_obj_set_pos(promptHint, 14, 38);
  lv_obj_set_width(promptHint, 640);
  oneLine(promptHint);
  promptTa = lv_textarea_create(promptPanel);
  lv_obj_set_pos(promptTa, 10, 58);
  lv_obj_set_size(promptTa, 640, 92);
  lv_obj_set_style_text_font(promptTa, F20, 0);
  lv_obj_set_style_bg_color(promptTa, lv_color_hex(C_PANEL), 0);
  lv_obj_add_state(promptTa, LV_STATE_FOCUSED);
  promptOk = mkBtn(promptPanel, "OK", [](lv_event_t *) { promptSubmit(); }, nullptr, C_ACC, T.onAcc, F18);
  lv_obj_set_pos(promptOk, 662, 10);
  lv_obj_set_size(promptOk, 128, 66);
  lv_obj_t *cancel = mkBtn(promptPanel, "Cancel", [](lv_event_t *) { promptClose(); }, nullptr, C_BTN, C_TEXT, F16);
  lv_obj_set_pos(cancel, 662, 84);
  lv_obj_set_size(cancel, 128, 66);
  promptChips = mkBox(promptPanel, C_BG);
  lv_obj_set_pos(promptChips, 10, 156);
  lv_obj_set_size(promptChips, 780, 40);
  lv_obj_add_flag(promptChips, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(promptChips, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(promptChips, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(promptChips, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(promptChips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(promptChips, 8, 0);
  promptKb = lv_keyboard_create(promptPanel);
  lv_obj_set_size(promptKb, SCR_W, 276);
  lv_obj_align(promptKb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(promptKb, promptTa);
  lv_obj_set_style_text_font(promptKb, F18, 0);
  lv_obj_add_event_cb(promptKb, [](lv_event_t *) { promptSubmit(); }, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(promptKb, [](lv_event_t *) { promptClose(); }, LV_EVENT_CANCEL, nullptr);
  hide(promptPanel, true);

  // modal dialog
  modalBack = mkBox(scr, 0x000000, 150);
  lv_obj_set_size(modalBack, SCR_W, SCR_H);
  lv_obj_add_flag(modalBack, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *m = mkCard(modalBack, 520);
  lv_obj_center(m);
  lv_obj_set_style_pad_all(m, 20, 0);
  lv_obj_set_style_shadow_width(m, 30, 0);
  lv_obj_set_style_shadow_opa(m, 150, 0);
  modalTitle = wrapLabel(m, F20, C_TEXT, "");
  modalText = wrapLabel(m, F16, C_DIM, "");
  lv_obj_t *row = mkRow(m);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_margin_top(row, 8, 0);
  modalCancel = mkBtn(row, "Cancel", [](lv_event_t *) { hide(modalBack, true); }, nullptr, C_BTN, C_TEXT, F16);
  lv_obj_set_height(modalCancel, 48);
  modalAlt = mkBtn(row, "Copy", [](lv_event_t *) {
    hide(modalBack, true);
    auto cb = modalAltCb;
    modalAltCb = nullptr;
    if (cb) cb();
  }, nullptr, C_BTN, C_TEXT, F16);
  lv_obj_set_height(modalAlt, 48);
  hide(modalAlt, true);
  modalOk = mkBtn(row, "OK", [](lv_event_t *) {
    hide(modalBack, true);
    auto cb = modalCb;
    modalCb = nullptr;
    if (cb) cb();
  }, nullptr, C_ACC, T.onAcc, F16);
  lv_obj_set_height(modalOk, 48);
  lv_obj_set_style_min_width(modalOk, 110, 0);
  hide(modalBack, true);

  // route sheet (how someone else's message reached you)
  rtBack = mkBox(scr, 0x000000, 150);
  lv_obj_set_size(rtBack, SCR_W, SCR_H);
  lv_obj_add_flag(rtBack, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(rtBack, [](lv_event_t *e) {
    if (lv_event_get_target(e) == rtBack) hide(rtBack, true);  // tap outside the card closes it
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *rc = mkCard(rtBack, 660);
  lv_obj_set_height(rc, SCR_H - 40);
  lv_obj_center(rc);
  lv_obj_set_style_pad_all(rc, 16, 0);
  lv_obj_set_style_shadow_width(rc, 30, 0);
  lv_obj_set_style_shadow_opa(rc, 150, 0);
  lv_obj_t *rh = mkRow(rc);
  lv_obj_set_width(rh, lv_pct(100));
  lv_obj_set_flex_align(rh, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_t *rhl = lv_obj_create(rh);
  lv_obj_remove_style_all(rhl);
  lv_obj_set_size(rhl, 360, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rhl, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rhl, 2, 0);
  rtTitle = mkLabel(rhl, F20, C_TEXT, "");
  rtSub = mkLabel(rhl, F12, C_DIM, "");
  lv_obj_set_width(rtSub, 360);
  lv_label_set_long_mode(rtSub, LV_LABEL_LONG_DOT);
  rtMapBtn = mkBtn(rh, LV_SYMBOL_GPS "  Show on map", [](lv_event_t *) { discoveryOnMap(); }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_height(rtMapBtn, 44);
  lv_obj_add_flag(rtMapBtn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *rx = mkBtn(rh, "Close", [](lv_event_t *) { hide(rtBack, true); }, nullptr, C_BTN, C_TEXT, F16);
  lv_obj_set_height(rx, 44);
  rtList = lv_obj_create(rc);
  lv_obj_remove_style_all(rtList);
  lv_obj_set_width(rtList, lv_pct(100));
  lv_obj_set_flex_grow(rtList, 1);
  lv_obj_set_flex_flow(rtList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(rtList, 10, 0);
  lv_obj_add_flag(rtList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(rtList, LV_DIR_VER);
  hide(rtBack, true);

  // toast
  toastBox = mkBox(lv_layer_top(), 0x10251C, 245);
  lv_obj_set_size(toastBox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_max_width(toastBox, 700, 0);
  lv_obj_align(toastBox, LV_ALIGN_BOTTOM_MID, 0, -64);
  lv_obj_set_style_radius(toastBox, 22, 0);
  lv_obj_set_style_border_width(toastBox, 1, 0);
  lv_obj_set_style_pad_hor(toastBox, 18, 0);
  lv_obj_set_style_pad_ver(toastBox, 10, 0);
  toastLbl = mkLabel(toastBox, F16, C_TEXT, "");
  lv_obj_set_style_max_width(toastLbl, 660, 0);
  lv_label_set_long_mode(toastLbl, LV_LABEL_LONG_WRAP);
  hide(toastBox, true);
  if (!toastTimer) {
    toastTimer = lv_timer_create([](lv_timer_t *t) {
      hide(toastBox, true);
      lv_timer_pause(t);
    }, 3500, nullptr);
    lv_timer_pause(toastTimer);
  }
}

static const char *HELP_HELLO =
    "Your radio introduces itself with a short 'hello' packet called an advert. Other radios use it to add you to their "
    "People list and to learn your name and (if you share it) your location.\n\n"
    "Hello nearby: only radios that hear you directly.\nHello everywhere: repeaters pass it on across the mesh.\n\n"
    "Say hello after changing your name, and every now and then so people keep seeing you.";
static const char *HELP_ROUTE =
    "Messages hop from repeater to repeater. 'Direct' means the other radio hears you without help. "
    "'Via 2 repeaters' means two repeaters relayed it.\n\n"
    "With no route yet, your radio searches the whole mesh (a flood) and remembers the path that worked. "
    "If messages to someone stop arriving, tap Forget route so it searches again.";
static const char *HELP_CHANNELS =
    "Channels are group chats everyone in range can join.\n\n"
    "Public: the shared channel every MeshCore radio starts with.\n"
    "Hashtag (#name): anyone who types the same name joins the same channel. Easy, but not secret.\n"
    "Private: protected by a secret key. Share the QR code only with people you trust.";
static const char *HELP_NOBODY =
    "New radios show up when they say hello, which can take a few minutes.\n\n"
    "1. Make sure your region matches your area (My Radio > Region).\n"
    "2. Say hello everywhere so others add you.\n"
    "3. Check the MAP tab: if you see traffic near you, repeaters are in range.\n"
    "4. A better antenna, or putting the radio near a window, helps a lot.";
static const char *HELP_REPEATER =
    "Repeaters relay messages so they travel further. Most let anyone check their status as a guest: leave the "
    "password empty. The owner can log in with the admin password.\n\n"
    "Rooms are like message boards: log in and the posts arrive as messages.";

// ================================================================ layout: rail + sections
static lv_obj_t *root, *rail, *railBtn[SEC_COUNT], *railDot, *railStatus;
static lv_obj_t *secRoot[SEC_COUNT];
static lv_obj_t *offPanel, *offTitle, *offText, *offBtn;

static void showSection(int s) {
  sec = s;
  for (int i = 0; i < SEC_COUNT; i++) {
    bool on = i == s;
    lv_obj_set_style_bg_color(railBtn[i], lv_color_hex(on ? T.sel : T.listBg), 0);
    lv_obj_set_style_border_width(railBtn[i], on ? 3 : 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(railBtn[i], 0), lv_color_hex(on ? C_ACC : C_DIM), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(railBtn[i], 1), lv_color_hex(on ? C_TEXT : C_DIM), 0);
    hide(secRoot[i], !on);
  }
  dirty[s] = true;
}

// ================================================================ CHATS
static lv_obj_t *convList, *paneTitle, *paneSub, *msgList, *msgEmpty, *composeBtn, *quickRow, *chatEmpty, *muteBtn;
static std::string curConv;
struct ConvRow {
  lv_obj_t *box, *av, *title, *last, *badge, *time;
};
static std::vector<ConvRow> convRows;
struct MsgRow {
  lv_obj_t *wrap, *box, *who, *meta, *text, *bars;
};
static std::vector<MsgRow> msgRows;
static const char *QUICK[] = {"73!", "Copy that", "Anyone on?", "Hearing you loud and clear", "On my way", "Thanks!", "Checking in from my desk"};

static void selectConv(const std::string &id) {
  curConv = id;
  if (R()) R()->markRead(id);
  dirty[SEC_CHATS] = true;
}
static void sendTo(const std::string &convId, const std::string &text) {
  if (!R() || text.empty()) return;
  if (!R()->sendText(convId, text)) radioToast(false, "Radio not connected");
  dirty[SEC_CHATS] = true;
}
static void openComposeWith(const std::string &initial) {
  Companion *r = R();
  Conv *c = r ? r->conv(curConv) : nullptr;
  if (!c) return;
  std::vector<std::string> chips;
  if (c->isChannel) {  // @mention the most recent people in this channel
    for (auto it = c->msgs.rbegin(); it != c->msgs.rend() && chips.size() < 6; ++it)
      if (!it->mine && !it->who.empty()) {
        std::string m = "@[" + it->who + "]";
        if (std::find(chips.begin(), chips.end(), m) == chips.end()) chips.push_back(m);
      }
  }
  std::string id = curConv;
  prompt((c->isChannel ? "Message to " : "Private message to ") + c->title,
         c->isChannel ? "Everyone on this channel can read it. Max 140 characters." : "Only " + c->title + " can read it. Max 140 characters.", initial,
         140, LV_SYMBOL_UPLOAD " Send", [id](const std::string &t) {
           sendTo(id, t);
           return true;
         }, chips);
}
static void openCompose(lv_event_t *) { openComposeWith(""); }

static void renderChats();
static void buildChats(lv_obj_t *s) {
  lv_obj_t *cl = mkBox(s, T.listBg);
  lv_obj_set_size(cl, 250, CH);
  lv_obj_set_style_border_side(cl, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(cl, 1, 0);
  lv_obj_set_style_border_color(cl, lv_color_hex(C_LINE), 0);
  lv_obj_add_flag(cl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(cl, LV_DIR_VER);
  lv_obj_set_flex_flow(cl, LV_FLEX_FLOW_COLUMN);
  convList = cl;
  chatEmpty = wrapLabel(cl, F14, C_DIM, "No chats yet. Join a channel in CHANNELS, or message someone from PEOPLE.");
  lv_obj_set_style_pad_all(chatEmpty, 14, 0);

  lv_obj_t *pane = mkBox(s, C_BG);
  lv_obj_set_pos(pane, 250, 0);
  lv_obj_set_size(pane, CW - 250, CH);
  paneTitle = mkLabel(pane, F18, C_TEXT, "");
  lv_obj_set_pos(paneTitle, 14, 8);
  lv_obj_set_width(paneTitle, CW - 250 - 180);
  oneLine(paneTitle);
  paneSub = mkLabel(pane, F12, C_DIM, "");
  lv_obj_set_pos(paneSub, 14, 32);
  lv_obj_set_width(paneSub, CW - 250 - 180);
  oneLine(paneSub);
  // Mute a channel: no alerts or sounds from it (the same list as the Chatter screen's Mute)
  muteBtn = mkBtn(pane, LV_SYMBOL_BELL "  Mute", [](lv_event_t *) {
    Conv *c = R() ? R()->conv(curConv) : nullptr;
    if (!c || !c->isChannel) return;
    bool m = !M->isMuted(c->title);
    M->setMuted(c->title, m);
    P->saveSettings();
    radioToast(true, m ? c->title + " muted: no alerts or sounds from it" : c->title + " unmuted");
    dirty[SEC_CHATS] = true;
    renderChats();
  }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_size(muteBtn, 110, 36);
  lv_obj_align(muteBtn, LV_ALIGN_TOP_RIGHT, -58, 8);
  lv_obj_t *hq = mkBtn(pane, "?", [](lv_event_t *) {
    Conv *c = R() ? R()->conv(curConv) : nullptr;
    if (c && c->isChannel) help("About channels", HELP_CHANNELS);
    else help("About routes and delivery", HELP_ROUTE);
  }, nullptr, C_BTN, C_ACC, F16);
  lv_obj_set_size(hq, 40, 36);
  lv_obj_align(hq, LV_ALIGN_TOP_RIGHT, -10, 8);
  msgList = mkBox(pane, C_BG);
  lv_obj_set_pos(msgList, 0, 52);
  lv_obj_set_size(msgList, CW - 250, CH - 52 - 96);
  lv_obj_add_flag(msgList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(msgList, LV_DIR_VER);
  lv_obj_set_flex_flow(msgList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(msgList, 10, 0);
  lv_obj_set_style_pad_row(msgList, 6, 0);
  lv_obj_set_style_border_side(msgList, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(msgList, 1, 0);
  lv_obj_set_style_border_color(msgList, lv_color_hex(C_LINE), 0);
  msgEmpty = wrapLabel(msgList, F14, C_DIM, "No messages yet. Say something!");
  lv_obj_set_style_pad_all(msgEmpty, 10, 0);
  // quick replies
  quickRow = mkBox(pane, C_BG);
  lv_obj_set_pos(quickRow, 0, CH - 94);
  lv_obj_set_size(quickRow, CW - 250, 40);
  lv_obj_add_flag(quickRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(quickRow, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(quickRow, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(quickRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(quickRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(quickRow, 10, 0);
  lv_obj_set_style_pad_column(quickRow, 6, 0);
  for (const char *q : QUICK) {
    lv_obj_t *b = mkBtn(quickRow, q, [](lv_event_t *e) {
      lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
      sendTo(curConv, lv_label_get_text(lv_obj_get_child(btn, 0)));
    }, nullptr, C_PANEL, C_TEXT, F14);
    lv_obj_set_height(b, 34);
    lv_obj_set_style_radius(b, 17, 0);
  }
  composeBtn = mkBtn(pane, LV_SYMBOL_KEYBOARD "   Write a message...", openCompose, nullptr, C_PANEL, C_DIM, F16);
  lv_obj_set_pos(composeBtn, 10, CH - 50);
  lv_obj_set_size(composeBtn, CW - 250 - 20, 44);
  lv_obj_align(lv_obj_get_child(composeBtn, 0), LV_ALIGN_LEFT_MID, 0, 0);
}
static ConvRow &convRow(size_t i) {
  while (convRows.size() <= i) {
    ConvRow r;
    r.box = mkBox(convList, T.listBg);
    lv_obj_set_size(r.box, 250, 62);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r.box, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_side(r.box, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r.box, 1, 0);
    lv_obj_set_style_border_color(r.box, lv_color_hex(T.listLine), 0);
    lv_obj_add_event_cb(r.box, [](lv_event_t *e) {
      size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
      if (R() && i < R()->convs.size()) selectConv(R()->convs[i].id);
    }, LV_EVENT_CLICKED, (void *)(intptr_t)convRows.size());
    r.av = mkAvatar(r.box, 38);
    lv_obj_set_pos(r.av, 10, 12);
    lv_obj_remove_flag(r.av, LV_OBJ_FLAG_CLICKABLE);
    r.title = mkLabel(r.box, F16, C_TEXT);
    lv_obj_set_pos(r.title, 58, 9);
    lv_obj_set_width(r.title, 130);
    oneLine(r.title);
    r.time = mkLabel(r.box, F12, C_DIM2);
    lv_obj_align(r.time, LV_ALIGN_TOP_RIGHT, -10, 11);
    r.last = mkLabel(r.box, F12, C_DIM);
    lv_obj_set_pos(r.last, 58, 34);
    lv_obj_set_width(r.last, 150);
    oneLine(r.last);
    r.badge = mkBox(r.box, C_AT);
    lv_obj_set_size(r.badge, LV_SIZE_CONTENT, 18);
    lv_obj_set_style_radius(r.badge, 9, 0);
    lv_obj_set_style_pad_hor(r.badge, 6, 0);
    lv_obj_align(r.badge, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_center(mkLabel(r.badge, F12, T.badgeText, "0"));
    convRows.push_back(r);
  }
  return convRows[i];
}
// ---------------------------------------------------------------- "heard by"
// Who a repeater is, from its path hash (1-3 bytes): your radio's contacts first, then CoreScope's map.
struct HopGuess {
  std::string name;  // "" = no repeater with this hash known
  int others = 0;    // other repeaters sharing the hash (so the name is a best guess)
  bool hasLoc = false;
  double lat = 0, lon = 0;
};
static std::vector<HopGuess> resolveHops(const std::vector<std::string> &hops);
// A repeater that passed something straight to you is in your range: of those sharing its short code,
// the nearest is the best guess ("~" marks a guess when others share the code).
static std::string hopName(const std::string &hop) {
  std::vector<HopGuess> g = resolveHops({hop});
  if (g.empty() || g[0].name.empty()) return "";
  return (g[0].others ? "~" : "") + g[0].name;
}
// Tapping your own message: what became of it. Send again where it didn't get through (not delivered,
// or no repeater heard passing it on), and Copy, which puts the text back in the message box to edit
// (the panel has no clipboard), like the MeshCore app's resend and copy.
static void showHeard(size_t idx) {
  Companion *r = R();
  Conv *c = r ? r->conv(curConv) : nullptr;
  if (!c || idx >= c->msgs.size() || !c->msgs[idx].mine) return;
  RMsg &m = c->msgs[idx];
  std::string convId = c->id, body = m.text;
  auto resend = [convId, body]() { sendTo(convId, body); };
  auto copy = [convId, body]() {
    selectConv(convId);
    openComposeWith(body);
  };
  if (m.status == MS_FAILED || !c->isChannel) {
    std::string title, text;
    bool retry = m.status == MS_FAILED;
    if (!c->isChannel && m.status == MS_FAILED) {
      title = "Not delivered";
      text = "No delivery receipt came back from " + c->title + ". They may be out of range or offline, or the receipt got lost on its way back. You can send it again.";
    } else if (m.status == MS_FAILED) {
      title = "Not sent";
      text = "Your radio didn't confirm sending this message. Check that it's connected, then send it again.";
    } else if (m.status == MS_DELIVERED) {
      title = "Delivered";
      text = c->title + "'s radio confirmed it arrived.";
    } else if (m.status == MS_SENT) {
      title = "Sent";
      text = "Waiting for " + c->title + "'s radio to confirm it arrived.";
    } else {
      title = "Sending...";
      text = "Your radio hasn't confirmed sending it yet.";
    }
    if (retry) modal(title, text, "Send again", resend, false, "Close");
    else modal(title, text, "Close", nullptr, false, nullptr);
    modalExtra("Copy", copy);
    return;
  }
  if (m.heard.empty()) {
    std::string text = "Your radio hasn't heard any repeater pass this message on";
    text += (P->nowMs() - m.ts) < 30000 ? " yet. Give it a few seconds." : ". It may not have reached one.";
    text += "\n\nRepeaters that hear your message send it on; your radio listens for that. You can send it again.";
    modal("Not heard yet", text, "Send again", resend, false, "Close");
    modalExtra("Copy", copy);
    return;
  }
  std::string text;
  for (auto &h : m.heard) {
    std::string name = hopName(h.hop);
    char line[200];
    snprintf(line, sizeof line, "%s  (%s)\n    %.1f dB SNR, %d dBm, %d %s\n", name.empty() ? "Unknown repeater" : name.c_str(), h.hop.c_str(), h.snr,
             h.rssi, h.hops, h.hops == 1 ? "hop" : "hops");
    text += line;
  }
  text += "\nThese are the repeaters your own radio heard passing it on. Others further away may have carried it too.";
  char title[64];
  snprintf(title, sizeof title, "Heard by %d repeater%s", (int)m.heard.size(), m.heard.size() == 1 ? "" : "s");
  modal(title, st::displayText(text), "Close", nullptr, false, nullptr);
  modalExtra("Copy", copy);
}

// ---------------------------------------------------------------- how someone else's message reached you
// Every copy your radio heard, each drawn as a chain: sender > repeaters > you.

// A path entry is just the first 1-3 bytes of a repeater's key, which several repeaters can share.
// As in meshcore-open's PathHopResolver: start from your own location, walk the path backwards and
// pick, for each hop, the matching repeater nearest the hop after it. Your radio's contacts and
// CoreScope's map (read from what was already downloaded) are both searched.
static std::vector<HopGuess> resolveHops(const std::vector<std::string> &hops) {
  std::vector<HopGuess> out(hops.size());
  Companion *r = R();
  LatLon here = myPlace();
  struct Cand {
    std::string key, name;
    bool loc;
    double lat, lon;
  };
  std::vector<Cand> cands;
  for (int i = (int)hops.size() - 1; i >= 0; i--) {
    const std::string &h = hops[i];
    if (h.empty()) continue;
    cands.clear();
    auto add = [&](const char *key, const std::string &name, bool loc, double lat, double lon) {
      for (auto &c : cands)
        if (!strcasecmp(c.key.c_str(), key)) return;
      cands.push_back({key, name, loc, lat, lon});
    };
    if (r)
      for (auto &c : r->contacts)
        if ((c.type == CT_REPEATER || c.type == CT_ROOM) && c.key.size() >= h.size() && strncasecmp(c.key.c_str(), h.c_str(), h.size()) == 0)
          add(c.key.c_str(), c.name.str(), c.hasLoc, c.lat, c.lon);
    for (auto &nd : M->nodes)
      if ((nd.role == ROLE_REPEATER || nd.role == ROLE_ROOM) && nd.key.size() >= h.size() && strncasecmp(nd.key.c_str(), h.c_str(), h.size()) == 0)
        add(nd.key.c_str(), nd.name.empty() ? std::string() : nd.name.str(), nd.hasLoc, nd.lat, nd.lon);
    if (cands.empty()) continue;
    int best = 0;
    double bestD = 1e18;
    for (int k = 0; k < (int)cands.size(); k++) {
      double d = cands[k].loc ? milesBetween(here, {cands[k].lat, cands[k].lon}) : 1e9 + k;  // no location: last resort
      if (d < bestD) {
        bestD = d;
        best = k;
      }
    }
    out[i].name = cands[best].name.empty() ? "unnamed" : cands[best].name;
    out[i].others = (int)cands.size() - 1;
    if (cands[best].loc) {
      here = {cands[best].lat, cands[best].lon};
      out[i].hasLoc = true;
      out[i].lat = here.lat;
      out[i].lon = here.lon;
    }
  }
  return out;
}

// Where a sender is: your radio's contact (by key prefix, or by name), else CoreScope's map
static PathStop stopFor(const std::string &name, const std::string &keyPrefix) {
  PathStop st;
  st.label = name;
  Companion *r = R();
  if (r)
    for (auto &c : r->contacts) {
      bool match = !keyPrefix.empty() ? c.key.size() >= keyPrefix.size() && strncasecmp(c.key.c_str(), keyPrefix.c_str(), keyPrefix.size()) == 0
                                      : !strcasecmp(c.name.c_str(), name.c_str());
      if (match && c.hasLoc) {
        st.hasLoc = true;
        st.lat = c.lat;
        st.lon = c.lon;
        return st;
      }
    }
  for (auto &nd : M->nodes) {
    bool match = !keyPrefix.empty() ? nd.key.size() >= keyPrefix.size() && strncasecmp(nd.key.c_str(), keyPrefix.c_str(), keyPrefix.size()) == 0
                                    : !nd.name.empty() && !strcasecmp(nd.name.c_str(), name.c_str());
    if (match && nd.hasLoc) {
      st.hasLoc = true;
      st.lat = nd.lat;
      st.lon = nd.lon;
      return st;
    }
  }
  return st;
}
static PathStop youStop() {
  PathStop st;
  st.label = "You";
  LatLon me = myPlace();
  st.hasLoc = true;
  st.lat = me.lat;
  st.lon = me.lon;
  return st;
}
// sender > hops > you, as map stops (hops resolved the same way as the pills)
static std::vector<PathStop> chainStops(const PathStop &from, const std::vector<std::string> &hops) {
  std::vector<PathStop> out{from};
  std::vector<HopGuess> g = resolveHops(hops);
  for (size_t i = 0; i < hops.size(); i++) {
    PathStop st;
    st.label = g[i].name.empty() ? "Repeater " + hops[i] : (g[i].others ? "~" : "") + g[i].name;
    st.hasLoc = g[i].hasLoc;
    st.lat = g[i].lat;
    st.lon = g[i].lon;
    out.push_back(st);
  }
  out.push_back(youStop());
  return out;
}
static std::vector<std::vector<PathStop>> rtStops;  // per route card: what its Map button pins
static std::vector<std::string> rtTitles;
static int rtMode = 0;                              // 0 a message's route, 1 "Who can hear me?"
static void routeMapBtn(lv_obj_t *parent, size_t idx) {
  lv_obj_t *b = mkBtn(parent, LV_SYMBOL_GPS "  Map", [](lv_event_t *e) {
    size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i >= rtStops.size()) return;
    hide(rtBack, true);
    showScreen(S_MAP);
    mapShowPath(rtTitles[i], rtStops[i], false);
  }, (void *)(intptr_t)idx, C_BTN, C_TEXT, F12);
  lv_obj_set_height(b, 30);
}

static lv_obj_t *routePill(lv_obj_t *row, const std::string &text, uint32_t col, bool strong) {
  lv_obj_t *p = mkBox(row, col, strong ? LV_OPA_COVER : 46);
  lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(p, 14, 0);
  lv_obj_set_style_pad_hor(p, 11, 0);
  lv_obj_set_style_pad_ver(p, 5, 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_border_color(p, lv_color_hex(col), 0);
  std::string t = st::displayText(text);
  if (t.size() > 30) {  // keep a pill to one line (cut on a character boundary)
    size_t cut = 28;
    while (cut > 0 && ((unsigned char)t[cut] & 0xC0) == 0x80) cut--;
    t = t.substr(0, cut) + "...";
  }
  mkLabel(p, F14, strong ? 0x0A0E13 : C_TEXT, t.c_str());
  return p;
}
static void routeArrow(lv_obj_t *row) { mkLabel(row, F14, C_DIM2, LV_SYMBOL_RIGHT); }
static lv_obj_t *routeCard() {
  lv_obj_t *c = mkCard(rtList, 600);
  lv_obj_set_style_bg_color(c, lv_color_hex(T.listBg), 0);
  lv_obj_set_style_pad_all(c, 12, 0);
  return c;
}
static lv_obj_t *pillRow(lv_obj_t *card) {
  lv_obj_t *row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_set_style_pad_row(row, 6, 0);
  return row;
}
// one chain: sender > hops (sender's side first) > you
static void drawChain(lv_obj_t *card, const std::string &sender, const std::vector<std::string> &hops, bool guessed) {
  static constexpr uint32_t RPT = 0xFFB547;
  lv_obj_t *row = pillRow(card);
  routePill(row, sender, avatarColor(sender), true);
  std::vector<HopGuess> g = resolveHops(hops);
  for (size_t i = 0; i < hops.size(); i++) {
    routeArrow(row);
    std::string t = g[i].name.empty() ? "Repeater " + hops[i] : (g[i].others || guessed ? "~" : "") + g[i].name + "  " + hops[i];
    routePill(row, t, RPT, false);
  }
  routeArrow(row);
  routePill(row, "You", C_ACC, true);
}

static void showRoute(size_t idx) {
  Companion *r = R();
  Conv *c = r ? r->conv(curConv) : nullptr;
  if (!c || idx >= c->msgs.size() || c->msgs[idx].mine) return;
  RMsg &m = c->msgs[idx];
  std::string sender = m.who.empty() ? (c->isChannel ? std::string("Sender") : c->title) : m.who;
  rtMode = 0;
  hide(rtMapBtn, true);
  rtStops.clear();
  rtTitles.clear();
  PathStop from = stopFor(sender, c->isChannel ? std::string() : c->key.substr(0, 12));
  lv_obj_clean(rtList);
  std::vector<const RxPath *> ps;
  for (auto &p : m.paths) ps.push_back(&p);
  std::stable_sort(ps.begin(), ps.end(), [](const RxPath *a, const RxPath *b) {
    if (a->direct != b->direct) return !a->direct;
    if (a->hops.size() != b->hops.size()) return a->hops.size() < b->hops.size();
    return a->snr > b->snr;
  });
  std::string sub = sender + "  |  " + c->title + "  |  " + agoLong(m.ts);
  if (ps.empty()) {
    lv_label_set_text(rtTitle, "Route not recorded");
    lv_obj_t *card = routeCard();
    std::string t = "Your radio reports the route only for messages it hears while the panel is connected. This one arrived "
                    "while the panel was away (the radio kept it for you), or before it connected.";
    if (m.hops > 0) t += "\n\nThe radio says it came through " + std::to_string(m.hops) + (m.hops == 1 ? " repeater." : " repeaters.");
    else if (m.hops == 0) t += "\n\nThe radio says it heard the sender directly, with no repeaters in between.";
    else if (!c->isChannel) t += "\n\nIt came along a set route (direct).";
    wrapLabel(card, F16, C_DIM, st::displayText(t));
  } else {
    if (ps.size() == 1) lv_label_set_text(rtTitle, "How this reached you");
    else lv_label_set_text_fmt(rtTitle, "How this reached you: heard %d times", (int)ps.size());
    if (ps.size() > 1) sub += "  |  each card is one copy";
    int n = 0;
    for (const RxPath *p : ps) {
      lv_obj_t *card = routeCard();
      lv_obj_t *head = mkRow(card);
      lv_obj_set_width(head, lv_pct(100));
      lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      std::string what = p->direct ? "Along a set route" : p->hops.empty() ? "Straight from the sender"
                         : std::to_string(p->hops.size()) + (p->hops.size() == 1 ? " repeater" : " repeaters");
      if (ps.size() > 1) what = "Copy " + std::to_string(++n) + "  |  " + what;
      mkLabel(head, F14, C_TEXT, what.c_str());
      lv_obj_t *sig = mkRow(head);
      lv_obj_set_width(sig, LV_SIZE_CONTENT);
      lv_obj_set_style_pad_column(sig, 6, 0);
      setBars(mkBars(sig), snrBars(p->snr));
      char b[48];
      snprintf(b, sizeof b, "%+.1f dB  %d dBm", p->snr, p->rssi);
      mkLabel(sig, F12, C_DIM, b);
      std::vector<std::string> mapHops = p->hops;
      if (p->direct) {  // the probable way it came: your saved route to them, reversed
        mapHops = c->isChannel ? std::vector<std::string>() : r->savedRoute(c->key);
        std::reverse(mapHops.begin(), mapHops.end());
      }
      rtStops.push_back(chainStops(from, mapHops));
      rtTitles.push_back("Route: " + sender + " to you" + (mapHops.empty() ? "" : " via " + std::to_string(mapHops.size()) +
                                                                                 (mapHops.size() == 1 ? " repeater" : " repeaters")));
      routeMapBtn(sig, rtStops.size() - 1);
      if (p->direct) {
        // Repeaters on a set route remove themselves as they pass it on, so the packet arrives without
        // them. Replies usually take your saved route to that person, reversed.
        std::vector<std::string> back;
        if (!c->isChannel) back = r->savedRoute(c->key);
        std::reverse(back.begin(), back.end());
        if (!back.empty()) {
          drawChain(card, sender, back, true);
          wrapLabel(card, F12, C_DIM2, "Probably this way: your radio's saved route to them, reversed. Repeaters on a set route don't leave their mark on the message.");
        } else {
          drawChain(card, sender, {}, false);
          wrapLabel(card, F12, C_DIM2, "Repeaters on a set route don't leave their mark on the message, so your radio can't see which ones carried it.");
        }
      } else {
        drawChain(card, sender, p->hops, false);
        if (p->hops.empty()) wrapLabel(card, F12, C_DIM2, "Your radio heard the sender directly, no repeaters in between.");
      }
    }
    lv_obj_t *foot = wrapLabel(rtList, F12, C_DIM2,
                               "Repeaters are known by the first byte(s) of their key. A ~ marks a best guess: several repeaters share "
                               "that ID, and the one nearest the next hop is shown.");
    lv_obj_set_width(foot, 600);
  }
  lv_label_set_text(rtSub, st::displayText(sub).c_str());
  lv_obj_scroll_to_y(rtList, 0, LV_ANIM_OFF);
  hide(rtBack, false);
  lv_obj_move_foreground(rtBack);
}
// ---------------------------------------------------------------- "Who can hear me?"
static const char *nodeTypeName(uint8_t t) { return t == 2 ? "Repeater" : t == 3 ? "Room server" : t == 4 ? "Sensor" : t == 1 ? "Companion" : "Node"; }
static std::string neighbourName(const Neighbour &nb) {
  Companion *r = R();
  if (r)
    if (RContact *c = r->contactByPrefix(nb.key.substr(0, 12))) return c->name.str();
  for (auto &nd : M->nodes)
    if (!nd.name.empty() && nd.key.size() >= 16 && strncasecmp(nd.key.c_str(), nb.key.c_str(), 16) == 0) return nd.name.str();
  return "";
}
static size_t discShown = (size_t)-1;
static bool discWasRunning = false;
static void renderDiscovery(bool force) {
  Companion *r = R();
  if (!r) return;
  DiscoverRun &dv = r->discovery;
  std::string sub;
  if (dv.running) sub = "Listening for answers...  " + std::to_string(std::max<int64_t>(0, (dv.until - P->nowMs() + 999) / 1000)) + " s";
  else if (dv.failed) sub = "Couldn't ask";
  else sub = dv.found.empty() ? "No answers" : std::to_string(dv.found.size()) + (dv.found.size() == 1 ? " radio heard you" : " radios heard you");
  lv_label_set_text(rtSub, sub.c_str());
  if (!force && dv.found.size() == discShown && dv.running == discWasRunning) return;
  discShown = dv.found.size();
  discWasRunning = dv.running;
  lv_obj_clean(rtList);
  hide(rtMapBtn, dv.found.empty());
  if (dv.found.empty()) {
    lv_obj_t *card = routeCard();
    wrapLabel(card, F16, C_DIM,
              dv.failed ? dv.note
              : dv.running ? "Your radio asked every repeater and sensor in direct range to answer. Answers trickle in over about 20 seconds: "
                             "each waits a random moment so they don't talk over each other."
                           : "Nobody answered. No repeater may be in direct range, or the antenna or transmit power is holding your radio back. "
                             "Repeaters set to stay hidden never answer.");
    return;
  }
  std::vector<const Neighbour *> v;
  for (auto &nb : dv.found) v.push_back(&nb);
  std::sort(v.begin(), v.end(), [](const Neighbour *a, const Neighbour *b) { return a->theyHear > b->theyHear; });
  LatLon me = myPlace();
  for (const Neighbour *nb : v) {
    lv_obj_t *card = routeCard();
    lv_obj_t *head = mkRow(card);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    std::string nm = neighbourName(*nb);
    if (nm.empty()) nm = std::string("Unknown ") + nodeTypeName(nb->type) + " " + nb->key.substr(0, 4);
    lv_obj_t *nl = mkLabel(head, F16, C_TEXT, st::displayText(nm).c_str());
    lv_obj_set_width(nl, 330);
    oneLine(nl);
    lv_obj_t *sig = mkRow(head);
    lv_obj_set_width(sig, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(sig, 6, 0);
    mkLabel(sig, F12, C_DIM, nodeTypeName(nb->type));
    setBars(mkBars(sig), snrBars(nb->theyHear));
    char b[160];
    snprintf(b, sizeof b, "They hear you  %+.1f dB      You hear them  %+.1f dB  (%d dBm)", nb->theyHear, nb->weHear, nb->rssi);
    std::string line = b;
    PathStop where = stopFor(nm, nb->key.substr(0, 16));
    if (where.hasLoc) {
      char d[48];
      snprintf(d, sizeof d, "      %.1f mi away", milesBetween(me, {where.lat, where.lon}));
      line += d;
    }
    wrapLabel(card, F12, C_DIM, line);
  }
  lv_obj_t *foot = wrapLabel(rtList, F12, C_DIM2,
                             "Only repeaters, room servers and sensors answer; phones and companion radios stay quiet. "
                             "\"They hear you\" is how well each one heard your radio: the number that matters for being heard.");
  lv_obj_set_width(foot, 600);
}
static void startDiscovery() {
  Companion *r = R();
  if (!r) return;
  r->discover();
  rtMode = 1;
  lv_label_set_text(rtTitle, "Who can hear me?");
  discShown = (size_t)-1;
  renderDiscovery(true);
  lv_obj_scroll_to_y(rtList, 0, LV_ANIM_OFF);
  hide(rtBack, false);
  lv_obj_move_foreground(rtBack);
}
static void discoveryOnMap() {
  Companion *r = R();
  if (!r || r->discovery.found.empty()) return;
  std::vector<PathStop> stops{youStop()};
  for (auto &nb : r->discovery.found) {
    std::string nm = neighbourName(nb);
    PathStop st = stopFor(nm.empty() ? nb.key.substr(0, 8) : nm, nb.key.substr(0, 16));
    char b[24];
    snprintf(b, sizeof b, "  %+.1f dB", nb.theyHear);
    st.label += b;
    stops.push_back(st);
  }
  hide(rtBack, true);
  showScreen(S_MAP);
  mapShowPath("Radios that heard you: " + std::to_string(r->discovery.found.size()), stops, true);
}

static void onMsgTap(size_t idx) {
  Companion *r = R();
  Conv *c = r ? r->conv(curConv) : nullptr;
  if (!c || idx >= c->msgs.size()) return;
  if (c->msgs[idx].mine) showHeard(idx);
  else showRoute(idx);
}

// ---------------------------------------------------------------- trace route
// Names for a trace's hops. The repeater you traced is named from your pick (its short code may be
// shared with others); the hops before it are resolved from your end outward, nearest match first;
// the way back mirrors the way out.
static std::vector<std::string> traceNames(const std::vector<std::string> &route, int turnAt, const std::string &targetKey) {
  std::vector<std::string> names(route.size(), "Repeater");
  if (route.empty()) return names;
  turnAt = std::max(0, std::min(turnAt, (int)route.size() - 1));
  std::vector<std::string> rev(route.begin(), route.begin() + turnAt + 1);
  std::reverse(rev.begin(), rev.end());  // resolveHops starts at your end
  std::vector<HopGuess> g = resolveHops(rev);
  for (int k = 0; k <= turnAt; k++) {
    const HopGuess &h = g[turnAt - k];
    if (!h.name.empty()) names[k] = (h.others ? "~" : "") + h.name;
  }
  Companion *r = R();
  RContact *tc = r ? r->contactByPrefix(targetKey.substr(0, 12)) : nullptr;
  if (tc) names[turnAt] = tc->name.str();
  for (size_t i = turnAt + 1; i < route.size(); i++) {
    int m = 2 * turnAt - (int)i;
    if (m >= 0) names[i] = names[m];
  }
  return names;
}
// Other repeaters known to use the same short code as the one you traced
static std::vector<std::string> sharingCode(const std::string &code, const std::string &targetKey) {
  std::vector<std::string> names, keys;
  auto add = [&](const std::string &key, const std::string &name) {
    if (code.empty() || key.size() < 12 || strncasecmp(key.c_str(), code.c_str(), code.size()) != 0) return;
    if (targetKey.size() >= 12 && strncasecmp(key.c_str(), targetKey.c_str(), 12) == 0) return;  // the one you picked
    for (auto &k : keys)
      if (strncasecmp(k.c_str(), key.c_str(), 12) == 0) return;
    keys.push_back(key.substr(0, 12));
    names.push_back(name.empty() ? std::string("an unnamed repeater") : name);
  };
  if (Companion *r = R())
    for (auto &c : r->contacts)
      if (c.type == CT_REPEATER || c.type == CT_ROOM) add(c.key.str(), c.name.str());
  for (auto &nd : M->nodes)
    if (nd.role == ROLE_REPEATER || nd.role == ROLE_ROOM) add(nd.key.str(), nd.name.empty() ? std::string() : nd.name.str());
  return names;
}
static std::string traceText() {
  Companion *r = R();
  if (!r) return "";
  TraceRun &t = r->trace;
  if (t.route.empty()) return t.note;  // it couldn't start
  std::vector<std::string> names = traceNames(t.route, t.turnAt, t.target);
  auto label = [&](size_t i) { return names[i] + " (" + t.route[i] + ")"; };
  std::string targetName = names[std::min((size_t)t.turnAt, names.size() - 1)];
  std::string code = t.route[std::min((size_t)t.turnAt, t.route.size() - 1)];
  std::vector<std::string> sharing = sharingCode(code, t.target);
  std::string shareNote;
  if (!sharing.empty())
    shareNote = "Its short code " + code + " is also used by " + sharing[0] +
                (sharing.size() > 1 ? " and " + std::to_string(sharing.size() - 1) + " more" : "") +
                ". Repeaters are picked by that code alone, so whichever of them hears the trace passes it on, and the one near you answers.";
  // how far a try got, from what your radio heard on air
  auto howFar = [](const std::vector<std::string> &route, int heard, const std::vector<std::string> &nm) {
    if (heard <= 0) return std::string("your radio didn't hear any repeater pass it on.");
    return "it got as far as " + nm[heard - 1] + " (" + std::to_string(heard) + " of " + std::to_string(route.size()) + " hops), then nothing came back.";
  };
  std::string out;
  if (t.waiting) {
    if (t.secondTry) {
      std::vector<std::string> fn = traceNames(t.first.route, t.first.turnAt, t.target);
      out = t.retriedDirect ? "Your saved route didn't answer" : "Straight to it didn't answer";
      if (t.first.heardHops > 0) out += " (it got as far as " + fn[t.first.heardHops - 1] + ")";
      out += t.retriedDirect ? ", so now trying straight to it and back.\n\n" : ", so now trying along your saved route.\n\n";
    } else if (t.viaSaved)
      out = "Sending a trace along your saved route, " + std::to_string(t.route.size()) + " hops there and back. Each repeater it passes notes how well it heard the last one.\n\n";
    else out = "Sending a trace straight to it and back. It notes how well it heard you, and your radio how well it heard it back.\n\n";
    out += "You";
    for (size_t i = 0; i < t.route.size(); i++) out += "  >  " + label(i);
    out += "  >  You\n\n";
    if (!shareNote.empty()) out += shareNote + "\n\n";
    if (t.heardHops > 0)
      out += "Your radio heard " + names[t.heardHops - 1] + " pass it on (" + std::to_string(t.heardHops) + " of " + std::to_string(t.route.size()) + ").\n";
    out += "Waiting for it to come back...  " + std::to_string((int)((P->nowMs() - t.started) / 1000)) + " s";
    return out;
  }
  if (t.failed) {
    if (t.secondTry) {
      std::vector<std::string> fn = traceNames(t.first.route, t.first.turnAt, t.target);
      out = std::string(t.first.viaSaved ? "Along your saved route: " : "Straight to it: ") + howFar(t.first.route, t.first.heardHops, fn) + "\n";
    }
    out += std::string(t.viaSaved ? "Along your saved route: " : "Straight to it: ") + howFar(t.route, t.heardHops, names) + "\n\n";
    bool directSilent = (!t.viaSaved && t.heardHops == 0) || (t.secondTry && !t.first.viaSaved && t.first.heardHops == 0);
    if (directSilent)
      out += targetName + " didn't pass the trace on where your radio could hear it. It may be off, out of range, or set not to repeat.";
    else out += "It may be out of range, offline, or set not to repeat.";
    int64_t seen = r->lastFirstHand(t.target);
    if (seen > 0) {
      int mins = (int)((P->nowMs() - seen) / 60000);
      out += "\n\nYour radio last heard a repeater with its code pass something straight to you " +
             (mins < 1 ? std::string("under a minute ago") : std::to_string(mins) + (mins == 1 ? " minute ago" : " minutes ago")) + ".";
    }
    if (!shareNote.empty()) out += "\n\n" + shareNote;
    if (t.viaSaved || t.secondTry) out += "\n\nTip: Forget route and wait for a fresh advert, then try again.";
    return out;
  }
  // SNR heard at each hop from the previous one, then by your radio from the last hop
  std::string prev = "You";
  for (size_t i = 0; i < t.route.size(); i++) {
    char b[48];
    snprintf(b, sizeof b, "   %+.1f dB\n", i < t.snrs.size() ? t.snrs[i] : 0.0f);
    out += prev + "  >  " + label(i) + b;
    prev = label(i);
  }
  char b[64];
  snprintf(b, sizeof b, "  >  You   %+.1f dB\n", t.finalSnr);
  out += prev + b;
  char tm[80];
  snprintf(tm, sizeof tm, "\nRound trip: %.1f s.", (P->nowMs() - t.started) / 1000.0);
  out += tm;
  out += " Higher is better. A hop within a few dB of your settings' limit (about -7.5 dB at SF7, -12.5 at SF9, -17.5 at SF11) is close to dropping.";
  return out;
}
static void refreshTrace() {
  if (!traceOpen) return;
  if (lv_obj_has_flag(modalBack, LV_OBJ_FLAG_HIDDEN)) {
    traceOpen = false;
    return;
  }
  Companion *r = R();
  lv_label_set_text(modalTitle, r && r->trace.waiting ? "Tracing the route..." : r && r->trace.failed ? "Trace didn't come back" : "Route trace");
  lv_label_set_text(modalText, st::displayText(traceText()).c_str());
  if (r && r->trace.done && !r->trace.failed && !modalCb) {  // finished: offer the map
    modalCb = [] {
      Companion *c = R();
      if (!c) return;
      TraceRun &t = c->trace;
      // the way out: you > the hops > the repeater you traced (the way back mirrors it)
      std::vector<std::string> out(t.route.begin(), t.route.begin() + std::min((size_t)t.turnAt + 1, t.route.size()));
      std::vector<std::string> rev(out.rbegin(), out.rend());
      std::vector<PathStop> stops = chainStops(youStop(), rev);  // resolved from your end
      std::reverse(stops.begin(), stops.end());
      stops.front().label = "You";
      stops.pop_back();  // chainStops added you at both ends
      RContact *tc = c->contactByPrefix(t.target.substr(0, 12));
      PathStop tgt = stopFor(tc ? tc->name.str() : t.target.substr(0, 8), t.target.substr(0, 12));
      if (!stops.empty() && stops.size() > 1) stops.back() = tgt;
      else stops.push_back(tgt);
      showScreen(S_MAP);
      mapShowPath("Trace: you to " + tgt.label + " (" + std::to_string(out.size()) + (out.size() == 1 ? " hop)" : " hops)"), stops, false);
    };
    setBtnText(modalOk, LV_SYMBOL_GPS "  Show on map");
    hide(modalCancel, false);
    setBtnText(modalCancel, "Close");
  }
}
static void startTrace(const std::string &key) {
  Companion *r = R();
  if (!r) return;
  r->traceRoute(key);
  modal("Tracing the route...", traceText(), "Close", nullptr, false, nullptr);
  traceOpen = true;
  refreshTrace();
}

static MsgRow &msgRow(size_t i) {
  while (msgRows.size() <= i) {
    MsgRow r;
    r.wrap = lv_obj_create(msgList);
    lv_obj_remove_style_all(r.wrap);
    lv_obj_set_size(r.wrap, lv_pct(100), LV_SIZE_CONTENT);
    r.box = mkBox(r.wrap, C_PANEL);
    lv_obj_set_size(r.box, 330, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(r.box, 14, 0);
    lv_obj_set_style_pad_all(r.box, 9, 0);
    lv_obj_set_style_pad_row(r.box, 2, 0);
    lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_border_width(r.box, 1, 0);
    r.who = mkLabel(r.box, F12, C_ACC);
    r.text = mkLabel(r.box, F16, C_TEXT);
    lv_obj_set_width(r.text, lv_pct(100));
    lv_label_set_long_mode(r.text, LV_LABEL_LONG_WRAP);
    lv_obj_t *mr = lv_obj_create(r.box);
    lv_obj_remove_style_all(mr);
    lv_obj_set_size(mr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mr, 6, 0);
    r.bars = mkBars(mr);
    r.meta = mkLabel(mr, F12, C_DIM2);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r.box, [](lv_event_t *e) { onMsgTap((size_t)(intptr_t)lv_event_get_user_data(e)); }, LV_EVENT_CLICKED,
                        (void *)(intptr_t)i);
    msgRows.push_back(r);
  }
  return msgRows[i];
}
static void renderChats() {
  Companion *r = R();
  size_t n = r ? r->convs.size() : 0;
  if (r && (curConv.empty() || !r->conv(curConv)) && n) curConv = r->convs[0].id;
  for (size_t i = 0; i < n; i++) {
    Conv &c = r->convs[i];
    ConvRow &w = convRow(i);
    bool sel = c.id == curConv;
    lv_obj_set_style_bg_color(w.box, lv_color_hex(sel ? T.sel : T.listBg), 0);
    setAvatar(w.av, c.title, c.isChannel ? CT_NONE : (r->contactByPrefix(c.key.substr(0, 12)) ? r->contactByPrefix(c.key.substr(0, 12))->type : CT_CHAT));
    if (c.isChannel) {
      lv_obj_set_style_bg_color(w.av, lv_color_hex(gcol(G_CHANNEL)), 0);
      lv_label_set_text(lv_obj_get_child(w.av, 0), c.title.size() > 1 && c.title[0] == '#' ? "#" : LV_SYMBOL_WIFI);
    }
    bool cmuted = c.isChannel && M->isMuted(c.title);
    lv_label_set_text(w.title, cmuted ? (LV_SYMBOL_MUTE "  " + c.title).c_str() : c.title.c_str());
    lv_obj_set_style_text_color(w.title, lv_color_hex(cmuted ? C_DIM : C_TEXT), 0);
    lv_obj_set_style_bg_color(w.badge, lv_color_hex(cmuted ? C_DIM2 : C_ACC), 0);
    std::string last = c.msgs.empty() ? (c.isChannel ? "Group channel" : "Private chat")
                                      : (c.msgs.back().mine ? "You: " : (c.isChannel && !c.msgs.back().who.empty() ? c.msgs.back().who + ": " : "")) +
                                            c.msgs.back().text;
    lv_label_set_text(w.last, st::displayText(last).c_str());
    lv_label_set_text(w.time, c.msgs.empty() ? "" : ago(c.msgs.back().ts).c_str());
    if (c.unread > 0 && !sel) {
      lv_label_set_text_fmt(lv_obj_get_child(w.badge, 0), "%d", std::min(c.unread, 99));
      hide(w.badge, false);
    } else hide(w.badge, true);
    hide(w.box, false);
  }
  for (size_t i = n; i < convRows.size(); i++) hide(convRows[i].box, true);
  hide(chatEmpty, n > 0);

  Conv *c = r ? r->conv(curConv) : nullptr;
  size_t m = 0;
  if (c) {
    c->unread = 0;
    lv_label_set_text(paneTitle, c->title.c_str());
    if (c->isChannel) {
      RChannel *ch = r->channelByIdx(c->chIdx);
      bool cm = M->isMuted(c->title);
      lv_label_set_text(paneSub, cm ? "Muted: no alerts or sounds from this channel"
                                 : ch && ch->kind == CK_PRIVATE ? "Private channel: only people with the key can read it"
                                                                : "Group channel: anyone on it can read along");
      setBtnText(muteBtn, cm ? LV_SYMBOL_MUTE "  Muted" : LV_SYMBOL_BELL "  Mute");
      setBtnColors(muteBtn, cm ? 0xFFB547 : C_BTN, cm ? 0x0A0E13 : C_TEXT);
    } else {
      RContact *rc = r->contactByPrefix(c->key.substr(0, 12));
      std::string sub = std::string("Private message  |  ") + (rc ? routeText(rc->pathLen) : "not in your People list");
      lv_label_set_text(paneSub, sub.c_str());
    }
    for (auto &msg : c->msgs) {
      MsgRow &w = msgRow(m++);
      lv_obj_set_style_bg_color(w.box, lv_color_hex(msg.mine ? T.chipOn : C_PANEL), 0);
      lv_obj_set_style_border_color(w.box, lv_color_hex(msg.mine ? T.fresh : C_LINE), 0);
      lv_obj_align(w.box, msg.mine ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 0);
      lv_label_set_text(w.who, msg.mine ? "You" : (msg.who.empty() ? "?" : msg.who.c_str()));
      lv_obj_set_style_text_color(w.who, lv_color_hex(msg.mine ? C_DIM : avatarColor(msg.who)), 0);
      lv_label_set_text(w.text, st::displayText(msg.text).c_str());
      std::string meta = ago(msg.ts);
      if (!msg.mine) {
        if (msg.hops >= 0) meta += msg.hops == 0 ? "  |  direct" : "  |  " + std::to_string(msg.hops) + (msg.hops == 1 ? " hop" : " hops");
        if (msg.paths.size() > 1) meta += "  |  heard " + std::to_string(msg.paths.size()) + "x";
        if (!msg.paths.empty()) meta += "  " LV_SYMBOL_RIGHT;
        hide(w.bars, !msg.hasSnr);
        if (msg.hasSnr) setBars(w.bars, snrBars(msg.snr));
      } else {
        hide(w.bars, true);
        static const char *stTxt[] = {"", "sending...", "sent", "sent, waiting for reply", LV_SYMBOL_OK " delivered", LV_SYMBOL_WARNING " not delivered"};
        int idx = msg.status == MS_SENT ? (c->isChannel ? 2 : 3) : msg.status == MS_DELIVERED ? 4 : msg.status == MS_FAILED ? 5 : msg.status;
        meta += std::string("  |  ") + stTxt[idx];
        if (c->isChannel && msg.status != MS_SENDING && msg.status != MS_FAILED) {
          if (!msg.heard.empty()) meta += "  |  heard by " + std::to_string(msg.heard.size()) + (msg.heard.size() == 1 ? " repeater" : " repeaters");
          else if (P->nowMs() - msg.ts > 30000) meta += "  |  " LV_SYMBOL_WARNING " no repeats heard";
        }
      }
      lv_label_set_text(w.meta, meta.c_str());
      hide(w.wrap, false);
    }
  } else {
    lv_label_set_text(paneTitle, "Chats");
    lv_label_set_text(paneSub, "");
  }
  for (size_t i = m; i < msgRows.size(); i++) hide(msgRows[i].wrap, true);
  hide(msgEmpty, m > 0 || !c);
  hide(composeBtn, !c);
  hide(quickRow, !c);
  hide(muteBtn, !c || !c->isChannel);
  lv_obj_update_layout(msgList);
  lv_obj_scroll_to_y(msgList, LV_COORD_MAX, LV_ANIM_OFF);
}

// ================================================================ PEOPLE
static lv_obj_t *pplList, *pplEmpty, *pplCount, *pplChips[5], *detail, *detailBody;
static int pplFilter = 0;  // 0 all, 1 people, 2 repeaters, 3 rooms, 4 favorites
static std::string selKey;
struct PRow {
  lv_obj_t *box, *av, *name, *sub, *star;
};
static std::vector<PRow> pRows;
static std::vector<std::string> pKeys;

static bool pplMatch(const RContact &c) {
  switch (pplFilter) {
    case 1: return c.type == CT_CHAT;
    case 2: return c.type == CT_REPEATER;
    case 3: return c.type == CT_ROOM;
    case 4: return c.fav();
    default: return true;
  }
}
static void buildPeople(lv_obj_t *s) {
  lv_obj_t *bar = mkBox(s, C_BG);
  lv_obj_set_size(bar, CW, 50);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(bar, 10, 0);
  lv_obj_set_style_pad_column(bar, 6, 0);
  static const char *names[] = {"All", "People", "Repeaters", "Rooms", LV_SYMBOL_OK " Favorites"};
  for (int i = 0; i < 5; i++) {
    pplChips[i] = mkBtn(bar, names[i], [](lv_event_t *e) {
      pplFilter = (int)(intptr_t)lv_event_get_user_data(e);
      dirty[SEC_PEOPLE] = true;
    }, (void *)(intptr_t)i, C_PANEL, C_TEXT, F14);
    lv_obj_set_height(pplChips[i], 36);
    lv_obj_set_style_radius(pplChips[i], 18, 0);
  }
  pplCount = mkLabel(bar, F12, C_DIM, "");
  lv_obj_set_flex_grow(pplCount, 1);
  lv_obj_set_style_text_align(pplCount, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_t *hq = mkBtn(bar, "?", [](lv_event_t *) { help("Why don't I see anyone?", HELP_NOBODY); }, nullptr, C_BTN, C_ACC, F16);
  lv_obj_set_size(hq, 40, 36);

  pplList = mkBox(s, T.listBg);
  lv_obj_set_pos(pplList, 0, 50);
  lv_obj_set_size(pplList, 330, CH - 50);
  lv_obj_add_flag(pplList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(pplList, LV_DIR_VER);
  lv_obj_set_flex_flow(pplList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_border_side(pplList, (lv_border_side_t)(LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_TOP), 0);
  lv_obj_set_style_border_width(pplList, 1, 0);
  lv_obj_set_style_border_color(pplList, lv_color_hex(C_LINE), 0);
  pplEmpty = wrapLabel(pplList, F14, C_DIM, "");
  lv_obj_set_style_pad_all(pplEmpty, 14, 0);

  detail = mkBox(s, C_BG);
  lv_obj_set_pos(detail, 330, 50);
  lv_obj_set_size(detail, CW - 330, CH - 50);
  lv_obj_add_flag(detail, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(detail, LV_DIR_VER);
  lv_obj_set_style_pad_all(detail, 12, 0);
  lv_obj_set_style_border_side(detail, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(detail, 1, 0);
  lv_obj_set_style_border_color(detail, lv_color_hex(C_LINE), 0);
  detailBody = lv_obj_create(detail);
  lv_obj_remove_style_all(detailBody);
  lv_obj_set_size(detailBody, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(detailBody, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(detailBody, 8, 0);
}
static PRow &pRow(size_t i) {
  while (pRows.size() <= i) {
    PRow r;
    r.box = mkBox(pplList, T.listBg);
    lv_obj_set_size(r.box, 330, 58);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(r.box, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
    lv_obj_set_style_border_side(r.box, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r.box, 1, 0);
    lv_obj_set_style_border_color(r.box, lv_color_hex(T.listLine), 0);
    lv_obj_add_event_cb(r.box, [](lv_event_t *e) {
      size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
      if (i < pKeys.size()) selKey = pKeys[i];
      dirty[SEC_PEOPLE] = true;
    }, LV_EVENT_CLICKED, (void *)(intptr_t)pRows.size());
    r.av = mkAvatar(r.box, 38);
    lv_obj_set_pos(r.av, 10, 10);
    lv_obj_remove_flag(r.av, LV_OBJ_FLAG_CLICKABLE);
    r.name = mkLabel(r.box, F16, C_TEXT);
    lv_obj_set_pos(r.name, 58, 8);
    lv_obj_set_width(r.name, 230);
    oneLine(r.name);
    r.sub = mkLabel(r.box, F12, C_DIM);
    lv_obj_set_pos(r.sub, 58, 32);
    lv_obj_set_width(r.sub, 260);
    oneLine(r.sub);
    r.star = mkLabel(r.box, F14, C_STAR, LV_SYMBOL_OK);
    lv_obj_align(r.star, LV_ALIGN_TOP_RIGHT, -10, 10);
    pRows.push_back(r);
  }
  return pRows[i];
}
static lv_obj_t *actBtn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, uint32_t fg = 0, uint32_t bg = 0) {
  lv_obj_t *b = mkBtn(parent, txt, cb, nullptr, bg ? bg : C_BTN, fg ? fg : C_TEXT, F14);
  lv_obj_set_size(b, 168, 44);
  return b;
}
static void loginPrompt(const std::string &key) {
  RContact *c = R() ? R()->contactByPrefix(key) : nullptr;
  if (!c) return;
  std::string k = key;
  prompt("Log in to " + c->name, "Leave empty to log in as a guest. Owners use the admin password.", "", 15, "Log in", [k](const std::string &pw) {
    if (R()) R()->login(k, pw);
    return true;
  });
}
// A radio from its key prefix: your contacts first, then CoreScope's map
static PathStop stopForPrefix(const std::string &prefix) {
  std::string name;
  if (Companion *r = R())
    for (auto &c : r->contacts)
      if (c.key.size() >= prefix.size() && strncasecmp(c.key.c_str(), prefix.c_str(), prefix.size()) == 0) {
        name = c.name;
        break;
      }
  if (name.empty())
    for (auto &nd : M->nodes)
      if (nd.key.size() >= prefix.size() && strncasecmp(nd.key.c_str(), prefix.c_str(), prefix.size()) == 0) {
        name = nd.name;
        break;
      }
  return stopFor(name.empty() ? "Radio " + prefix.substr(0, 4) : name, prefix);
}
static std::string secsText(uint32_t s) {
  char b[24];
  if (s < 90) snprintf(b, sizeof b, "%us ago", (unsigned)s);
  else if (s < 5400) snprintf(b, sizeof b, "%um ago", (unsigned)(s / 60));
  else if (s < 172800) snprintf(b, sizeof b, "%uh ago", (unsigned)(s / 3600));
  else snprintf(b, sizeof b, "%ud ago", (unsigned)(s / 86400));
  return b;
}
static void neighboursOnMap() {
  Companion *r = R();
  RContact *c = r ? r->contactByPrefix(selKey) : nullptr;
  if (!c) return;
  RemoteStatus &rs = r->remote[c->key.substr(0, 12)];
  std::vector<PathStop> stops{stopFor(c->name, c->key.substr(0, 16))};
  for (auto &nb : rs.neigh) {
    PathStop st = stopForPrefix(nb.prefix);
    char b[24];
    snprintf(b, sizeof b, "  %+.1f dB", nb.snr);
    st.label += b;
    stops.push_back(st);
  }
  showScreen(S_MAP);
  mapShowPath(c->name + " hears " + std::to_string(rs.neigh.size()) + " repeaters", stops, true);
}
static void foundRouteOnMap() {
  Companion *r = R();
  if (!r || !r->pathDisc.done || r->pathDisc.failed) return;
  RContact *c = r->contactByPrefix(r->pathDisc.key);
  std::vector<PathStop> stops{youStop()};
  std::vector<HopGuess> g = resolveHops(r->pathDisc.out);
  for (size_t i = 0; i < g.size(); i++) {
    PathStop st;
    st.label = g[i].name.empty() ? "Repeater " + r->pathDisc.out[i] : (g[i].others ? "~" : "") + g[i].name;
    st.hasLoc = g[i].hasLoc;
    st.lat = g[i].lat;
    st.lon = g[i].lon;
    stops.push_back(st);
  }
  stops.push_back(c ? stopFor(c->name, c->key.substr(0, 16)) : stopForPrefix(r->pathDisc.key.substr(0, 12)));
  showScreen(S_MAP);
  mapShowPath("Route to " + stops.back().label, stops, false);
}
static std::string hopsText(const std::vector<std::string> &hops) {
  if (hops.empty()) return "straight (in direct range)";
  std::string t;
  for (auto &h : hops) {
    std::string n = hopName(h);
    t += (t.empty() ? "" : " > ") + (n.empty() ? h : n);
  }
  return t;
}
// After tapping Neighbours, Command or Find a route, keep its result card in view for a while
static int focusCard = 0;  // 1 neighbours, 2 admin, 3 route
static uint32_t focusAt = 0;
static void focusOn(int which) {
  focusCard = which;
  focusAt = lv_tick_get();
}
static void maybeFocus(lv_obj_t *card, int which) {
  if (focusCard != which || lv_tick_elaps(focusAt) > 45000) return;
  lv_obj_update_layout(detail);
  lv_obj_scroll_to_view_recursive(card, LV_ANIM_OFF);
}
static void cliPrompt() {
  focusOn(2);
  RContact *c = R() ? R()->contactByPrefix(selKey) : nullptr;
  if (!c) return;
  std::string k = c->key;
  prompt("Command for " + c->name, "Repeater admin commands, as in the MeshCore app's command line. Tap one below or type your own.", "", 120,
         "Send", [k](const std::string &t) {
           if (t.empty()) return false;
           if (R()) R()->sendCli(k, t);
           return true;
         },
         {"ver", "clock", "get name", "get radio", "get tx", "get repeat", "neighbors", "advert"});
}
static void renderDetail() {
  lv_obj_clean(detailBody);
  Companion *r = R();
  RContact *c = r && !selKey.empty() ? r->contactByPrefix(selKey) : nullptr;
  if (!c) {
    wrapLabel(detailBody, F16, C_DIM, "Tap someone on the left to see how to reach them.");
    lv_obj_t *b = mkBtn(detailBody, LV_SYMBOL_WIFI "  Say hello everywhere", [](lv_event_t *) {
      if (R()) R()->sendAdvert(true);
    }, nullptr, C_ACC, T.onAcc, F16);
    lv_obj_set_height(b, 48);
    wrapLabel(detailBody, F12, C_DIM2, "Saying hello lets other radios add you, and they usually say hello back.");
    return;
  }
  lv_obj_t *head = lv_obj_create(detailBody);
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, lv_pct(100), 64);
  lv_obj_t *av = mkAvatar(head, 60);
  setAvatar(av, c->name, c->type);
  lv_obj_t *nm = mkLabel(head, F24, C_TEXT, c->name.c_str());
  lv_obj_set_pos(nm, 72, 4);
  lv_obj_set_width(nm, CW - 330 - 24 - 72);
  oneLine(nm);
  std::string tl = std::string(typeName(c->type)) + (c->fav() ? "  |  favorite" : "");
  lv_obj_set_pos(mkLabel(head, F14, C_DIM, tl.c_str()), 72, 36);

  std::string info = "Last heard: " + agoLong(c->lastAdvert) + "\nRoute: " + routeText(c->pathLen);
  if (c->hasLoc) {
    char b[64];
    snprintf(b, sizeof b, "\nAbout %.1f miles away", milesBetween(myPlace(), {c->lat, c->lon}));
    info += b;
  }
  wrapLabel(detailBody, F14, C_TEXT, info);

  lv_obj_t *acts = mkRow(detailBody);
  if (c->type == CT_CHAT || c->type == CT_ROOM) {
    actBtn(acts, LV_SYMBOL_ENVELOPE "  Message", [](lv_event_t *) {
      RContact *cc = R() ? R()->contactByPrefix(selKey) : nullptr;
      if (!cc) return;
      Conv &cv = R()->openDm(*cc);
      selectConv(cv.id);
      showSection(SEC_CHATS);
      openCompose(nullptr);
    }, T.onAcc, C_ACC);
  }
  if (c->type == CT_REPEATER) {
    actBtn(acts, LV_SYMBOL_SHUFFLE "  Trace route", [](lv_event_t *) { startTrace(selKey); }, T.onAcc, C_ACC);
    actBtn(acts, LV_SYMBOL_LIST "  Neighbours", [](lv_event_t *) {
      RContact *cc = R() ? R()->contactByPrefix(selKey) : nullptr;
      if (!cc) return;
      RemoteStatus &rs = R()->remote[cc->key.substr(0, 12)];
      focusOn(1);
      if (rs.loggedIn) R()->requestNeighbours(cc->key);
      else loginPrompt(selKey);
    }, T.onAcc, C_ACC);
  }
  actBtn(acts, LV_SYMBOL_GPS "  Find a route", [](lv_event_t *) {
    RContact *cc = R() ? R()->contactByPrefix(selKey) : nullptr;
    focusOn(3);
    if (cc) R()->discoverPath(cc->key);
  }, T.onAcc, C_ACC);
  if (c->type == CT_REPEATER || c->type == CT_ROOM) {
    actBtn(acts, LV_SYMBOL_REFRESH "  Check status", [](lv_event_t *) {
      RemoteStatus &rs = R()->remote[selKey.substr(0, 12)];
      if (rs.loggedIn) R()->requestStatus(selKey);
      else loginPrompt(selKey);
    }, T.onAcc, C_ACC);
  }
  actBtn(acts, c->fav() ? LV_SYMBOL_CLOSE "  Unfavorite" : LV_SYMBOL_OK "  Favorite", [](lv_event_t *) {
    RContact *cc = R() ? R()->contactByPrefix(selKey) : nullptr;
    if (cc) R()->setFavorite(cc->key, !cc->fav());
  }, C_STAR);
  actBtn(acts, LV_SYMBOL_LOOP "  Forget route", [](lv_event_t *) {
    modal("Forget the route?", "Your radio will search the whole mesh for this contact again on the next message. Do this if messages stopped arriving.",
          "Forget route", [] {
            if (R()) R()->resetPath(selKey);
          });
  });
  actBtn(acts, LV_SYMBOL_UPLOAD "  Share nearby", [](lv_event_t *) {
    modal("Share this contact?", "Sends this contact card to radios in direct range, so people near you can add them too.", "Share", [] {
      if (R()) R()->shareContact(selKey);
    });
  });
  actBtn(acts, LV_SYMBOL_TRASH "  Remove", [](lv_event_t *) {
    RContact *cc = R() ? R()->contactByPrefix(selKey) : nullptr;
    if (!cc) return;
    std::string note = cc->name + " will be removed from your radio.";
    if (!R()->manualAdd) note += "\n\nYour radio adds new radios automatically, so they come back the next time they say hello. Turn that off in My Radio to keep them away.";
    modal("Remove " + cc->name + "?", note, "Remove", [] {
      if (R()) R()->removeContact(selKey);
      selKey.clear();
    }, true);
  }, 0xFF7A59);

  if (c->type == CT_REPEATER || c->type == CT_ROOM) {
    RemoteStatus &rs = r->remote[c->key.substr(0, 12)];
    lv_obj_t *card = mkCard(detailBody, CW - 330 - 24);
    lv_obj_t *hr = mkRow(card);
    mkLabel(hr, F16, C_TEXT, c->type == CT_ROOM ? "Room status" : "Repeater status");
    lv_obj_t *hq = mkBtn(hr, "?", [](lv_event_t *) { help("Repeaters and rooms", HELP_REPEATER); }, nullptr, C_BTN, C_ACC, F14);
    lv_obj_set_size(hq, 34, 30);
    std::string st;
    if (rs.waitingLogin) st = "Logging in... (can take up to a minute over several hops)";
    else if (rs.waitingStatus) st = "Asking for status...";
    else if (rs.loginFailed) st = "Login failed. Try again with the right password, or empty for guest.";
    else if (!rs.loggedIn) st = "Tap Check status to log in (guest is fine).";
    if (!st.empty()) wrapLabel(card, F14, C_DIM, st);
    if (rs.hasStats) {
      char b[400];
      int up = (int)rs.upSecs;
      snprintf(b, sizeof b,
               "Battery: %.2f V\nUp for: %dd %dh %dm\nPackets heard: %u   relayed: %u\nLast signal: SNR %.1f dB, RSSI %d dBm\nNoise floor: %d dBm   queue: %d",
               rs.battMv / 1000.0, up / 86400, up / 3600 % 24, up / 60 % 60, (unsigned)rs.recv, (unsigned)rs.sent, rs.lastSnr, rs.lastRssi, rs.noiseFloor,
               rs.txQueue);
      wrapLabel(card, F14, C_TEXT, b);
      wrapLabel(card, F12, C_DIM2, "Updated " + agoLong(rs.at) + (rs.isAdmin ? "  |  logged in as admin" : "  |  logged in as guest"));
    }
    if (c->type == CT_REPEATER && (rs.waitingNeigh || rs.hasNeigh || rs.neighAt)) {
      lv_obj_t *nc = mkCard(detailBody, CW - 330 - 24);
      lv_obj_t *nh = mkRow(nc);
      mkLabel(nh, F16, C_TEXT, "Neighbours: repeaters it hears");
      if (!rs.neigh.empty()) {
        lv_obj_t *mb = mkBtn(nh, LV_SYMBOL_GPS "  Map", [](lv_event_t *) { neighboursOnMap(); }, nullptr, C_BTN, C_TEXT, F12);
        lv_obj_set_height(mb, 30);
      }
      if (rs.waitingNeigh) wrapLabel(nc, F14, C_DIM, "Asking... (can take up to a minute over several hops)");
      else if (!rs.hasNeigh) wrapLabel(nc, F14, C_DIM, "No answer. Its firmware may be too old, or the reply got lost. Try again.");
      else if (rs.neigh.empty()) wrapLabel(nc, F14, C_DIM, "It hasn't heard any other repeaters directly.");
      else {
        std::string t;
        for (auto &nb : rs.neigh) {
          char b[48];
          snprintf(b, sizeof b, "   SNR %+.1f dB   %s\n", nb.snr, secsText(nb.secsAgo).c_str());
          t += stopForPrefix(nb.prefix).label + b;
        }
        if (!t.empty()) t.pop_back();
        wrapLabel(nc, F14, C_TEXT, t);
        char b[64];
        snprintf(b, sizeof b, "Strongest %d of %d  |  updated ", (int)rs.neigh.size(), rs.neighTotal);
        wrapLabel(nc, F12, C_DIM2, b + agoLong(rs.neighAt));
      }
      maybeFocus(nc, 1);
    }
    if (c->type == CT_REPEATER && rs.isAdmin) {
      lv_obj_t *ac = mkCard(detailBody, CW - 330 - 24);
      lv_obj_t *ah = mkRow(ac);
      mkLabel(ah, F16, C_TEXT, "Remote admin");
      lv_obj_t *rb = mkBtn(ah, LV_SYMBOL_KEYBOARD "  Command", [](lv_event_t *) { cliPrompt(); }, nullptr, C_ACC, T.onAcc, F14);
      lv_obj_set_height(rb, 34);
      std::string t;
      size_t from = rs.cli.size() > 14 ? rs.cli.size() - 14 : 0;
      for (size_t i = from; i < rs.cli.size(); i++) t += rs.cli[i] + "\n";
      if (rs.waitingCli) t += "...";
      else if (!t.empty()) t.pop_back();
      wrapLabel(ac, F14, rs.cli.empty() ? C_DIM : C_TEXT,
                t.empty() ? "Send commands to this repeater as its owner. Its answers show here." : t);
      maybeFocus(ac, 2);
    }
  }
  if (r->pathDisc.key == c->key && (r->pathDisc.waiting || r->pathDisc.done)) {
    PathDiscovery &pd = r->pathDisc;
    lv_obj_t *pc = mkCard(detailBody, CW - 330 - 24);
    lv_obj_t *ph = mkRow(pc);
    mkLabel(ph, F16, C_TEXT, "Fresh route");
    if (pd.done && !pd.failed) {
      lv_obj_t *mb = mkBtn(ph, LV_SYMBOL_GPS "  Map", [](lv_event_t *) { foundRouteOnMap(); }, nullptr, C_BTN, C_TEXT, F12);
      lv_obj_set_height(mb, 30);
    }
    if (pd.waiting) {
      char b[96];
      snprintf(b, sizeof b, "Searching the whole mesh... %ds", (int)((P->nowMs() - pd.started) / 1000));
      wrapLabel(pc, F14, C_DIM, b);
    } else if (pd.failed) wrapLabel(pc, F14, C_DIM, pd.note.empty() ? "No route came back." : pd.note);
    else {
      std::vector<std::string> backTravel(pd.back.rbegin(), pd.back.rend());  // in the order the answer travelled
      wrapLabel(pc, F14, C_TEXT, "There: " + hopsText(pd.out) + "\nBack: " + hopsText(backTravel));
      char b[96];
      snprintf(b, sizeof b, "Found in %ds. Messages keep using the saved route; Forget route makes your radio learn a new one.",
               (int)std::max<int64_t>(1, (P->nowMs() - pd.started) / 1000));
      wrapLabel(pc, F12, C_DIM2, b);
    }
    maybeFocus(pc, 3);
  }
}
static void renderPeople() {
  Companion *r = R();
  std::vector<RContact *> list;
  if (r)
    for (auto &c : r->contacts)
      if (pplMatch(c)) list.push_back(&c);
  std::sort(list.begin(), list.end(), [](RContact *a, RContact *b) {
    if (a->fav() != b->fav()) return a->fav();
    return a->lastAdvert > b->lastAdvert;
  });
  for (int i = 0; i < 5; i++) {
    bool on = i == pplFilter;
    lv_obj_set_style_bg_color(pplChips[i], lv_color_hex(on ? T.chipOn : C_PANEL), 0);
    lv_obj_set_style_border_color(pplChips[i], lv_color_hex(on ? C_ACC : T.btnBorder), 0);
  }
  size_t shown = std::min<size_t>(list.size(), 80);
  pKeys.clear();
  for (size_t i = 0; i < shown; i++) {
    RContact &c = *list[i];
    PRow &w = pRow(i);
    pKeys.push_back(c.key);
    setAvatar(w.av, c.name, c.type);
    lv_label_set_text(w.name, c.name.c_str());
    std::string sub = std::string(typeName(c.type)) + "  |  " + agoLong(c.lastAdvert) + "  |  " +
                      (c.pathLen < 0 ? "no route" : c.pathLen == 0 ? "direct" : std::to_string(c.pathLen) + " hops");
    lv_label_set_text(w.sub, sub.c_str());
    hide(w.star, !c.fav());
    lv_obj_set_style_bg_color(w.box, lv_color_hex(c.key == selKey ? T.sel : T.listBg), 0);
    hide(w.box, false);
  }
  for (size_t i = shown; i < pRows.size(); i++) hide(pRows[i].box, true);
  size_t total = r ? r->contacts.size() : 0;
  lv_label_set_text_fmt(pplCount, "%d of %d", (int)list.size(), (int)total);
  lv_label_set_text(pplEmpty, total == 0 ? "Nobody yet. Radios appear here after they say hello. Tap ? to learn why it can take a while."
                                         : "Nobody matches this filter.");
  hide(pplEmpty, shown > 0);
  renderDetail();
}

// ================================================================ CHANNELS
static lv_obj_t *chList, *chSide;
static int selChan = -1;
static std::vector<lv_obj_t *> chRows;
static const char *kindName(ChanKind k) { return k == CK_PUBLIC ? "Public" : k == CK_HASHTAG ? "Hashtag" : "Private"; }
static const char *kindHelp(ChanKind k) {
  return k == CK_PUBLIC ? "The channel every MeshCore radio shares. Anyone can read it."
         : k == CK_HASHTAG ? "Anyone who joins the same #name is in this chat. Not secret."
                           : "Only people with this channel's key can read it.";
}
static void buildChannels(lv_obj_t *s) {
  chList = mkBox(s, T.listBg);
  lv_obj_set_size(chList, 330, CH);
  lv_obj_add_flag(chList, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(chList, LV_DIR_VER);
  lv_obj_set_flex_flow(chList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_border_side(chList, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(chList, 1, 0);
  lv_obj_set_style_border_color(chList, lv_color_hex(C_LINE), 0);
  chSide = mkBox(s, C_BG);
  lv_obj_set_pos(chSide, 330, 0);
  lv_obj_set_size(chSide, CW - 330, CH);
  lv_obj_add_flag(chSide, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(chSide, LV_DIR_VER);
  lv_obj_set_flex_flow(chSide, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(chSide, 12, 0);
  lv_obj_set_style_pad_row(chSide, 10, 0);
}
static void joinHashtagPrompt() {
  std::vector<std::string> chips;
  for (auto &c : M->channels)
    if (!c.name.empty() && c.name[0] == '#' && chips.size() < 8) chips.push_back(c.name);
  prompt("Join a hashtag channel", "Type the name, like #chicago. Everyone using the same name ends up in the same chat.", "#", 31, "Join",
         [](const std::string &t) {
           if (t.size() < 2) return false;
           if (R()) R()->addHashtagChannel(t);
           return true;
         }, chips);
}
static void renderChannelSide() {
  lv_obj_clean(chSide);
  Companion *r = R();
  RChannel *ch = r && selChan >= 0 ? r->channelByIdx(selChan) : nullptr;
  if (ch) {
    lv_obj_t *back = mkBtn(chSide, LV_SYMBOL_LEFT "  Back", [](lv_event_t *) {
      selChan = -1;
      dirty[SEC_CHANNELS] = true;
    }, nullptr, C_BTN, C_TEXT, F14);
    lv_obj_set_height(back, 36);
    mkLabel(chSide, F24, C_TEXT, ch->name.c_str());
    wrapLabel(chSide, F14, C_DIM, std::string(kindName(ch->kind)) + " channel  |  slot " + std::to_string(ch->idx) + "\n" + kindHelp(ch->kind));
    if (ch->kind == CK_PRIVATE) {
      lv_obj_t *qr = lv_qrcode_create(chSide);
      lv_qrcode_set_size(qr, 150);
      lv_qrcode_set_dark_color(qr, lv_color_hex(0x0A0E13));
      lv_qrcode_set_light_color(qr, lv_color_white());
      lv_obj_set_style_border_color(qr, lv_color_white(), 0);
      lv_obj_set_style_border_width(qr, 6, 0);
      std::string link = ch->shareLink();
      lv_qrcode_update(qr, link.c_str(), (uint32_t)link.size());
      wrapLabel(chSide, F12, C_DIM, "Friends scan this with the MeshCore app to join. Key:\n" + ch->secretHex());
    }
    lv_obj_t *row = mkRow(chSide);
    lv_obj_t *open = mkBtn(row, LV_SYMBOL_ENVELOPE "  Open chat", [](lv_event_t *) {
      selectConv("c:" + std::to_string(selChan));
      showSection(SEC_CHATS);
    }, nullptr, C_ACC, T.onAcc, F14);
    lv_obj_set_height(open, 44);
    lv_obj_t *rm = mkBtn(row, LV_SYMBOL_TRASH "  Remove", [](lv_event_t *) {
      RChannel *c = R() ? R()->channelByIdx(selChan) : nullptr;
      if (!c) return;
      std::string warn = c->kind == CK_PRIVATE ? "\n\nThis is a private channel: if you don't have the key written down, you can only rejoin with someone's QR code." : "";
      modal("Remove " + c->name + "?", "You'll stop seeing its messages. You can join again any time." + warn, "Remove", [] {
        if (R()) R()->removeChannel(selChan);
        selChan = -1;
      }, true);
    }, nullptr, C_BTN, 0xFF7A59, F14);
    lv_obj_set_height(rm, 44);
    return;
  }
  lv_obj_t *hr = mkRow(chSide);
  mkLabel(hr, F18, C_TEXT, "Add a channel");
  lv_obj_t *hq = mkBtn(hr, "?", [](lv_event_t *) { help("About channels", HELP_CHANNELS); }, nullptr, C_BTN, C_ACC, F14);
  lv_obj_set_size(hq, 34, 30);
  struct Opt {
    const char *title, *desc;
    lv_event_cb_t cb;
  };
  Opt opts[] = {
      {"# Join a hashtag channel", "Type a name like #chicago. Anyone can join.", [](lv_event_t *) { joinHashtagPrompt(); }},
      {LV_SYMBOL_WIFI "  Join Public", "The channel every MeshCore radio shares.", [](lv_event_t *) {
         if (R()) R()->addPublicChannel();
       }},
      {LV_SYMBOL_PLUS "  Create a private channel", "Makes a secret key and a QR code to share.", [](lv_event_t *) {
         prompt("Name your private channel", "Only people you share the QR code with can join.", "", 31, "Create", [](const std::string &t) {
           if (t.empty()) return false;
           if (R()) R()->createPrivateChannel(t);
           return true;
         });
       }},
      {LV_SYMBOL_EDIT "  Join a private channel", "Someone gave you a name and a 32-character key.", [](lv_event_t *) {
         prompt("Private channel name", "Use the same name your friend uses.", "", 31, "Next", [](const std::string &name) {
           if (name.empty()) return false;
           std::string nm = name;
           lv_async_call([](void *p) {
             std::string *nmp = (std::string *)p;
             std::string nm2 = *nmp;
             delete nmp;
             prompt("Key for " + nm2, "32 characters, 0-9 and a-f.", "", 32, "Join", [nm2](const std::string &k) {
               if (k.size() != 32 || k.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
                 radioToast(false, "The key must be exactly 32 characters, 0-9 and a-f");
                 return false;
               }
               uint8_t sec[16];
               for (int i = 0; i < 16; i++) sec[i] = (uint8_t)strtol(k.substr(i * 2, 2).c_str(), nullptr, 16);
               if (R()) R()->addPrivateChannel(nm2, sec, false);
               return true;
             });
           }, new std::string(nm));
           return true;
         });
       }},
  };
  for (auto &o : opts) {
    lv_obj_t *b = mkBtn(chSide, "", o.cb, nullptr, C_PANEL, C_TEXT, F16);
    lv_obj_delete(lv_obj_get_child(b, 0));
    lv_obj_set_size(b, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    mkLabel(b, F16, C_TEXT, o.title);
    mkLabel(b, F12, C_DIM, o.desc);
  }
  // popular hashtag channels seen on CoreScope in your area, not joined yet
  std::vector<const ChannelInfo *> pop;
  for (auto &c : M->channels) {
    if (c.name.size() < 2 || c.name[0] != '#') continue;
    bool joined = false;
    if (r)
      for (auto &j : r->channels) joined |= j.name == c.name;
    if (!joined) pop.push_back(&c);
  }
  std::sort(pop.begin(), pop.end(), [](const ChannelInfo *a, const ChannelInfo *b) { return a->count > b->count; });
  if (!pop.empty()) {
    mkLabel(chSide, F16, C_TEXT, "Popular near you");
    wrapLabel(chSide, F12, C_DIM, "Active hashtag channels on " + M->areaLabel + " CoreScope. Tap to join.");
    lv_obj_t *row = mkRow(chSide);
    for (size_t i = 0; i < pop.size() && i < 10; i++) {
      std::string t = pop[i]->name + "  " + std::to_string(pop[i]->count);
      std::string *nm = new std::string(pop[i]->name);
      lv_obj_t *b = mkBtn(row, t.c_str(), [](lv_event_t *e) {
        std::string *n = (std::string *)lv_event_get_user_data(e);
        if (R()) R()->addHashtagChannel(*n);
      }, nm, T.chipOn, C_TEXT, F14);
      lv_obj_add_event_cb(b, [](lv_event_t *e) { delete (std::string *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, nm);
      lv_obj_set_height(b, 36);
      lv_obj_set_style_radius(b, 18, 0);
    }
  }
}
static void renderChannels() {
  for (auto *o : chRows) lv_obj_delete(o);
  chRows.clear();
  Companion *r = R();
  if (r) {
    lv_obj_t *hdr = mkLabel(chList, F12, C_DIM, (std::to_string(r->channels.size()) + " of " + std::to_string(r->maxChannels) + " channel slots used").c_str());
    lv_obj_set_style_pad_all(hdr, 12, 0);
    chRows.push_back(hdr);
    for (auto &ch : r->channels) {
      lv_obj_t *b = mkBox(chList, ch.idx == selChan ? T.sel : T.listBg);
      lv_obj_set_size(b, 330, 62);
      lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_color(b, lv_color_hex(T.listPressed), LV_STATE_PRESSED);
      lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(b, 1, 0);
      lv_obj_set_style_border_color(b, lv_color_hex(T.listLine), 0);
      lv_obj_add_event_cb(b, [](lv_event_t *e) {
        selChan = (int)(intptr_t)lv_event_get_user_data(e);
        dirty[SEC_CHANNELS] = true;
      }, LV_EVENT_CLICKED, (void *)(intptr_t)ch.idx);
      lv_obj_t *av = mkAvatar(b, 38);
      lv_obj_set_pos(av, 10, 12);
      lv_obj_remove_flag(av, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_bg_color(av, lv_color_hex(ch.kind == CK_PRIVATE ? 0xB78CFF : gcol(G_CHANNEL)), 0);
      lv_label_set_text(lv_obj_get_child(av, 0), ch.kind == CK_PUBLIC ? LV_SYMBOL_WIFI : ch.kind == CK_HASHTAG ? "#" : LV_SYMBOL_EYE_CLOSE);
      lv_obj_t *nm = mkLabel(b, F16, C_TEXT, ch.name.c_str());
      lv_obj_set_pos(nm, 58, 10);
      lv_obj_set_width(nm, 250);
      oneLine(nm);
      lv_obj_set_pos(mkLabel(b, F12, C_DIM, kindName(ch.kind)), 58, 34);
      chRows.push_back(b);
    }
  }
  renderChannelSide();
}

// ================================================================ MY RADIO
static lv_obj_t *myRoot;
struct Preset {
  const char *name;
  double f, bw;
  int sf, cr;
};
// From the MeshCore app's preset list (meshcore-open lib/models/radio_settings.dart)
static const Preset PRESETS[] = {
    {"USA / Canada", 910.525, 62.5, 7, 5},         {"EU / UK (Narrow)", 869.618, 62.5, 8, 5},  {"Australia (Narrow)", 916.575, 62.5, 7, 5},
    {"New Zealand (Narrow)", 917.375, 62.5, 7, 5}, {"Australia", 915.8, 250, 10, 5},           {"EU / UK (Long Range)", 869.525, 250, 11, 5},
};
static int presetFor(double f, double bw, int sf) {
  for (int i = 0; i < (int)(sizeof PRESETS / sizeof PRESETS[0]); i++)
    if (std::fabs(PRESETS[i].f - f) < 0.002 && std::fabs(PRESETS[i].bw - bw) < 0.1 && PRESETS[i].sf == sf) return i;
  return -1;
}
static int suggestedPreset() {
  double lat = M->center.lat, lon = M->center.lon;
  if (lon > -170 && lon < -50 && lat > 10) return 0;
  if (lat > 34 && lat < 72 && lon > -25 && lon < 45) return 1;
  if (lat < -10 && lat > -45 && lon > 110 && lon < 155) return 2;
  if (lat < -33 && lat > -48 && lon > 165) return 3;
  return -1;
}
static void applyPreset(int i) {
  const Preset &p = PRESETS[i];
  modal(std::string("Switch to ") + p.name + "?",
        "Everyone near you has to use the same setting, or you won't hear each other. If your local group uses something else, ask them first.",
        "Switch", [i] {
          const Preset &q = PRESETS[i];
          if (R()) R()->setRadio(q.f, q.bw, q.sf, q.cr);
        });
}
static void runWizard();

static void buildMyRadio(lv_obj_t *s) {
  myRoot = mkBox(s, C_BG);
  lv_obj_set_size(myRoot, CW, CH);
  lv_obj_add_flag(myRoot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(myRoot, LV_DIR_VER);
  lv_obj_set_flex_flow(myRoot, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_all(myRoot, 10, 0);
  lv_obj_set_style_pad_row(myRoot, 10, 0);
  lv_obj_set_style_pad_column(myRoot, 10, 0);
}
static lv_obj_t *cardHeader(lv_obj_t *card, const char *title, const char *helpTitle = nullptr, const char *helpText = nullptr) {
  lv_obj_t *hr = mkRow(card);
  mkLabel(hr, F18, C_TEXT, title);
  if (helpText) {
    auto *hp = new std::pair<const char *, const char *>(helpTitle, helpText);
    lv_obj_t *hq = mkBtn(hr, "?", [](lv_event_t *e) {
      auto *p = (std::pair<const char *, const char *> *)lv_event_get_user_data(e);
      help(p->first, p->second);
    }, hp, C_BTN, C_ACC, F14);
    lv_obj_add_event_cb(hq, [](lv_event_t *e) { delete (std::pair<const char *, const char *> *)lv_event_get_user_data(e); }, LV_EVENT_DELETE, hp);
    lv_obj_set_size(hq, 34, 30);
  }
  return hr;
}
static void renderMyRadio() {
  lv_obj_clean(myRoot);
  Companion *r = R();
  if (!r) return;
  const int W2 = (CW - 30) / 2;
  // identity
  lv_obj_t *id = mkCard(myRoot, W2);
  lv_obj_t *head = lv_obj_create(id);
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, lv_pct(100), 60);
  lv_obj_t *av = mkAvatar(head, 56);
  setAvatar(av, r->selfName, CT_CHAT);
  lv_obj_t *nm = mkLabel(head, F24, C_TEXT, r->selfName.empty() ? "(no name)" : r->selfName.c_str());
  lv_obj_set_pos(nm, 68, 2);
  lv_obj_set_width(nm, W2 - 28 - 68);
  oneLine(nm);
  lv_obj_set_pos(mkLabel(head, F12, C_DIM, ("ID " + r->selfKey.substr(0, 8) + "  |  " + r->devName).c_str()), 68, 34);
  std::string hw = r->model.empty() ? "" : r->model + "  " + r->fwVersion;
  if (r->battMv > 0) {
    char b[32];
    snprintf(b, sizeof b, "%sBattery %.2f V", hw.empty() ? "" : "\n", r->battMv / 1000.0);
    hw += b;
  }
  if (!hw.empty()) wrapLabel(id, F12, C_DIM, hw);
  lv_obj_t *er = mkRow(id);
  lv_obj_t *en = mkBtn(er, LV_SYMBOL_EDIT "  Change name", [](lv_event_t *) {
    prompt("Your name on the mesh", "This is what others see. Keep it short, like DudeTested or KD9XYZ Desk.", R()->selfName, 31, "Save", [](const std::string &t) {
      if (t.empty()) return false;
      R()->setName(t);
      return true;
    });
  }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_height(en, 40);
  lv_obj_t *wz = mkBtn(er, LV_SYMBOL_PLAY "  Setup guide", [](lv_event_t *) { runWizard(); }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_height(wz, 40);

  // say hello
  lv_obj_t *hello = mkCard(myRoot, W2);
  cardHeader(hello, "Say hello", "Saying hello (adverts)", HELP_HELLO);
  wrapLabel(hello, F12, C_DIM, "Lets other radios add you. Do it after changing your name or location.");
  lv_obj_t *hr = mkRow(hello);
  lv_obj_t *h1 = mkBtn(hr, "Hello nearby", [](lv_event_t *) { R()->sendAdvert(false); }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_height(h1, 44);
  lv_obj_t *h2 = mkBtn(hr, LV_SYMBOL_WIFI "  Hello everywhere", [](lv_event_t *) { R()->sendAdvert(true); }, nullptr, C_ACC, T.onAcc, F14);
  lv_obj_set_height(h2, 44);

  // radio health (CMD_GET_STATS): asked for when this page is shown, at most once a minute
  bool newFw = r->fwCode == 0 || r->fwCode >= 8;
  static int64_t lastAsk = 0;
  if (newFw && !r->statsUnsupported && P->nowMs() - lastAsk > 60000) {
    lastAsk = P->nowMs();
    r->requestStats();
  }
  lv_obj_t *hl = mkCard(myRoot, W2);
  lv_obj_t *hh = cardHeader(hl, "Radio health");
  lv_obj_t *rf = mkBtn(hh, LV_SYMBOL_REFRESH, [](lv_event_t *) { R()->requestStats(); }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_size(rf, 40, 30);
  const RadioStats &rs = r->stats;
  if (!newFw || r->statsUnsupported) {
    wrapLabel(hl, F12, C_DIM, "Your radio's firmware is too old to report this. Update the companion firmware to see it.");
  } else if (!rs.core && !rs.radio && !rs.packets) {
    wrapLabel(hl, F12, C_DIM, "Asking your radio...");
  } else {
    auto dur = [](uint32_t sec) {
      char b[32];
      if (sec >= 86400) snprintf(b, sizeof b, "%u d %u h", (unsigned)(sec / 86400), (unsigned)(sec % 86400 / 3600));
      else if (sec >= 3600) snprintf(b, sizeof b, "%u h %u min", (unsigned)(sec / 3600), (unsigned)(sec % 3600 / 60));
      else if (sec >= 60) snprintf(b, sizeof b, "%u min %u s", (unsigned)(sec / 60), (unsigned)(sec % 60));
      else snprintf(b, sizeof b, "%u s", (unsigned)sec);
      return std::string(b);
    };
    char b[200];
    if (rs.radio) {
      const char *q = rs.noiseFloor <= -105 ? "quiet" : rs.noiseFloor <= -95 ? "some noise" : "noisy: something nearby may be interfering";
      snprintf(b, sizeof b, "Background noise  %d dBm  (%s)", rs.noiseFloor, q);
      wrapLabel(hl, F14, C_TEXT, b);
      snprintf(b, sizeof b, "Last packet heard  %d dBm, %+.1f dB", rs.lastRssi, rs.lastSnr);
      wrapLabel(hl, F12, C_DIM, b);
    }
    if (rs.radio && rs.core && rs.upSecs) {
      snprintf(b, sizeof b, "Time on air  sending %s (%.2f%%), receiving %s", dur(rs.txAirSecs).c_str(), 100.0 * rs.txAirSecs / rs.upSecs,
               dur(rs.rxAirSecs).c_str());
      wrapLabel(hl, F12, C_DIM, b);
    }
    if (rs.packets) {
      snprintf(b, sizeof b, "Packets  heard %u (flood %u, direct %u), sent %u (flood %u, direct %u), damaged %u", (unsigned)rs.recv,
               (unsigned)rs.recvFlood, (unsigned)rs.recvDirect, (unsigned)rs.sent, (unsigned)rs.sentFlood, (unsigned)rs.sentDirect,
               (unsigned)rs.recvErrors);
      wrapLabel(hl, F12, C_DIM, b);
    }
    if (rs.core) {
      snprintf(b, sizeof b, "Up %s  |  waiting to send %d", dur(rs.upSecs).c_str(), rs.queueLen);
      wrapLabel(hl, F12, C_DIM, b);
    }
  }

  // who can hear me (zero-hop discovery)
  lv_obj_t *wh = mkCard(myRoot, W2);
  cardHeader(wh, "Who can hear me?");
  wrapLabel(wh, F12, C_DIM,
            "Asks the repeaters and sensors in direct range to answer, and shows how well each one hears you. "
            "Good for checking your antenna or where to put the radio.");
  lv_obj_t *wb = mkBtn(wh, LV_SYMBOL_WIFI "  Check now", [](lv_event_t *) { startDiscovery(); }, nullptr, newFw ? C_ACC : C_BTN, newFw ? T.onAcc : C_DIM,
                       F14);
  lv_obj_set_height(wb, 44);
  if (!newFw) wrapLabel(wh, F12, C_DIM2, "Needs newer companion firmware on your radio.");
  else if (r->discovery.done && !r->discovery.failed)
    wrapLabel(wh, F12, C_DIM2, "Last check: " + std::to_string(r->discovery.found.size()) +
                                   (r->discovery.found.size() == 1 ? " radio heard you, " : " radios heard you, ") + agoLong(r->discovery.started));

  // region
  lv_obj_t *reg = mkCard(myRoot, CW - 20);
  cardHeader(reg, "Region (radio settings)");
  int cur = presetFor(r->freqMHz, r->bwKHz, r->sf), sug = suggestedPreset();
  char rb[200];
  snprintf(rb, sizeof rb, "Now: %s   (%.3f MHz, BW %g kHz, SF%d, CR%d)", cur >= 0 ? PRESETS[cur].name : "Custom settings", r->freqMHz, r->bwKHz, r->sf, r->cr);
  wrapLabel(reg, F14, C_TEXT, rb);
  if (sug >= 0 && cur != sug)
    wrapLabel(reg, F12, C_STAR, std::string(LV_SYMBOL_WARNING "  Your map area suggests ") + PRESETS[sug].name +
                                    ". If you hear nobody, this may be why (but check what your local group uses).");
  lv_obj_t *pr = mkRow(reg);
  for (int i = 0; i < (int)(sizeof PRESETS / sizeof PRESETS[0]); i++) {
    std::string t = std::string(i == cur ? LV_SYMBOL_OK "  " : "") + PRESETS[i].name + (i == sug ? "  (suggested)" : "");
    lv_obj_t *b = mkBtn(pr, t.c_str(), [](lv_event_t *e) {
      int i = (int)(intptr_t)lv_event_get_user_data(e);
      applyPreset(i);
    }, (void *)(intptr_t)i, i == cur ? T.chipOn : C_BTN, C_TEXT, F14);
    lv_obj_set_height(b, 42);
    if (i == cur) lv_obj_set_style_border_color(b, lv_color_hex(C_ACC), 0);
  }

  // power
  lv_obj_t *pw = mkCard(myRoot, W2);
  cardHeader(pw, "Transmit power");
  wrapLabel(pw, F14, C_TEXT, "Now " + std::to_string(r->txPower) + " dBm (max " + std::to_string(r->maxTxPower) + ")");
  wrapLabel(pw, F12, C_DIM, "Higher reaches further but uses more battery. Indoors at a desk, Normal is plenty.");
  lv_obj_t *pwr = mkRow(pw);
  struct PL {
    const char *n;
    int dbm;
  } pls[] = {{"Low", 10}, {"Normal", 17}, {"Max", r->maxTxPower}};
  for (auto &p : pls) {
    lv_obj_t *b = mkBtn(pwr, p.n, [](lv_event_t *e) { R()->setTxPower((int)(intptr_t)lv_event_get_user_data(e)); }, (void *)(intptr_t)p.dbm,
                        r->txPower == p.dbm ? T.chipOn : C_BTN, C_TEXT, F14);
    lv_obj_set_size(b, 82, 42);
  }

  // location + auto add
  lv_obj_t *lc = mkCard(myRoot, W2);
  cardHeader(lc, "Location and contacts");
  wrapLabel(lc, F14, C_TEXT, r->locPolicy ? "Your location is shared when you say hello." : "Your location is not shared.");
  lv_obj_t *lr = mkRow(lc);
  lv_obj_t *l1 = mkBtn(lr, r->locPolicy ? "Stop sharing" : "Share my map area", [](lv_event_t *) {
    Companion *c = R();
    if (c->locPolicy) c->setLocation(false, 0, 0);
    else {
      // rounded to about 1 km, from the area set on the MESHtop settings page
      double la = std::round(M->center.lat * 100) / 100, lo = std::round(M->center.lon * 100) / 100;
      c->setLocation(true, la, lo);
    }
  }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_height(l1, 42);
  wrapLabel(lc, F14, C_TEXT, r->manualAdd ? "New radios are NOT added automatically." : "New radios are added to People automatically.");
  lv_obj_t *l2 = mkBtn(lc, r->manualAdd ? "Add them automatically" : "Stop auto-adding", [](lv_event_t *) { R()->setAutoAdd(R()->manualAdd != 0); }, nullptr,
                       C_BTN, C_TEXT, F14);
  lv_obj_set_height(l2, 42);

  // bluetooth + maintenance
  lv_obj_t *bt = mkCard(myRoot, CW - 20);
  cardHeader(bt, "Bluetooth and radio");
  wrapLabel(bt, F12, C_DIM,
            "The radio takes one Bluetooth connection at a time. To use the MeshCore phone app instead, tap Disconnect here first.");
  lv_obj_t *br = mkRow(bt);
  lv_obj_t *b1 = mkBtn(br, LV_SYMBOL_EDIT "  Change PIN", [](lv_event_t *) {
    prompt("New Bluetooth PIN", "6 digits. You'll need it on the settings page and in the phone app.", "", 6, "Save", [](const std::string &t) {
      if (t.size() != 6 || t.find_first_not_of("0123456789") != std::string::npos) {
        radioToast(false, "The PIN must be 6 digits");
        return false;
      }
      R()->setPin((uint32_t)atol(t.c_str()));
      return true;
    });
  }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_height(b1, 42);
  lv_obj_t *b2 = mkBtn(br, LV_SYMBOL_REFRESH "  Restart radio", [](lv_event_t *) {
    modal("Restart the radio?", "It reconnects by itself after a few seconds.", "Restart", [] { R()->reboot(); });
  }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_height(b2, 42);
  lv_obj_t *b3 = mkBtn(br, LV_SYMBOL_POWER "  Disconnect", [](lv_event_t *) { P->radioEnable(false); }, nullptr, C_BTN, C_TEXT, F14);
  lv_obj_set_height(b3, 42);
  lv_obj_t *b4 = mkBtn(br, LV_SYMBOL_CLOSE "  Use a different radio", [](lv_event_t *) {
    modal("Forget this radio?", "MESHtop will forget the paired radio and look for another one.", "Forget", [] { P->radioForget(); }, true);
  }, nullptr, C_BTN, 0xFF7A59, F14);
  lv_obj_set_height(b4, 42);
}

// ================================================================ setup wizard (first connect)
static lv_obj_t *wiz, *wizBody, *wizDots, *wizBack, *wizNext;
static int wizStep = 0;
static constexpr int WIZ_STEPS = 5;
static void wizRender();
static void wizGo(int s) {
  wizStep = s;
  if (wizStep >= WIZ_STEPS) {
    hide(wiz, true);
    P->setRadioSetupDone(true);
    showSection(SEC_CHATS);
    return;
  }
  wizRender();
}
static void runWizard() {
  if (!ready()) return;
  hide(wiz, false);
  lv_obj_move_foreground(wiz);
  wizGo(0);
}
static void buildWizard(lv_obj_t *scr) {
  wiz = mkBox(scr, C_BG);
  lv_obj_set_size(wiz, SCR_W, SCR_H);
  lv_obj_add_flag(wiz, LV_OBJ_FLAG_CLICKABLE);
  wizDots = mkRow(wiz);
  lv_obj_set_pos(wizDots, 30, 22);
  lv_obj_set_size(wizDots, 300, 20);
  wizBody = mkBox(wiz, C_BG);
  lv_obj_set_pos(wizBody, 30, 54);
  lv_obj_set_size(wizBody, SCR_W - 60, SCR_H - 54 - 80);
  lv_obj_set_flex_flow(wizBody, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(wizBody, 12, 0);
  lv_obj_add_flag(wizBody, LV_OBJ_FLAG_SCROLLABLE);
  wizBack = mkBtn(wiz, LV_SYMBOL_LEFT "  Back", [](lv_event_t *) { wizGo(std::max(0, wizStep - 1)); }, nullptr, C_BTN, C_TEXT, F16);
  lv_obj_set_size(wizBack, 140, 52);
  lv_obj_align(wizBack, LV_ALIGN_BOTTOM_LEFT, 30, -16);
  wizNext = mkBtn(wiz, "Next  " LV_SYMBOL_RIGHT, [](lv_event_t *) { wizGo(wizStep + 1); }, nullptr, C_ACC, T.onAcc, F18);
  lv_obj_set_size(wizNext, 180, 52);
  lv_obj_align(wizNext, LV_ALIGN_BOTTOM_RIGHT, -30, -16);
  lv_obj_t *skip = mkBtn(wiz, "Skip setup", [](lv_event_t *) { wizGo(WIZ_STEPS); }, nullptr, C_BG, C_DIM, F14);
  lv_obj_set_style_border_width(skip, 0, 0);
  lv_obj_align(skip, LV_ALIGN_TOP_RIGHT, -20, 14);
  hide(wiz, true);
}
static void wizRender() {
  Companion *r = R();
  if (!r) return;
  lv_obj_clean(wizDots);
  for (int i = 0; i < WIZ_STEPS; i++) {
    lv_obj_t *d = mkBox(wizDots, i <= wizStep ? C_ACC : T.btnBorder);
    lv_obj_set_size(d, i == wizStep ? 28 : 12, 12);
    lv_obj_set_style_radius(d, 6, 0);
  }
  lv_obj_clean(wizBody);
  hide(wizBack, wizStep == 0);
  setBtnText(wizNext, wizStep == WIZ_STEPS - 1 ? "Finish  " LV_SYMBOL_OK : "Next  " LV_SYMBOL_RIGHT);
  switch (wizStep) {
    case 0:
      mkLabel(wizBody, &lv_font_montserrat_28, C_TEXT, "Your radio is connected!");
      wrapLabel(wizBody, F18, C_DIM, "MESHtop is talking to " + (r->devName.empty() ? std::string("your radio") : r->devName) +
                                         ". Let's get you on the mesh in four quick steps: your name, your region, location, and channels.");
      wrapLabel(wizBody, F14, C_DIM2, "You can change all of this later under MESH > MY RADIO.");
      break;
    case 1: {
      mkLabel(wizBody, F24, C_TEXT, "1.  Your name on the mesh");
      wrapLabel(wizBody, F16, C_DIM, "This is what other people see next to your messages.");
      mkLabel(wizBody, &lv_font_montserrat_28, C_ACC, r->selfName.empty() ? "(no name yet)" : r->selfName.c_str());
      lv_obj_t *b = mkBtn(wizBody, LV_SYMBOL_EDIT "  Change name", [](lv_event_t *) {
        prompt("Your name on the mesh", "Keep it short, like DudeTested or KD9XYZ Desk.", R()->selfName, 31, "Save", [](const std::string &t) {
          if (t.empty()) return false;
          R()->setName(t);
          lv_async_call([](void *) { wizRender(); }, nullptr);
          return true;
        });
      }, nullptr, C_BTN, C_TEXT, F16);
      lv_obj_set_height(b, 48);
      break;
    }
    case 2: {
      mkLabel(wizBody, F24, C_TEXT, "2.  Your region");
      int cur = presetFor(r->freqMHz, r->bwKHz, r->sf), sug = suggestedPreset();
      wrapLabel(wizBody, F16, C_DIM,
                std::string("Everyone near you must use the same radio setting. Now: ") + (cur >= 0 ? PRESETS[cur].name : "custom settings") + ".");
      lv_obj_t *row = mkRow(wizBody);
      for (int i = 0; i < 4; i++) {
        std::string t = std::string(i == cur ? LV_SYMBOL_OK "  " : "") + PRESETS[i].name + (i == sug ? "\n(suggested for your map area)" : "");
        lv_obj_t *b = mkBtn(row, t.c_str(), [](lv_event_t *e) { applyPreset((int)(intptr_t)lv_event_get_user_data(e)); },
                            (void *)(intptr_t)i, i == cur ? T.chipOn : C_PANEL, C_TEXT, F16);
        lv_obj_set_size(b, 360, 64);
        if (i == sug) lv_obj_set_style_border_color(b, lv_color_hex(C_ACC), 0);
      }
      wrapLabel(wizBody, F12, C_DIM2, "Other presets are under MY RADIO > Region. If a local group told you different settings, use theirs.");
      break;
    }
    case 3: {
      mkLabel(wizBody, F24, C_TEXT, "3.  Show yourself on the map?");
      wrapLabel(wizBody, F16, C_DIM,
                "If you share, your rough location (your MESHtop area, about 1 km) goes out with your hello, so you appear on mesh maps.");
      wrapLabel(wizBody, F16, C_TEXT, r->locPolicy ? LV_SYMBOL_OK "  Sharing is on." : "Sharing is off.");
      lv_obj_t *row = mkRow(wizBody);
      lv_obj_t *a = mkBtn(row, "Share my area", [](lv_event_t *) {
        R()->setLocation(true, std::round(M->center.lat * 100) / 100, std::round(M->center.lon * 100) / 100);
        lv_async_call([](void *) { wizRender(); }, nullptr);
      }, nullptr, C_PANEL, C_TEXT, F16);
      lv_obj_set_size(a, 240, 56);
      lv_obj_t *b = mkBtn(row, "Don't share", [](lv_event_t *) {
        R()->setLocation(false, 0, 0);
        lv_async_call([](void *) { wizRender(); }, nullptr);
      }, nullptr, C_PANEL, C_TEXT, F16);
      lv_obj_set_size(b, 240, 56);
      break;
    }
    case 4: {
      mkLabel(wizBody, F24, C_TEXT, "4.  Join channels and say hello");
      std::string have;
      for (auto &c : r->channels) have += (have.empty() ? "" : ", ") + c.name;
      wrapLabel(wizBody, F16, C_DIM, "On your radio now: " + (have.empty() ? std::string("no channels") : have));
      lv_obj_t *row = mkRow(wizBody);
      lv_obj_t *pub = mkBtn(row, "Join Public", [](lv_event_t *) { R()->addPublicChannel(); }, nullptr, C_PANEL, C_TEXT, F16);
      lv_obj_set_height(pub, 48);
      lv_obj_t *hs = mkBtn(row, "# Join a hashtag channel", [](lv_event_t *) { joinHashtagPrompt(); }, nullptr, C_PANEL, C_TEXT, F16);
      lv_obj_set_height(hs, 48);
      lv_obj_t *hello = mkBtn(row, LV_SYMBOL_WIFI "  Say hello everywhere", [](lv_event_t *) { R()->sendAdvert(true); }, nullptr, C_ACC, T.onAcc, F16);
      lv_obj_set_height(hello, 48);
      wrapLabel(wizBody, F14, C_DIM2, "Saying hello lets others add you. Replies and new people show up under PEOPLE within a few minutes.");
      break;
    }
  }
}

// ================================================================ build + public API
void radioBuild(lv_obj_t *s, lv_obj_t *scr) {
  scrRoot = scr;
  convRows.clear();
  msgRows.clear();
  pRows.clear();
  chRows.clear();
  root = s;
  // left rail
  rail = mkBox(s, T.listBg);
  lv_obj_set_size(rail, RAIL_W, CH);
  lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(rail, 1, 0);
  lv_obj_set_style_border_color(rail, lv_color_hex(C_LINE), 0);
  static const char *icons[] = {LV_SYMBOL_ENVELOPE, LV_SYMBOL_CALL, LV_SYMBOL_LIST, LV_SYMBOL_SETTINGS};
  static const char *names[] = {"CHATS", "PEOPLE", "CHANNELS", "MY RADIO"};
  for (int i = 0; i < SEC_COUNT; i++) {
    lv_obj_t *b = lv_button_create(rail);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, 0, i * 76);
    lv_obj_set_size(b, RAIL_W - 1, 76);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_ACC), 0);
    lv_obj_t *ic = mkLabel(b, F24, C_DIM, icons[i]);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_t *lb = mkLabel(b, F12, C_DIM, names[i]);
    lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(b, [](lv_event_t *e) { showSection((int)(intptr_t)lv_event_get_user_data(e)); }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    railBtn[i] = b;
  }
  railDot = mkBox(rail, 0x5A6675);
  lv_obj_set_size(railDot, 10, 10);
  lv_obj_set_style_radius(railDot, 5, 0);
  lv_obj_align(railDot, LV_ALIGN_BOTTOM_LEFT, 10, -40);
  railStatus = mkLabel(rail, F12, C_DIM, "Off");
  lv_obj_set_width(railStatus, RAIL_W - 12);
  lv_label_set_long_mode(railStatus, LV_LABEL_LONG_WRAP);
  lv_obj_align(railStatus, LV_ALIGN_BOTTOM_LEFT, 8, -8);

  for (int i = 0; i < SEC_COUNT; i++) {
    secRoot[i] = mkBox(s, C_BG);
    lv_obj_set_pos(secRoot[i], RAIL_W, 0);
    lv_obj_set_size(secRoot[i], CW, CH);
  }
  buildChats(secRoot[SEC_CHATS]);
  buildPeople(secRoot[SEC_PEOPLE]);
  buildChannels(secRoot[SEC_CHANNELS]);
  buildMyRadio(secRoot[SEC_RADIO]);

  // not connected: setup instructions over the content
  offPanel = mkBox(s, C_BG);
  lv_obj_set_pos(offPanel, RAIL_W, 0);
  lv_obj_set_size(offPanel, CW, CH);
  lv_obj_set_flex_flow(offPanel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(offPanel, 24, 0);
  lv_obj_set_style_pad_row(offPanel, 12, 0);
  offTitle = mkLabel(offPanel, F24, C_TEXT, "");
  offText = wrapLabel(offPanel, F16, C_DIM, "");
  offBtn = mkBtn(offPanel, LV_SYMBOL_BLUETOOTH "  Find my radio", [](lv_event_t *) { P->radioEnable(true); }, nullptr, C_ACC, T.onAcc, F18);
  lv_obj_set_size(offBtn, 260, 54);
  offPinBtn = mkBtn(offPanel, LV_SYMBOL_EDIT "  Change PIN", [](lv_event_t *) { askRadioPin(); }, nullptr, C_PANEL, C_TEXT, F18);
  lv_obj_set_size(offPinBtn, 220, 54);
  lv_obj_add_flag(offPinBtn, LV_OBJ_FLAG_HIDDEN);

  buildOverlays(scr);
  buildWizard(scr);
  showSection(sec);
  for (auto &d : dirty) d = true;
}

static void renderRail() {
  Companion *r = R();
  bool on = P->radioEnabled();
  RadioState s = r ? r->state : R_OFF;
  uint32_t dot = s == R_READY ? 0x3DDC97 : s == R_ERROR ? 0xFF7A59 : (s == R_OFF || !on) ? 0x5A6675 : 0xFFB547;
  lv_obj_set_style_bg_color(railDot, lv_color_hex(dot), 0);
  std::string t = !on ? "Radio off" : s == R_READY ? (r->battMv > 0 ? "Radio on\n" + std::to_string(r->battMv / 1000) + "." + std::to_string(r->battMv % 1000 / 100) + " V" : "Radio on")
                                    : radioStateName(s);
  lv_label_set_text(railStatus, t.c_str());
  lv_obj_set_style_pad_left(railStatus, 16, 0);
  bool showOff = !on || s != R_READY;
  hide(offPanel, !showOff);
  if (showOff) {
    if (!on) {
      lv_label_set_text(offTitle, "Talk on the mesh from your desk");
      lv_label_set_text(offText,
                        "Connect MESHtop to your own MeshCore radio over Bluetooth and chat right here, no phone needed.\n\n"
                        "1.  Flash your radio (e.g. XIAO nRF52840 + Wio-SX1262) with MeshCore \"Companion Radio (BLE)\" from flasher.meshcore.io.\n"
                        "2.  Power it on near the panel.\n"
                        "3.  If the MeshCore phone app is connected to it, disconnect the app first.\n"
                        "4.  Tap Find my radio. The usual Bluetooth PIN is 123456.");
      hide(offBtn, false);
    } else {
      lv_label_set_text(offTitle, s == R_ERROR ? "Having trouble connecting" : "Connecting to your radio...");
      std::string body = (r ? r->status : std::string("Starting Bluetooth...")) +
                         "\n\nMake sure the radio is powered, close by, and not connected to the phone app. Radio PIN: " + P->radioPin() + ".";
      if (!P->radioName().empty()) body += "\nLooking for a radio named: " + P->radioName();
      lv_label_set_text(offText, body.c_str());
      hide(offBtn, true);
    }
    hide(offPinBtn, !on);
    // a failed pairing almost always means the PIN: ask for it once per failure
    static bool askedForThisFailure = false;
    bool pinFail = on && s == R_ERROR && r && r->status.find("PIN") != std::string::npos;
    if (pinFail && !askedForThisFailure && lv_obj_has_flag(promptPanel, LV_OBJ_FLAG_HIDDEN)) {
      askedForThisFailure = true;
      askRadioPin();
    }
    if (!pinFail) askedForThisFailure = false;
  } else {
    hide(offPinBtn, true);
  }
}

void radioDirty() {
  for (auto &d : dirty) d = true;
}
void radioToastCb(bool ok, const std::string &t) { radioToast(ok, t); }
void focusResultForTest(int which) { focusOn(which); }
void selectPersonForTest(const std::string &key) {
  showSection(SEC_PEOPLE);
  selKey = key;
  dirty[SEC_PEOPLE] = true;
}
void radioTick(bool visible) {
  // The trace runs in a pop-up, and the rest of this screen pauses while a pop-up is open (we're
  // called with visible=false then), so it is kept live here, before that check. Without this the
  // pop-up froze at "Waiting for it to come back... 0 s" even when the answer had arrived.
  static uint32_t lastTrace = 0;
  if (traceOpen && lv_tick_elaps(lastTrace) > 400) {
    lastTrace = lv_tick_get();
    refreshTrace();
  }
  if (!visible) return;
  static uint32_t last = 0;
  static bool wasReady = false;
  if (lv_tick_elaps(last) > 1000) {
    last = lv_tick_get();
    renderRail();
  }
  static uint32_t lastDisc = 0;
  if (rtMode == 1 && rtBack && !lv_obj_has_flag(rtBack, LV_OBJ_FLAG_HIDDEN) && lv_tick_elaps(lastDisc) > 500) {
    lastDisc = lv_tick_get();
    renderDiscovery(false);
  }
  static uint32_t lastPd = 0;  // the "Searching... Ns" count while a route is being found
  if (sec == SEC_PEOPLE && R() && R()->pathDisc.waiting && lv_tick_elaps(lastPd) > 1000) {
    lastPd = lv_tick_get();
    dirty[SEC_PEOPLE] = true;
  }
  bool rd = ready();
  if (rd && !wasReady) {
    for (auto &d : dirty) d = true;
    renderRail();
    if (!P->radioSetupDone() && R()->selfInfoValid) runWizard();
  }
  wasReady = rd;
  if (!rd) return;
  if (dirty[sec] && lv_obj_has_flag(promptPanel, LV_OBJ_FLAG_HIDDEN)) {
    dirty[sec] = false;
    switch (sec) {
      case SEC_CHATS: renderChats(); break;
      case SEC_PEOPLE: renderPeople(); break;
      case SEC_CHANNELS: renderChannels(); break;
      case SEC_RADIO: renderMyRadio(); break;
    }
  }
  if (!lv_obj_has_flag(wiz, LV_OBJ_FLAG_HIDDEN) && dirty[SEC_RADIO]) {  // keep the wizard in step with the radio
    dirty[SEC_RADIO] = false;
    wizRender();
  }
}
void radioOpenConv(const std::string &id) {
  showSection(SEC_CHATS);
  selectConv(id);
}
void radioOpenConvFor(const std::string &id) { radioOpenConv(id); }
void showDiscovery() {
  showSection(SEC_RADIO);
  startDiscovery();
}
void discoveryToMap() { discoveryOnMap(); }
void routeToMap(size_t i) {
  if (i >= rtStops.size()) return;
  hide(rtBack, true);
  showScreen(S_MAP);
  mapShowPath(rtTitles[i], rtStops[i], false);
}
int radioUnread() {
  Companion *r = R();
  if (!r) return 0;
  int n = 0;
  for (auto &c : r->convs) {
    if (c.isChannel && M->isMuted(c.title)) continue;  // muted channels don't call for attention
    if (curScreen == S_RADIO && sec == SEC_CHATS && c.id == curConv) continue;
    n += c.unread;
  }
  return n;
}
bool radioViewing(const std::string &convId) { return curScreen == S_RADIO && sec == SEC_CHATS && curConv == convId; }
bool radioComposeOpen() {
  return (promptPanel && !lv_obj_has_flag(promptPanel, LV_OBJ_FLAG_HIDDEN)) || (wiz && !lv_obj_has_flag(wiz, LV_OBJ_FLAG_HIDDEN)) ||
         (modalBack && !lv_obj_has_flag(modalBack, LV_OBJ_FLAG_HIDDEN));
}
void showTraceFor(const std::string &) {
  showSection(SEC_PEOPLE);
  modal("Route trace", traceText(), "Close", nullptr, false, nullptr);
  traceOpen = true;
  refreshTrace();
}

std::string traceTextNow() { return traceText(); }
void tidyForScreenshot() {  // simulator: close pop-ups and sheets, unpin any route on the map
  if (modalBack) hide(modalBack, true);
  if (promptPanel) hide(promptPanel, true);
  traceOpen = false;
  if (rtBack) hide(rtBack, true);
  mapClearPath();
}
void startTraceFor(const std::string &key) {
  showSection(SEC_PEOPLE);
  startTrace(key);
}
std::string popupButtons() {  // simulator: the pop-up's visible buttons, left to right
  std::string b;
  for (lv_obj_t *o : {modalCancel, modalAlt, modalOk})
    if (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) b += std::string(b.empty() ? "" : "|") + lv_label_get_text(lv_obj_get_child(o, 0));
  return b;
}
void popupPress(int which) {  // simulator: 0 left, 1 middle, 2 right (OK)
  lv_obj_t *o = which == 0 ? modalCancel : which == 1 ? modalAlt : modalOk;
  if (o && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_send_event(o, LV_EVENT_CLICKED, nullptr);
}
std::string composeTextNow() { return promptPanel && !lv_obj_has_flag(promptPanel, LV_OBJ_FLAG_HIDDEN) ? lv_textarea_get_text(promptTa) : ""; }
std::string popupTextNow() { return modalBack && !lv_obj_has_flag(modalBack, LV_OBJ_FLAG_HIDDEN) ? lv_label_get_text(modalText) : ""; }

void showHeardFor(const std::string &convId, size_t msgIndex) {
  curConv = convId;
  showSection(SEC_CHATS);
  showHeard(msgIndex);
}
void showRouteFor(const std::string &convId, size_t msgIndex) {
  curConv = convId;
  showSection(SEC_CHATS);
  showRoute(msgIndex);
}

void radioShowSection(const char *name) {
  static const char *n[] = {"chats", "people", "channels", "radio"};
  for (int i = 0; i < SEC_COUNT; i++)
    if (!strcmp(name, n[i])) showSection(i);
  if (!strcmp(name, "wizard")) runWizard();
}

}  // namespace ui
