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
VERSION    ?= 0.5.2
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
# macOS (clang 18+ for C23, e.g. Homebrew's llvm). Files ending in _mac.c
# replace the Linux-only ones listed here.
MACOS      := $(if $(findstring darwin,$(TARGET)),1)
POSIX_ONLY := src/pty/pty.c src/config/watch.c src/util/spawn.c src/util/os_posix.c src/remote/proc.c \
              src/platform/fdwatch_posix.c src/sessions/helper_io.c src/sessions/serial.c \
              src/sessions/serial_baud.c src/sessions/telnet_helper.c
WIN_ONLY   := $(wildcard src/*/*_win.c)
LINUX_ONLY := src/config/watch.c src/sessions/serial_baud.c
MAC_ONLY   := $(wildcard src/*/*_mac.c)
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
EXCLUDE  := $(POSIX_ONLY) $(HELPER_SRC) $(MAC_ONLY)
HELPER   := $(NAME)-helper.exe
# Binary mode for every fopen(): no CRLF translation anywhere.
BINMODE  := $(shell $(CC) -print-file-name=binmode.o)
# Icon, manifest (UTF-8 code page, DPI awareness) and version information.
WINDRES  ?= $(CC:gcc=windres)
ifeq ($(origin AR),default)
AR       := $(CC:gcc=ar)
endif
RES      := $(BUILD)/$(NAME)_res.o
else ifdef MACOS
# No OpenGL ES on macOS: the native OpenGL core profile (src/render/gl.h).
PKGS     := sdl3 libpcre2-8 freetype2 fontconfig
# Apple's toolchain probes the stack itself and makes PIE by default; the GNU
# ld options below do not exist there.
HARDEN   := -D_FORTIFY_SOURCE=3 -fstack-protector-strong $(CFPROT) -fPIE -fno-strict-overflow \
            -fno-delete-null-pointer-checks
CPPFLAGS += -include src/util/compat_mac.h
# The bundled Homebrew libraries need the macOS they were built on, so the
# app claims no older one. Build on the oldest macOS you want to support.
MACOS_MIN ?= $(shell sw_vers -productVersion | cut -d. -f1).0
CFLAGS   += -mmacosx-version-min=$(MACOS_MIN)
LDFLAGS  += -mmacosx-version-min=$(MACOS_MIN) -Wl,-headerpad_max_install_names
OS_LIBS  := -framework OpenGL -lobjc
EXCLUDE  := $(WIN_ONLY) $(LINUX_ONLY)
else
LDFLAGS  += -pie -Wl,-z,relro,-z,now,-z,noexecstack,--as-needed
EXCLUDE  := $(WIN_ONLY) $(MAC_ONLY)
endif
OS_SRC   := $(filter-out $(EXCLUDE),$(wildcard src/*.c src/*/*.c))
CFLAGS   += -std=c23 $(WARN) $(HARDEN) $(OPT) $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS)) $(OS_LIBS) -lm

SRC := $(OS_SRC)
OBJ := $(SRC:%.c=$(BUILD)/%.o)
DEP := $(OBJ:.o=.d)

.PHONY: all clean debug analyze install uninstall test app app-unbundled dmg

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
TEST_SRC := $(filter-out $(EXCLUDE),$(wildcard src/util/*.c src/config/*.c src/vt/*.c \
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

# One rendered frame written to a PPM image, through the real platform, font and
# renderer code: $(BUILD)/render_snapshot out.ppm [theme]. Needs a display (or
# SDL_VIDEO_DRIVER=offscreen on Linux).
$(BUILD)/render_snapshot$(EXE): tests/render_snapshot.c $(BUILD)/libapp.a
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-missing-prototypes $(LDFLAGS) $^ -o $@ $(LDLIBS)

# libFuzzer targets for the untrusted-input parsers (needs clang). One binary per
# FUZZ_TARGET; run e.g. ./build/fuzz_term -max_total_time=60 corpus/. FUZZ_CC and
# FUZZ_TIME can be overridden. The sources are the unit-test set minus test_core.c.
FUZZ_CC   ?= clang
FUZZ_TIME ?= 30
# Extra link flags, e.g. -fuse-ld=lld when the system linker is older than FUZZ_CC.
FUZZ_LDFLAGS ?=
FUZZ_SRC  := $(filter-out tests/test_core.c,$(TEST_SRC))
FUZZ_NAMES := term sftp unescape sixel ini
FUZZ_BINS  := $(addprefix $(BUILD)/fuzz_,$(FUZZ_NAMES))
FUZZ_SAN   := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

$(BUILD)/fuzz_term:     FUZZ_N := 1
$(BUILD)/fuzz_sftp:     FUZZ_N := 2
$(BUILD)/fuzz_unescape: FUZZ_N := 3
$(BUILD)/fuzz_sixel:    FUZZ_N := 4
$(BUILD)/fuzz_ini:      FUZZ_N := 5

$(FUZZ_BINS): $(BUILD)/fuzz_%: tests/fuzz.c $(FUZZ_SRC) $(wildcard tests/*.inc) $(GEN)/app_name.h
	@mkdir -p $(@D)
	$(FUZZ_CC) $(CPPFLAGS) -std=c23 -Wno-missing-prototypes -g -O1 -DFUZZ_TARGET=$(FUZZ_N) $(FUZZ_SAN) $(FUZZ_LDFLAGS) \
	    $(shell $(PKG_CONFIG) --cflags libpcre2-8) tests/fuzz.c $(FUZZ_SRC) -o $@ \
	    $(shell $(PKG_CONFIG) --libs libpcre2-8) -lm

fuzz: $(FUZZ_BINS)
	@for b in $(FUZZ_BINS); do echo "== $$b"; ASAN_OPTIONS=detect_leaks=1 \
	    LSAN_OPTIONS=suppressions=tests/lsan-fuzz.supp:print_suppressions=1 \
	    $$b -max_total_time=$(FUZZ_TIME) -print_final_stats=1 || exit 1; done

# Reproduce a crash/corpus file with plain gcc (no libFuzzer): make fuzz-standalone ARGS="file..."
fuzz-standalone:
	$(CC) $(CPPFLAGS) -std=c23 -Wno-missing-prototypes -g -O1 -DFUZZ_TARGET=$(or $(T),1) -DFUZZ_STANDALONE \
	    $(TEST_SAN) $(shell $(PKG_CONFIG) --cflags libpcre2-8) tests/fuzz.c $(FUZZ_SRC) -o $(BUILD)/fuzz_standalone \
	    $(shell $(PKG_CONFIG) --libs libpcre2-8) -lm
	./$(BUILD)/fuzz_standalone $(ARGS)

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

ifdef MACOS
# macOS (e.g. a Homebrew formula): the program, its data and the man page;
# BSD install has no -D, and there is no desktop file or icon cache. fonts.conf
# goes with the data so the bundled-libraries' default is not needed.
install: $(NAME) $(BUILD)/$(NAME).1 $(BUILD)/fonts.conf
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(DATADIR)/themes $(DESTDIR)$(DATADIR)/highlight $(DESTDIR)$(MANDIR)
	install -m755 $(NAME) $(DESTDIR)$(PREFIX)/bin/$(NAME)
	install -m644 data/themes/*.ini $(DESTDIR)$(DATADIR)/themes/
	install -m644 data/highlight/*.ini $(DESTDIR)$(DATADIR)/highlight/
	install -m644 data/config.ini.example $(DESTDIR)$(DATADIR)/
	install -m644 $(BUILD)/fonts.conf $(DESTDIR)$(DATADIR)/fonts.conf
	install -m644 $(BUILD)/$(NAME).1 $(DESTDIR)$(MANDIR)/$(NAME).1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(NAME) $(DESTDIR)$(MANDIR)/$(NAME).1
	rm -rf $(DESTDIR)$(DATADIR)
else
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
endif

# macOS: a self-contained, ad-hoc signed .app bundle and a .dmg holding it.
APP_BUNDLE := $(BUILD)/$(DISPLAY_NAME).app
DMG        := $(BUILD)/$(NAME)-$(VERSION)-macos-$(shell uname -m).dmg
MAC_SED    := sed -e 's|@NAME@|$(NAME)|g' -e 's|@DISPLAY_NAME@|$(DISPLAY_NAME)|g' -e 's|@APP_ID@|$(APP_ID)|g' \
                  -e 's|@VERSION@|$(VERSION)|g' -e 's|@AUTHOR@|$(AUTHOR)|g' -e 's|@MACOS_MIN@|$(MACOS_MIN)|g'

$(BUILD)/Info.plist: data/macos/Info.plist.in Makefile
	@mkdir -p $(@D)
	$(MAC_SED) $< > $@

$(BUILD)/fonts.conf: data/macos/fonts.conf.in Makefile
	@mkdir -p $(@D)
	$(MAC_SED) $< > $@

app: $(NAME) $(BUILD)/Info.plist $(BUILD)/fonts.conf
	packaging/macos/make-app.sh $(NAME) "$(APP_BUNDLE)" $(BUILD)/Info.plist $(BUILD)/fonts.conf

# A thin .app in APP_DIR for a package manager (Homebrew) that already provides
# the libraries: nothing is copied in, and data comes from $(DATADIR) via
# `make install`. APP_DIR=$(PREFIX) puts it next to bin/.
APP_DIR ?= $(PREFIX)
app-unbundled: $(NAME) $(BUILD)/Info.plist $(BUILD)/fonts.conf
	packaging/macos/make-app.sh --unbundled $(NAME) "$(DESTDIR)$(APP_DIR)/$(DISPLAY_NAME).app" $(BUILD)/Info.plist \
	    $(BUILD)/fonts.conf

dmg: app
	rm -f $(DMG)
	hdiutil create -volname "$(DISPLAY_NAME) $(VERSION)" -srcfolder "$(APP_BUNDLE)" -ov -format UDZO $(DMG)

clean:
	rm -rf build build-debug build-analyze $(NAME) $(NAME).exe $(NAME)-helper.exe

-include $(DEP)
