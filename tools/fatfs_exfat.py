# MESHtop: read exFAT memory cards (most cards over 32 GB come formatted exFAT), like WadaMesh does.
#
# ESP-IDF builds FatFs, the card's file system driver, with exFAT switched off, and has no menuconfig
# option for it. This switches it on in FatFs's config header (FF_FS_EXFAT 1) in two places:
#   framework-espidf                   the ESP-IDF source the panel's libraries are compiled from
#   framework-arduinoespressif32-libs  the copy of the headers the rest of the firmware is compiled
#                                      against (so both agree on FatFs's structures)
# It runs before every build and changes nothing once both are done.
#
# The libraries are only recompiled when custom_sdkconfig in platformio.ini changes (pioarduino keeps
# its hash on the first line of sdkconfig.defaults). After a fresh PlatformIO install that happens on
# the first build anyway. If you ever need to force it, delete sdkconfig.defaults and build again.
# Check with:  strings <libs>/esp32s3/lib/libfatfs.a | grep EXFAT   (no output = exFAT is off)
Import("env")  # noqa: F821  (PlatformIO provides it)

import re
from pathlib import Path

platform = env.PioPlatform()  # noqa: F821
mcu = env.BoardConfig().get("build.mcu", "esp32s3")  # noqa: F821
ON = re.compile(r"^#define\s+FF_FS_EXFAT\s+1\b", re.M)
OFF = re.compile(r"^#define\s+FF_FS_EXFAT\s+0\b", re.M)


def switch_on(package, relative):
    folder = platform.get_package_dir(package)
    if not folder:
        return None  # not installed (yet)
    f = Path(folder) / relative
    if not f.exists():
        return None
    text = f.read_text()
    if OFF.search(text):
        text = OFF.sub("#define FF_FS_EXFAT\t\t1", text)
        f.write_text(text)
        print(f"exFAT: switched on in {f}")
    return bool(ON.search(text))


src = switch_on("framework-espidf", "components/fatfs/src/ffconf.h")
hdr = switch_on("framework-arduinoespressif32-libs", f"{mcu}/include/fatfs/src/ffconf.h")
if src is False or hdr is False:
    print("exFAT: WARNING, couldn't switch it on (FatFs's ffconf.h has changed?). FAT32 cards still work.")
