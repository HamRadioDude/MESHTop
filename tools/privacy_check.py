"""Build-time privacy guard: nothing from the user's MeshCore radio may be sent to CoreScope or any
other internet service. Fails the build if
  1. radio-side code (src/core/companion.*, src/device/ble_radio.*) uses the network layer beyond
     the read-only wifiBusy() flag, or any HTTP/WebSocket client, or
  2. the network layer (src/device/net.cpp) gains an upload-style call (POST/PUT/PATCH/DELETE,
     WebSocket sends) or references the radio.
Runs automatically before every PlatformIO build (extra_scripts) and can be run by hand:
  python3 tools/privacy_check.py
"""
import os, re, sys

try:  # run by hand
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
except NameError:  # run by PlatformIO (SCons has no __file__)
    Import("env")  # noqa: F821
    ROOT = env.subst("$PROJECT_DIR")  # noqa: F821


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


problems = []
radio_files = ["src/core/companion.cpp", "src/core/companion.h", "src/device/ble_radio.cpp", "src/device/ble_radio.h"]
for rel in radio_files:
    code = strip_comments(read(rel))
    for m in re.finditer(r"net::(\w+)", code):
        if m.group(1) != "wifiBusy":
            problems.append(f"{rel}: radio code calls net::{m.group(1)} (only net::wifiBusy() is allowed)")
    for bad in ["HTTPClient", "WebSocketsClient", "NetworkClient", "WiFiClient", "esp_http_client"]:
        if bad in code:
            problems.append(f"{rel}: radio code uses {bad}")

net = strip_comments(read("src/device/net.cpp"))
for pat, why in [
    (r"\.(POST|PUT|PATCH)\s*\(", "HTTP upload call"),
    (r"sendRequest\s*\(", "custom HTTP request"),
    (r"\bws\.(sendTXT|sendBIN|sendPing)\s*\(", "WebSocket send"),
    (r"bleradio::|Companion\b|companion\.h|ble_radio\.h", "network layer references the radio"),
]:
    for m in re.finditer(pat, net):
        line = net.count("\n", 0, m.start()) + 1
        problems.append(f"src/device/net.cpp:~{line}: {why} ({m.group(0).strip()})")

# Radio-only mode: CoreScope must be refused where every request to it starts (REST and live feed),
# so a radio-only panel can never contact it whatever else changes.
for fn in ("getJson", "startWs"):
    m = re.search(r"static\s+[\w:<>&\*\s]+\b" + fn + r"\s*\([^)]*\)\s*\{", net)
    head = net[m.end(): m.end() + 200] if m else ""
    if "coreScopeOff()" not in head:
        problems.append(f"src/device/net.cpp: {fn}() must refuse CoreScope first thing when it is switched off (radio only)")

if problems:
    print("PRIVACY CHECK FAILED - radio data must never reach the internet:")
    for p in problems:
        print("  - " + p)
    sys.exit(1)
print("privacy check: OK (radio code has no network path; CoreScope access is read-only and off in radio-only mode)")

