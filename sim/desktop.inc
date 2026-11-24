// MESHtop Desktop: the simulator as a window you can click and screen-record (Windows).
// Real map tiles: map packs and tile folders from a memory card's maps folder (found by itself), and
// OpenStreetMap tiles downloaded while online, saved in maps/osm the same way the panel does.
// The traffic is demo traffic and the radio is the simulator's pretend radio.
//
// Included at the end of sim_main.cpp when MESHTOP_DESKTOP is defined. On Linux it builds as a
// headless check: it runs for a few seconds and writes a screenshot (for testing the tiles).

#include "core/tiles.h"

#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <mmsystem.h>
#endif

namespace dt {

static std::vector<std::string> roots;        // folders that hold maps/ (or tiles/)
static std::vector<st::PmTiles *> packs;
static std::vector<FILE *> packFiles;
static std::string saveDir;                    // downloads go here (<root>/maps/osm)
static bool online = true;
static int startZoom = 0;

static uint64_t key(const std::string &style, int z, int x, int y) {
  uint64_t s = style == "light" ? 1 : 0;
  return (s << 60) | ((uint64_t)z << 52) | ((uint64_t)x << 26) | (uint64_t)y;
}

// ---- files
static bool exists(const std::string &p) {
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) return false;
  fclose(f);
  return true;
}
static bool readFile(const std::string &p, std::vector<uint8_t> &out) {
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 2000000) {
    fclose(f);
    return false;
  }
  out.resize((size_t)n);
  bool ok = fread(out.data(), 1, (size_t)n, f) == (size_t)n;
  fclose(f);
  return ok;
}
static void makeDirs(const std::string &path) {  // every folder above a file
  for (size_t i = 1; i < path.size(); i++)
    if (path[i] == '/' || path[i] == '\\') {
      std::string d = path.substr(0, i);
#ifdef _WIN32
      CreateDirectoryA(d.c_str(), nullptr);
#else
      std::string cmd = "mkdir -p '" + d + "'";
      (void)!system(cmd.c_str());
#endif
    }
}
static void writeFile(const std::string &p, const std::vector<uint8_t> &data) {
  makeDirs(p);
  FILE *f = fopen(p.c_str(), "wb");
  if (!f) return;
  fwrite(data.data(), 1, data.size(), f);
  fclose(f);
}
static std::vector<std::string> listDirs(const std::string &dir) {
  std::vector<std::string> out;
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') out.push_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  std::string cmd = "ls -1 '" + dir + "' 2>/dev/null";
  FILE *p = popen(cmd.c_str(), "r");
  if (!p) return out;
  char b[512];
  while (fgets(b, sizeof b, p)) {
    std::string n = b;
    while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
    if (!n.empty()) out.push_back(n);
  }
  pclose(p);
#endif
  return out;
}

// ---- where the maps are: the folder next to the program, then any drive with a maps folder
static void addRoot(const std::string &r) {
  for (auto &x : roots)
    if (x == r) return;
  roots.push_back(r);
}
static void findRoots(const std::string &exeDir, const std::string &given) {
  if (!given.empty()) addRoot(given);
  if (exists(exeDir + "/maps/osm") || !listDirs(exeDir + "/maps").empty()) addRoot(exeDir);
#ifdef _WIN32
  DWORD drives = GetLogicalDrives();
  for (char d = 'D'; d <= 'Z'; d++)
    if (drives & (1u << (d - 'A'))) {
      std::string r = std::string(1, d) + ":";
      UINT t = GetDriveTypeA((r + "\\").c_str());
      if (t != DRIVE_REMOVABLE && t != DRIVE_FIXED) continue;
      if (!listDirs(r + "\\maps").empty() || !listDirs(r + "\\tiles").empty()) addRoot(r);
    }
#endif
  if (roots.empty()) addRoot(exeDir);  // nothing found: keep downloads next to the program
  saveDir = roots.front() + "/maps/osm";
}
static void openPacks() {
  for (auto &r : roots)
    for (auto &name : listDirs(r + "/maps")) {
      if (name == "osm") continue;
      std::string path = r + "/maps/" + name + "/" + name + ".pmtiles";
      FILE *f = fopen(path.c_str(), "rb");
      if (!f) continue;
      auto *pm = new st::PmTiles();
      pm->name = name;
      pm->read = [f](uint64_t off, uint32_t len, uint8_t *dst) {
#ifdef _WIN32
        if (_fseeki64(f, (long long)off, SEEK_SET)) return false;
#else
        if (fseeko(f, (off_t)off, SEEK_SET)) return false;
#endif
        return fread(dst, 1, len, f) == len;
      };
      if (pm->open()) {
        packs.push_back(pm);
        packFiles.push_back(f);
        printf("[maps] pack %s (zoom %d-%d) in %s\n", name.c_str(), pm->minZoom, pm->maxZoom, r.c_str());
      } else {
        printf("[maps] %s: %s\n", path.c_str(), pm->error.c_str());
        delete pm;
        fclose(f);
      }
    }
}
static bool localTile(int z, int x, int y, std::vector<uint8_t> &out) {
  for (auto *pm : packs)
    if (pm->get(z, (uint32_t)x, (uint32_t)y, out)) return true;
  char p[96];
  for (auto &r : roots) {
    snprintf(p, sizeof p, "/maps/osm/%d/%d/%d.png", z, x, y);
    if (readFile(r + p, out)) return true;
    snprintf(p, sizeof p, "/tiles/%d/%d/%d.jpg", z, x, y);
    if (readFile(r + p, out)) return true;
    snprintf(p, sizeof p, "/tiles/%d/%d/%d.png", z, x, y);
    if (readFile(r + p, out)) return true;
  }
  return false;
}

// ---- PNG/JPEG -> RGB565, darkened for night the way the panel does it
static bool decode(const std::vector<uint8_t> &data, bool invert, std::vector<uint16_t> &pix) {
  static std::vector<uint8_t> rgb(256 * 256 * 3);
  if (data.empty() || !st::decodeTileRGB(data.data(), data.size(), rgb.data())) return false;
  if (invert && st::tileBrightness(rgb.data()) < 100) invert = false;  // drawn dark already
  pix.resize(256 * 256);
  for (int i = 0; i < 256 * 256; i++) {
    int R = rgb[i * 3], G = rgb[i * 3 + 1], B = rgb[i * 3 + 2];
    if (invert) {
      int mx = std::max(R, std::max(G, B)), mn = std::min(R, std::min(G, B));
      int d = 255 - mx - mn;
      R = std::min(255, ((R + d) * 150 >> 8) + 4);
      G = std::min(255, ((G + d) * 155 >> 8) + 6);
      B = std::min(255, ((B + d) * 170 >> 8) + 10);
    }
    pix[i] = (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));
  }
  return true;
}

// ---- the tile cache, and downloads on a worker thread
struct Want {
  int z, x, y;
  std::string style;
};
struct Got {
  Want w;
  std::vector<uint8_t> data;  // empty = failed
};
static std::unordered_map<uint64_t, std::vector<uint16_t>> cache;
static std::unordered_set<uint64_t> failed, pending;
static std::mutex mtx;
static std::condition_variable cv;
static std::deque<Want> dlQueue;
static std::deque<Got> dlDone;

static const uint16_t *tile(int z, int x, int y, const std::string &style, bool ask) {
  uint64_t k = key(style, z, x, y);
  auto it = cache.find(k);
  if (it != cache.end()) return it->second.data();
  if (!ask || failed.count(k) || pending.count(k)) return nullptr;
  std::vector<uint8_t> data;
  if (localTile(z, x, y, data)) {
    std::vector<uint16_t> pix;
    if (decode(data, style == "dark", pix)) return (cache[k] = std::move(pix)).data();
    failed.insert(k);
    return nullptr;
  }
  if (!online) {
    failed.insert(k);
    return nullptr;
  }
  pending.insert(k);
  {
    std::lock_guard<std::mutex> g(mtx);
    dlQueue.push_back({z, x, y, style});
  }
  cv.notify_one();
  return nullptr;
}
static bool tileFailed(int z, int x, int y, const std::string &style) { return failed.count(key(style, z, x, y)) > 0; }

#ifdef _WIN32
static bool httpGet(const std::wstring &host, const std::wstring &path, std::vector<uint8_t> &out) {
  static HINTERNET ses = WinHttpOpen(L"MESHtop-Desktop (+https://github.com/HamRadioDude/MESHTop)",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!ses) return false;
  bool ok = false;
  HINTERNET con = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status == 200) {
      DWORD avail = 0;
      while (WinHttpQueryDataAvailable(req, &avail) && avail) {
        size_t at = out.size();
        out.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(req, out.data() + at, avail, &got)) break;
        out.resize(at + got);
      }
      ok = !out.empty();
    }
  }
  if (req) WinHttpCloseHandle(req);
  if (con) WinHttpCloseHandle(con);
  return ok;
}
#endif

static void downloader() {
  for (;;) {
    Want w;
    {
      std::unique_lock<std::mutex> g(mtx);
      cv.wait(g, [] { return !dlQueue.empty(); });
      w = dlQueue.back();  // newest first: the view you're looking at now
      dlQueue.pop_back();
    }
    Got got{w, {}};
#ifdef _WIN32
    wchar_t path[64];
    swprintf(path, 64, L"/%d/%d/%d.png", w.z, w.x, w.y);
    if (!httpGet(L"tile.openstreetmap.org", path, got.data)) got.data.clear();
#endif
    std::lock_guard<std::mutex> g(mtx);
    dlDone.push_back(std::move(got));
  }
}

// main thread: take finished downloads; keep them on the card like the panel does
static void poll() {
  std::deque<Got> done;
  {
    std::lock_guard<std::mutex> g(mtx);
    done.swap(dlDone);
  }
  bool changed = false;
  for (auto &d : done) {
    uint64_t k = key(d.w.style, d.w.z, d.w.x, d.w.y);
    pending.erase(k);
    std::vector<uint16_t> pix;
    if (!d.data.empty() && decode(d.data, d.w.style == "dark", pix)) {
      cache[k] = std::move(pix);
      char p[64];
      snprintf(p, sizeof p, "/%d/%d/%d.png", d.w.z, d.w.x, d.w.y);
      if (!exists(saveDir + p)) writeFile(saveDir + p, d.data);
    } else failed.insert(k);
    changed = true;
  }
  if (cache.size() > 600) cache.clear();  // ~75 MB; the map asks again for what's on screen
  if (failed.size() > 4000) failed.clear();
  if (changed) ui::mapChanged();
}

// ---- settings: meshtop-desktop.txt next to the program, then the command line (key=value)
static std::string mapsGiven;
static void setting(const std::string &k, const std::string &v) {
  if (k == "center") {
    double la, lo;
    if (sscanf(v.c_str(), "%lf , %lf", &la, &lo) == 2 || sscanf(v.c_str(), "%lf %lf", &la, &lo) == 2) model.center = {la, lo};
  } else if (k == "area") model.areaLabel = v;
  else if (k == "radius") model.cfg.radiusMi = std::max(5, std::min(300, atoi(v.c_str())));
  else if (k == "maps") mapsGiven = v;
  else if (k == "offline") online = !(v == "1" || v == "yes" || v == "true");
  else if (k == "zoom") startZoom = atoi(v.c_str());
}
static void parseLine(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
  if (line.empty() || line[0] == '#') return;
  size_t e = line.find('=');
  if (e == std::string::npos) return;
  std::string k = line.substr(0, e), v = line.substr(e + 1);
  while (!k.empty() && k.back() == ' ') k.pop_back();
  while (!v.empty() && v[0] == ' ') v.erase(0, 1);
  setting(k, v);
}
static std::string exeDir(const char *argv0) {
#ifdef _WIN32
  char b[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, b, MAX_PATH);
  std::string p(b, n);
#else
  std::string p = argv0 ? argv0 : ".";
#endif
  size_t s = p.find_last_of("/\\");
  return s == std::string::npos ? "." : p.substr(0, s);
}

}  // namespace dt

static void desktopConfig(int argc, char **argv) {
  std::ifstream f(dt::exeDir(argc > 0 ? argv[0] : nullptr) + "/meshtop-desktop.txt");
  std::string line;
  while (std::getline(f, line)) dt::parseLine(line);
  for (int i = 1; i < argc; i++) dt::parseLine(argv[i]);
}

// ---- the screen as 32-bit pixels for Windows, and screenshots
static void fbToBgra(std::vector<uint32_t> &out) {
  out.resize(800 * 480);
  for (int i = 0; i < 800 * 480; i++) {
    uint16_t c = fb[i];
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[i] = ((r * 527 + 23) >> 6) << 16 | ((g * 259 + 33) >> 6) << 8 | ((b * 527 + 23) >> 6);
  }
}
static std::string saveScreenshot(const std::string &dir) {  // a 24-bit BMP, opens anywhere
  std::vector<uint32_t> px;
  fbToBgra(px);
  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  char name[64];
  strftime(name, sizeof name, "meshtop-%Y%m%d-%H%M%S.bmp", &lt);
  std::string path = dir + "/" + name;
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return "";
  const int W = 800, H = 480, row = W * 3, size = 54 + row * H;
  uint8_t h[54] = {'B', 'M'};
  auto le32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) h[at + i] = (uint8_t)(v >> (8 * i)); };
  le32(2, size);
  le32(10, 54);
  le32(14, 40);
  le32(18, W);
  le32(22, H);
  h[26] = 1;
  h[28] = 24;
  le32(34, row * H);
  fwrite(h, 1, 54, f);
  std::vector<uint8_t> line(row);
  for (int y = H - 1; y >= 0; y--) {
    for (int x = 0; x < W; x++) {
      uint32_t p = px[y * W + x];
      line[x * 3] = p & 255;
      line[x * 3 + 1] = (p >> 8) & 255;
      line[x * 3 + 2] = (p >> 16) & 255;
    }
    fwrite(line.data(), 1, row, f);
  }
  fclose(f);
  return path;
}

static void desktopStart(int argc, char **argv) {
  std::string dir = dt::exeDir(argc > 0 ? argv[0] : nullptr);
  dt::findRoots(dir, dt::mapsGiven);
  dt::openPacks();
  printf("[maps] looking in:");
  for (auto &r : dt::roots) printf(" %s", r.c_str());
  printf("; downloads saved in %s%s\n", dt::saveDir.c_str(), dt::online ? "" : " (offline: no downloads)");
  dtTile = dt::tile;
  dtFailed = dt::tileFailed;
  std::thread(dt::downloader).detach();
  ui::mapRecenter();
  if (dt::startZoom) ui::mapSetView(model.center.lat, model.center.lon, dt::startZoom);
}

#ifdef _WIN32
// ================================================================ the window (Windows)
static int scale = 1;
static HWND win;
static std::vector<uint32_t> bgra;
static std::string shotDir;

static void fitWindow() {
  RECT r = {0, 0, 800 * scale, 480 * scale};
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRect(&r, style, FALSE);
  SetWindowPos(win, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}
static void paint(HDC dc) {
  fbToBgra(bgra);
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = 800;
  bi.bmiHeader.biHeight = -480;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(dc, COLORONCOLOR);  // whole-number sizes: crisp pixels
  SetBrushOrgEx(dc, 0, 0, nullptr);
  StretchDIBits(dc, 0, 0, 800 * scale, 480 * scale, 0, 0, 800, 480, bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}
static void setTitle(const char *extra) {
  std::string t = "MESHtop Desktop " + plat.version() + "   (1 / 2 / 3: size,  S: screenshot)";
  if (extra && *extra) t += std::string("   ") + extra;
  SetWindowTextA(win, t.c_str());
}
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_LBUTTONDOWN:
      SetCapture(h);
      mouseDown = true;
      [[fallthrough]];
    case WM_MOUSEMOVE:
      mouseX = std::max(0, std::min(799, (int)(short)LOWORD(l) / scale));
      mouseY = std::max(0, std::min(479, (int)(short)HIWORD(l) / scale));
      return 0;
    case WM_LBUTTONUP:
      mouseDown = false;
      ReleaseCapture();
      return 0;
    case WM_KEYDOWN:
      if (w >= '1' && w <= '3') {
        scale = (int)(w - '0');
        fitWindow();
      } else if (w == 'S') {
        std::string p = saveScreenshot(shotDir);
        setTitle(p.empty() ? "(couldn't save the screenshot)" : ("saved " + p.substr(p.find_last_of("/\\") + 1)).c_str());
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      paint(dc);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

static int desktopMain(int argc, char **argv) {
  SetProcessDPIAware();  // exact pixels, not stretched by Windows display scaling
  desktopStart(argc, argv);
  shotDir = dt::exeDir(nullptr);
  HINSTANCE inst = GetModuleHandleA(nullptr);
  WNDCLASSA wc = {};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
  wc.lpszClassName = "MESHtopDesktop";
  RegisterClassA(&wc);
  win = CreateWindowA("MESHtopDesktop", "MESHtop Desktop", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                      800, 480, nullptr, nullptr, inst, nullptr);
  fitWindow();
  setTitle("");
  ShowWindow(win, SW_SHOW);
  timeBeginPeriod(1);
  ULONGLONG last = GetTickCount64();
  uint32_t carry = 0;
  for (;;) {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) ExitProcess(0);  // don't wait for the download thread
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    ULONGLONG now = GetTickCount64();
    carry += (uint32_t)std::min<ULONGLONG>(now - last, 200);
    last = now;
    if (carry >= 5) {
      uint32_t step = carry - carry % 5;
      carry -= step;
      run(step);  // the UI, demo traffic and pretend radio, in real time
    }
    dt::poll();
    HDC dc = GetDC(win);
    paint(dc);
    ReleaseDC(win, dc);
    Sleep(10);
  }
}
#else
// ================================================================ headless check (Linux)
static int desktopMain(int argc, char **argv) {
  desktopStart(argc, argv);
  for (int i = 0; i < 40; i++) {
    run(250);
    dt::poll();
  }
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(nullptr);
  std::string p = saveScreenshot(".");
  printf("[desktop] screenshot %s, %zu tiles cached, %zu failed\n", p.c_str(), dt::cache.size(), dt::failed.size());
  fflush(stdout);
  std::_Exit(0);  // don't wait for the download thread
}
#endif
