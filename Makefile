CC ?= cc
BUILD_DIR ?= build
CORE := $(BUILD_DIR)/karashi_libretro.so

CPPFLAGS += -Isrc
CFLAGS ?= -O3
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden

LDFLAGS += -shared -Wl,--no-undefined -Wl,--version-script=link.T
PGO_DIR ?= $(abspath $(BUILD_DIR))/pgo

ifeq ($(PGO),generate)
CFLAGS += -fprofile-generate -fprofile-update=single -fprofile-dir=$(PGO_DIR)
LDFLAGS += -fprofile-generate
endif
ifeq ($(PGO),use)
CFLAGS += -fprofile-use -fprofile-partial-training -fprofile-dir=$(PGO_DIR) -Wno-missing-profile -Wno-error=coverage-mismatch
endif

HARDWARE := src/content.c src/scheduler.c src/state.c \
	src/z80.c src/m68k.c src/sh2.c src/mars.c \
	src/eeprom.c src/eeprom93.c src/spi_eeprom.c \
	src/tms9918.c src/smsvdp.c src/mdvdp.c \
	src/sn76489.c src/ym2413.c src/ym2612.c \
	src/sg1000.c src/sms.c src/paprium.c src/md.c

SOURCES := src/libretro.c $(HARDWARE)
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

TESTS := content scheduler state \
	z80 m68k sh2 mars \
	eeprom eeprom93 spi_eeprom \
	tms9918 smsvdp mdvdp \
	sn76489 ym2413 ym2612 \
	sg1000 sms md
TEST_BINARIES := $(patsubst %,$(BUILD_DIR)/%_test,$(TESTS))
TEST_FLAGS := $(CFLAGS)

.PHONY: all clean check sanitize tools

all: $(CORE)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJECTS:.o=.d)

$(CORE): $(OBJECTS) link.T
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) -lm

$(BUILD_DIR)/%_test: tests/%_test.c $(HARDWARE) $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) -o $@ $< $(HARDWARE) -lm

$(BUILD_DIR)/libretro_smoke_test: tests/libretro_smoke_test.c src/libretro.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) -o $@ tests/libretro_smoke_test.c -ldl

check: all $(TEST_BINARIES) $(BUILD_DIR)/libretro_smoke_test
	@for test in $(TEST_BINARIES); do $$test || exit 1; done
	$(BUILD_DIR)/libretro_smoke_test $(CORE)
	@count=$$(nm -D --defined-only $(CORE) | awk '$$2 == "T" && $$3 ~ /^retro_/ { count++ } END { print count + 0 }'); \
	[ "$$count" -eq 25 ] || { printf 'Expected 25 libretro exports, found %s\n' "$$count" >&2; exit 1; }

SANITIZE_BUILD_DIR ?= /tmp/karashi-sanitize
SANITIZE_CCACHE_DIR ?= /tmp/karashi-ccache
SANITIZE_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=c11 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden
SANITIZE_LDFLAGS := -fsanitize=address,undefined -shared -Wl,--no-undefined -Wl,--version-script=link.T

sanitize:
	CCACHE_DIR=$(SANITIZE_CCACHE_DIR) $(MAKE) BUILD_DIR=$(SANITIZE_BUILD_DIR) CFLAGS='$(SANITIZE_CFLAGS)' LDFLAGS='$(SANITIZE_LDFLAGS)' check

TOOLS := $(BUILD_DIR)/capture $(BUILD_DIR)/bench $(BUILD_DIR)/zex $(BUILD_DIR)/mdprobe $(BUILD_DIR)/marsprobe $(BUILD_DIR)/m68kvec $(BUILD_DIR)/ramdiff $(BUILD_DIR)/statecheck $(BUILD_DIR)/smsprobe

$(BUILD_DIR)/capture: tools/capture.c src/libretro.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -O2 -o $@ $< -ldl

$(BUILD_DIR)/bench: tools/bench.c src/libretro.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -O2 -o $@ $< -ldl

$(BUILD_DIR)/zex: tools/zex.c src/z80.c src/state.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) -o $@ tools/zex.c src/z80.c src/state.c

$(BUILD_DIR)/mdprobe: tools/mdprobe.c $(HARDWARE) $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) -o $@ tools/mdprobe.c $(HARDWARE) -lm

$(BUILD_DIR)/smsprobe: tools/smsprobe.c $(HARDWARE) $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) -o $@ tools/smsprobe.c $(HARDWARE) -lm

$(BUILD_DIR)/m68kvec: tools/m68kvec.c src/m68k.c src/state.c src/m68k.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) -o $@ tools/m68kvec.c src/m68k.c src/state.c

$(BUILD_DIR)/statecheck: tools/statecheck.c src/libretro.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -O2 -o $@ $< -ldl

$(BUILD_DIR)/ramdiff: tools/ramdiff.c src/libretro.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -O2 -o $@ $< -ldl

$(BUILD_DIR)/marsprobe: tools/marsprobe.c $(HARDWARE) $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(TEST_FLAGS) -o $@ tools/marsprobe.c $(HARDWARE) -lm

tools: $(TOOLS)

clean:
	rm -rf $(BUILD_DIR)
