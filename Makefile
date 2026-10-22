# ---------------------------------------------------------------------------
#  Build file. The program name is two variables: NAME is the lowercase
#  binary/config name and DISPLAY_NAME is what users see. Change them (or run
#  `make NAME=whatever DISPLAY_NAME=Whatever`) and the binary, config
#  directory, app-id, data directory, window title and TERM_PROGRAM follow.
# ---------------------------------------------------------------------------
NAME       ?= feterm
DISPLAY_NAME ?= FeTerm
VERSION    ?= 0.1.0
APP_ID     ?= io.github.$(NAME)
PREFIX     ?= /usr/local
DATADIR    ?= $(PREFIX)/share/$(NAME)

CC         ?= gcc
PKG_CONFIG ?= pkg-config
PKGS       := sdl3 libpcre2-8 freetype2 fontconfig glesv2

BUILD      ?= build
GEN        := $(BUILD)/gen

WARN    := -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow \
           -Wformat=2 -Wformat-security -Wnull-dereference -Wvla -Wcast-qual \
           -Wstrict-prototypes -Wmissing-prototypes -Wimplicit-fallthrough \
           -Wundef -Werror
HARDEN  := -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection \
           -fcf-protection -fPIE -fno-strict-overflow -fno-delete-null-pointer-checks
OPT     ?= -O2 -g

CPPFLAGS += -D_GNU_SOURCE -I$(GEN) -Isrc
CFLAGS   += -std=c23 $(WARN) $(HARDEN) $(OPT) $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDFLAGS  += -pie -Wl,-z,relro,-z,now,-z,noexecstack,--as-needed
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

SRC := $(wildcard src/*.c src/*/*.c)
OBJ := $(SRC:%.c=$(BUILD)/%.o)
DEP := $(OBJ:.o=.d)

.PHONY: all clean debug analyze install uninstall test

all: $(NAME) $(BUILD)/$(APP_ID).desktop

$(GEN)/app_name.h: src/app_name.h.in Makefile
	@mkdir -p $(@D)
	sed -e 's|@NAME@|$(NAME)|g' -e 's|@DISPLAY_NAME@|$(DISPLAY_NAME)|g' -e 's|@VERSION@|$(VERSION)|g' \
	    -e 's|@APP_ID@|$(APP_ID)|g' -e 's|@DATADIR@|$(DATADIR)|g' $< > $@

# Launcher entry, named after the app-id so Wayland compositors match the window to it.
$(BUILD)/$(APP_ID).desktop: data/app.desktop.in Makefile
	@mkdir -p $(@D)
	sed -e 's|@NAME@|$(NAME)|g' -e 's|@DISPLAY_NAME@|$(DISPLAY_NAME)|g' -e 's|@APP_ID@|$(APP_ID)|g' $< > $@

$(BUILD)/%.o: %.c $(GEN)/app_name.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LDLIBS)

# Headless unit tests for the core, built with ASan + UBSan.
TEST_SRC := $(wildcard src/util/*.c src/config/*.c src/vt/*.c src/highlight/*.c src/ui/*.c) \
            src/input/keys.c src/input/click.c src/input/mouse.c src/input/keybind.c \
            src/app/layout.c src/render/boxdraw.c tests/test_core.c
TEST_SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

$(BUILD)/test_core: $(TEST_SRC) $(GEN)/app_name.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c23 $(WARN) -Wno-missing-prototypes -O1 -g $(TEST_SAN) \
	    $(shell $(PKG_CONFIG) --cflags libpcre2-8) $(TEST_SRC) -o $@ $(shell $(PKG_CONFIG) --libs libpcre2-8) -lm

test: $(BUILD)/test_core
	./$(BUILD)/test_core

# AddressSanitizer + UndefinedBehaviorSanitizer build.
debug:
	$(MAKE) BUILD=build-debug OPT="-O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all" \
	        LDFLAGS="-fsanitize=address,undefined" NAME=$(NAME)

# GCC static analyzer.
analyze:
	$(MAKE) BUILD=build-analyze OPT="-O0 -fanalyzer" NAME=$(NAME)

ICON_SIZES := 16 24 32 48 64 128 256 512
ICONDIR    := $(PREFIX)/share/icons/hicolor
APPSDIR    := $(PREFIX)/share/applications

install: $(NAME) $(BUILD)/$(APP_ID).desktop
	install -Dm755 $(NAME) $(DESTDIR)$(PREFIX)/bin/$(NAME)
	install -d $(DESTDIR)$(DATADIR)/themes $(DESTDIR)$(DATADIR)/highlight
	install -m644 data/themes/*.ini $(DESTDIR)$(DATADIR)/themes/
	-install -m644 data/highlight/*.ini $(DESTDIR)$(DATADIR)/highlight/
	install -Dm644 $(BUILD)/$(APP_ID).desktop $(DESTDIR)$(APPSDIR)/$(APP_ID).desktop
	for s in $(ICON_SIZES); do \
	    install -Dm644 data/icons/feterm-$$s.png $(DESTDIR)$(ICONDIR)/$${s}x$$s/apps/$(APP_ID).png || exit 1; \
	done
	install -Dm644 data/icons/feterm.svg $(DESTDIR)$(ICONDIR)/scalable/apps/$(APP_ID).svg
	install -Dm644 data/icons/feterm-symbolic.svg $(DESTDIR)$(ICONDIR)/symbolic/apps/$(APP_ID)-symbolic.svg
	$(if $(DESTDIR),,-gtk-update-icon-cache -qtf $(ICONDIR))
	$(if $(DESTDIR),,-update-desktop-database -q $(APPSDIR))

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(NAME)
	rm -rf $(DESTDIR)$(DATADIR)
	rm -f $(DESTDIR)$(APPSDIR)/$(APP_ID).desktop
	for s in $(ICON_SIZES); do rm -f $(DESTDIR)$(ICONDIR)/$${s}x$$s/apps/$(APP_ID).png; done
	rm -f $(DESTDIR)$(ICONDIR)/scalable/apps/$(APP_ID).svg
	rm -f $(DESTDIR)$(ICONDIR)/symbolic/apps/$(APP_ID)-symbolic.svg
	$(if $(DESTDIR),,-gtk-update-icon-cache -qtf $(ICONDIR))

clean:
	rm -rf build build-debug build-analyze $(NAME)

-include $(DEP)
