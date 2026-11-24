# Desktop simulator build (Linux). Run from the project root:  make -C sim -j8 && ./sim/build/scopething-sim
ROOT := ..
LVGL := $(ROOT)/.pio/libdeps/crowpanel7/lvgl
BUILD := build
CFLAGS := -O2 -g -DLV_CONF_INCLUDE_SIMPLE -I$(ROOT)/include -I$(LVGL) -I$(ROOT)/src
CXXFLAGS := $(CFLAGS) -std=c++20

LV_SRC := $(shell find $(LVGL)/src -name '*.c')
LV_OBJ := $(patsubst $(LVGL)/%.c,$(BUILD)/lvgl/%.o,$(LV_SRC))
APP_SRC := extmem_host.cpp $(ROOT)/src/core/model.cpp $(ROOT)/src/core/companion.cpp $(ROOT)/src/ui/ui_radio.cpp $(ROOT)/src/app/demo.cpp $(ROOT)/src/app/radio_bridge.cpp $(ROOT)/src/ui/ui.cpp $(ROOT)/src/ui/ui_map.cpp sim_main.cpp
APP_OBJ := $(patsubst %.cpp,$(BUILD)/app/%.o,$(notdir $(APP_SRC)))

all: $(BUILD)/scopething-sim

$(BUILD)/lvgl/%.o: $(LVGL)/%.c $(ROOT)/include/lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/liblvgl.a: $(LV_OBJ)
	@ar rcs $@ $^

vpath %.cpp $(ROOT)/src/core $(ROOT)/src/app $(ROOT)/src/ui .
$(BUILD)/app/%.o: %.cpp $(wildcard $(ROOT)/src/*/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/scopething-sim: $(APP_OBJ) $(BUILD)/liblvgl.a
	$(CXX) -o $@ $(APP_OBJ) $(BUILD)/liblvgl.a -lm

# offline-map tile reading (PNG/JPEG, PMTiles packs) against the reference PMTiles library's output
$(BUILD)/tiles-test: tiletest/tiles_test.cpp $(BUILD)/app/tiles.o $(BUILD)/app/extmem_host.o $(BUILD)/liblvgl.a
	$(CXX) $(CXXFLAGS) -o $@ tiletest/tiles_test.cpp $(BUILD)/app/tiles.o $(BUILD)/app/extmem_host.o $(BUILD)/liblvgl.a -lm
tiles-test: $(BUILD)/tiles-test
	cd tiletest && test -f out/ids.txt || python3 make_fixtures.py
	./$(BUILD)/tiles-test

clean:
	rm -rf $(BUILD)

# MESHtop Desktop for Windows (cross-compiled with MinGW-w64): make -C sim windows
WIN := build-win
WCC := x86_64-w64-mingw32-gcc-posix
WCXX := x86_64-w64-mingw32-g++-posix
WFLAGS := -O2 -D_USE_MATH_DEFINES -DLV_CONF_INCLUDE_SIMPLE -I$(ROOT)/include -I$(LVGL) -I$(ROOT)/src
WLV_OBJ := $(patsubst $(LVGL)/%.c,$(WIN)/lvgl/%.o,$(LV_SRC))
$(WIN)/lvgl/%.o: $(LVGL)/%.c $(ROOT)/include/lv_conf.h
	@mkdir -p $(dir $@)
	@$(WCC) $(WFLAGS) -c $< -o $@
$(WIN)/liblvgl.a: $(WLV_OBJ)
	@x86_64-w64-mingw32-ar rcs $@ $^
windows: $(WIN)/liblvgl.a
	$(WCXX) $(WFLAGS) -std=c++20 -DMESHTOP_DESKTOP -D_POSIX_THREAD_SAFE_FUNCTIONS -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
	  $(APP_SRC) $(ROOT)/src/core/tiles.cpp $(WIN)/liblvgl.a -static -static-libgcc -static-libstdc++ -mwindows \
	  -lwinhttp -lgdi32 -lwinmm -o $(WIN)/MESHtop-Desktop.exe

# the same app as a headless check on Linux: ./sim/build/meshtop-desktop maps=<folder> center=lat,lon zoom=13 offline=1
desktop-check: $(BUILD)/liblvgl.a
	$(CXX) $(CXXFLAGS) -DMESHTOP_DESKTOP $(APP_SRC) $(ROOT)/src/core/tiles.cpp $(BUILD)/liblvgl.a -lm -lpthread -o $(BUILD)/meshtop-desktop
