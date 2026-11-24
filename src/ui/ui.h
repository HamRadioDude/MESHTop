// ScopeThing panel UI (LVGL 9.1). Platform-agnostic: runs on the CrowPanel and in the desktop simulator.
#pragma once
#include <string>
#include <vector>

#include "app/platform.h"
#include "core/companion.h"
#include "core/model.h"

namespace ui {

struct WifiItem {
  std::string ssid;
  int rssi = 0;
  bool secure = true;
};
void wifiScanResult(const std::vector<WifiItem> &items);
void wifiStatus(const std::string &text, bool ok, bool trying);

void begin(st::Model *m, st::Platform *p);

// Wire these to the model callbacks / network events (UI thread only)
void onRoute(const st::Route &r);
void onAlert(const st::Alert &a);
void onChatMsg(const std::string &channel, const st::ChatMsg &m);
void mapChanged();       // new tiles or directory/My Nodes changes: re-bake the map
void configChanged();    // area / radius / style changed: recenter
void channelsChanged();  // channel list arrived
void historyChanged(const std::string &channel);
void myNodesChanged();
void observersChanged();
void searchChanged();    // server search results merged into the directory
void showScreenByName(const char *name);  // simulator / screenshots
void openFind(const char *query);
void openAlerts();
bool tabShown(int screen);  // simulator: is this tab on the bar (0 MAP, 1 FEED, 2 CHATTER, 3 MY NODES, 4 MESH, 5 STATUS)
void showDataSource();      // the data source choices (STATUS > Data > Change)
void showDimChoice();      // screen dimming choices (STATUS > Dimming)
void closeChooser();       // simulator: close the data source / dimming chooser
void selectPersonForTest(const std::string &key);  // simulator: MESH > People, this contact selected
void focusResultForTest(int which);  // simulator: as if Neighbours (1), Command (2) or Find a route (3) was tapped
void goIdleNow();  // simulator: act as if untouched for the idle time (back to the map, dimmed)
bool isIdle();
void toggleTrafficPanel();  // simulator: fold or unfold the map's live traffic panel
bool trafficPanelFolded();
void showSetup(bool on, const char *ssid);
void themeChanged();
void radioChanged();
void radioResult(bool ok, const std::string &text);  // toast from the companion client
void showRadioSection(const char *name);
void radioOpenConvFor(const std::string &convId);  // simulator: open a radio conversation
void showDiscovery();          // simulator: run "Who can hear me?" and open its sheet
void discoveryToMap();         // simulator: pin its answers on the map
void routeToMap(size_t card);  // simulator: pin card N of the open route sheet on the map
// open the "heard by" details for a message you sent (used by the simulator)
void showHeardFor(const std::string &convId, size_t msgIndex);
void showRouteFor(const std::string &convId, size_t msgIndex);  // how someone else's message reached you
// show the trace result panel (used by the simulator; the trace itself is run by the radio client)
void showTraceFor(const std::string &key);
std::string traceTextNow();  // simulator: the trace panel's text as it stands
void tidyForScreenshot();    // simulator: close pop-ups and sheets, unpin any route on the map
void startTraceFor(const std::string &key);  // simulator: start a trace as the Trace route button does
std::string popupTextNow();                  // simulator: what the pop-up shows ("" when closed)
std::string popupButtons();                  // simulator: the pop-up's visible buttons, left to right ("Close|Copy|Send again")
void popupPress(int which);                  // simulator: press the pop-up's left (0), middle (1) or right (2) button
std::string composeTextNow();                // simulator: text in the message box ("" when closed)  // companion radio state / messages changed
void onRadioMessage(const st::Conv &c, const st::RMsg &m);  // settings changed the theme  // first-run Wi-Fi setup instructions

}  // namespace ui
