# ---------------------------------------------------------------------------
#  Build file. The program name is two variables: NAME is the lowercase
#  binary/config name and DISPLAY_NAME is what users see. Change them (or run
#  `make NAME=whatever DISPLAY_NAME=Whatever`) and the binary, config
#  directory, app-id, data directory, window title and TERM_PROGRAM follow.
# ---------------------------------------------------------------------------
NAME       ?= feterm
DISPLAY_NAME ?= FeTerm
AUTHOR     ?= Faheem Essack
EMAIL      ?= faheem.essack@gmail.com
VERSION    ?= 0.5.0
APP_ID     ?= io.gitlab.faheeme.$(NAME)
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
           -Wundef $(WERROR)
# Warnings are errors by default. Packagers building with newer compilers can
# pass WERROR= so a new warning doesn't break the build.
WERROR  ?= -Werror
# Control-flow protection has a different flag on each CPU (CET on x86, PAC/BTI
# on ARM64); other CPUs build without it.
TARGET  := $(shell $(CC) -dumpmachine)
CFPROT  := $(if $(filter x86_64-% i%86-%,$(TARGET)),-fcf-protection,$(if $(filter aarch64-%,$(TARGET)),-mbranch-protection=standard))
HARDEN  := -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection \
           $(CFPROT) -fPIE -fno-strict-overflow -fno-delete-null-pointer-checks
OPT     ?= -O2 -g

# Windows (MinGW-w64, usually cross-compiled: make CC=x86_64-w64-mingw32-gcc).
# Files ending in _win.c replace the POSIX-only ones listed here.
WINDOWS    := $(if $(findstring mingw,$(TARGET)),1)
POSIX_ONLY := src/pty/pty.c src/config/watch.c src/util/spawn.c src/util/os_posix.c src/remote/proc.c \
              src/platform/fdwatch_posix.c src/sessions/helper_io.c src/sessions/serial.c \
              src/sessions/serial_baud.c src/sessions/telnet_helper.c
WIN_ONLY   := $(wildcard src/*/*_win.c)
# feterm-helper.exe on Windows: the Telnet and serial clients as a console program.
HELPER_SRC := src/sessions/helper_main_win.c src/sessions/helper_con_win.c src/sessions/telnet_helper_win.c \
              src/sessions/serial_win.c

CPPFLAGS += -D_GNU_SOURCE -I$(GEN) -Isrc
ifdef WINDOWS
EXE      := .exe
PKGS     := sdl3 libpcre2-8 freetype2 fontconfig
HARDEN   := -D_FORTIFY_SOURCE=3 -fstack-protector-strong $(CFPROT)
CPPFLAGS += -include src/util/compat_win.h -D__USE_MINGW_ANSI_STDIO=1 -DGL_GLEXT_PROTOTYPES
LDFLAGS  += -mwindows -Wl,--dynamicbase,--high-entropy-va,--nxcompat
OS_LIBS  := -lGLESv2 -lssp -lws2_32 -lshell32 -lole32 -luuid
OS_SRC   := $(filter-out $(POSIX_ONLY) $(HELPER_SRC),$(wildcard src/*.c src/*/*.c))
HELPER   := $(NAME)-helper.exe
# Binary mode for every fopen(): no CRLF translation anywhere.
BINMODE  := $(shell $(CC) -print-file-name=binmode.o)
# Icon, manifest (UTF-8 code page, DPI awareness) and version information.
WINDRES  ?= $(CC:gcc=windres)
ifeq ($(origin AR),default)
AR       := $(CC:gcc=ar)
endif
RES      := $(BUILD)/$(NAME)_res.o
else
LDFLAGS  += -pie -Wl,-z,relro,-z,now,-z,noexecstack,--as-needed
OS_SRC   := $(filter-out $(WIN_ONLY),$(wildcard src/*.c src/*/*.c))
endif
CFLAGS   += -std=c23 $(WARN) $(HARDEN) $(OPT) $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS)) $(OS_LIBS) -lm

SRC := $(OS_SRC)
OBJ := $(SRC:%.c=$(BUILD)/%.o)
DEP := $(OBJ:.o=.d)

.PHONY: all clean debug analyze install uninstall test

all: $(NAME)$(EXE) $(HELPER) $(BUILD)/$(APP_ID).desktop $(BUILD)/$(NAME).1

$(GEN)/app_name.h: src/app_name.h.in Makefile
	@mkdir -p $(@D)
	sed -e 's|@NAME@|$(NAME)|g' -e 's|@DISPLAY_NAME@|$(DISPLAY_NAME)|g' -e 's|@VERSION@|$(VERSION)|g' \
	    -e 's|@APP_ID@|$(APP_ID)|g' -e 's|@DATADIR@|$(DATADIR)|g' \
	    -e 's|@AUTHOR@|$(AUTHOR)|g' -e 's|@EMAIL@|$(EMAIL)|g' $< > $@

# Launcher entry, named after the app-id so Wayland compositors match the window to it.
$(BUILD)/$(APP_ID).desktop: data/app.desktop.in Makefile
	@mkdir -p $(@D)
	sed -e 's|@NAME@|$(NAME)|g' -e 's|@DISPLAY_NAME@|$(DISPLAY_NAME)|g' -e 's|@APP_ID@|$(APP_ID)|g' $< > $@

# Man page, with the same name substitutions.
$(BUILD)/$(NAME).1: data/app.1.in Makefile
	@mkdir -p $(@D)
	sed -e 's|@NAME@|$(NAME)|g' -e 's|@NAME_UPPER@|$(shell echo '$(NAME)' | tr a-z A-Z)|g' \
	    -e 's|@DISPLAY_NAME@|$(DISPLAY_NAME)|g' -e 's|@VERSION@|$(VERSION)|g' -e 's|@APP_ID@|$(APP_ID)|g' \
	    -e 's|@DATADIR@|$(DATADIR)|g' -e 's|@AUTHOR@|$(AUTHOR)|g' -e 's|@EMAIL@|$(EMAIL)|g' $< > $@

$(BUILD)/%.o: %.c $(GEN)/app_name.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

comma := ,
$(BUILD)/$(NAME)_res.o: data/windows/feterm.rc data/windows/feterm.manifest data/windows/feterm.ico $(GEN)/app_name.h
	@mkdir -p $(@D)
	$(WINDRES) -I$(GEN) -Idata/windows -DAPP_VERSION_NUM=$(subst .,$(comma),$(VERSION)),0 $< -O coff -o $@

$(NAME)$(EXE): $(OBJ) $(RES)
	$(CC) $(CFLAGS) $(LDFLAGS) $(BINMODE) $^ -o $@ $(LDLIBS)

# The helper links only what it uses: the app's objects go in as a library.
$(BUILD)/libapp.a: $(filter-out $(BUILD)/src/main.o,$(OBJ))
	rm -f $@
	$(AR) rcs $@ $^

$(NAME)-helper.exe: $(HELPER_SRC:%.c=$(BUILD)/%.o) $(BUILD)/libapp.a $(RES)
	$(CC) $(CFLAGS) $(filter-out -mwindows,$(LDFLAGS)) -mconsole $(BINMODE) $^ -o $@ -lws2_32 -lssp -lm

# Headless unit tests for the core, built with ASan + UBSan.
TEST_SRC := $(filter-out $(if $(WINDOWS),$(POSIX_ONLY) $(HELPER_SRC),$(WIN_ONLY)),$(wildcard src/util/*.c src/config/*.c src/vt/*.c \
                src/highlight/*.c src/ui/*.c src/sessions/*.c src/remote/*.c)) \
            src/input/keys.c src/input/click.c src/input/mouse.c src/input/keybind.c \
            src/app/layout.c src/app/banner.c src/render/boxdraw.c tests/test_core.c \
            $(if $(WINDOWS),src/pty/conpty_win.c)
# MinGW has no sanitizers; a Windows build cross-compiled on Linux runs its tests under Wine.
ifdef WINDOWS
TEST_SAN ?=
TEST_RUN ?= $(if $(filter Windows_NT,$(OS)),,WINEDEBUG=-all wine)
else
TEST_SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
endif

$(BUILD)/test_core$(EXE): $(TEST_SRC) $(wildcard tests/*.inc) $(GEN)/app_name.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c23 $(WARN) -Wno-missing-prototypes -O1 -g $(TEST_SAN) \
	    $(shell $(PKG_CONFIG) --cflags libpcre2-8) $(TEST_SRC) -o $@ $(shell $(PKG_CONFIG) --libs libpcre2-8) -lm

test: $(BUILD)/test_core$(EXE)
	$(TEST_RUN) ./$(BUILD)/test_core$(EXE)

# AddressSanitizer + UndefinedBehaviorSanitizer build.
debug:
	$(MAKE) BUILD=build-debug OPT="-O1 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all" \
	        LDFLAGS="-fsanitize=address,undefined" NAME=$(NAME)

# GCC static analyzer.
analyze:
	$(MAKE) BUILD=build-analyze OPT="-O1 -fanalyzer" NAME=$(NAME)

ICON_SIZES := 16 24 32 48 64 128 256 512
ICONDIR    := $(PREFIX)/share/icons/hicolor
APPSDIR    := $(PREFIX)/share/applications
MANDIR     := $(PREFIX)/share/man/man1

install: $(NAME) $(BUILD)/$(APP_ID).desktop $(BUILD)/$(NAME).1
	install -Dm755 $(NAME) $(DESTDIR)$(PREFIX)/bin/$(NAME)
	install -d $(DESTDIR)$(DATADIR)/themes $(DESTDIR)$(DATADIR)/highlight
	install -m644 data/themes/*.ini $(DESTDIR)$(DATADIR)/themes/
	-install -m644 data/highlight/*.ini $(DESTDIR)$(DATADIR)/highlight/
	install -m644 data/config.ini.example $(DESTDIR)$(DATADIR)/
	install -Dm644 $(BUILD)/$(NAME).1 $(DESTDIR)$(MANDIR)/$(NAME).1
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
	rm -f $(DESTDIR)$(MANDIR)/$(NAME).1
	for s in $(ICON_SIZES); do rm -f $(DESTDIR)$(ICONDIR)/$${s}x$$s/apps/$(APP_ID).png; done
	rm -f $(DESTDIR)$(ICONDIR)/scalable/apps/$(APP_ID).svg
	rm -f $(DESTDIR)$(ICONDIR)/symbolic/apps/$(APP_ID)-symbolic.svg
	$(if $(DESTDIR),,-gtk-update-icon-cache -qtf $(ICONDIR))

clean:
	rm -rf build build-debug build-analyze $(NAME) $(NAME).exe $(NAME)-helper.exe

-include $(DEP)
