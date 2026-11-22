CXX        ?= g++
PKG_CONFIG ?= pkg-config
BUILD      ?= build
PREFIX     ?= /usr/local

FFMPEG_PKGS = libavformat libavcodec libavfilter libavutil libswscale libswresample
QT_PKGS     = Qt6Widgets Qt6Multimedia Qt6OpenGLWidgets Qt6OpenGL

# The GUI is built when Qt 6 is installed; the CLI never needs it.
HAVE_QT := $(shell $(PKG_CONFIG) --exists $(QT_PKGS) && echo yes)
QT_LIBEXEC := $(shell qmake6 -query QT_HOST_LIBEXECS 2>/dev/null || echo /usr/lib/qt6)
MOC     ?= $(QT_LIBEXEC)/moc
RCC     ?= $(QT_LIBEXEC)/rcc
ICON_SIZES = 16 22 24 32 48 64 128 256 512

CXXFLAGS ?= -O2 -g
# -fPIC: Qt is built with reduced relocations and requires it. "override" keeps
# these when CXXFLAGS is given on the make command line (e.g. CXXFLAGS=-O2).
override CXXFLAGS += -std=c++23 -fPIC -Wall -Wextra -Wpedantic $(shell $(PKG_CONFIG) --cflags $(FFMPEG_PKGS))
FFMPEG_LIBS := $(shell $(PKG_CONFIG) --libs $(FFMPEG_PKGS))
SDL_CFLAGS  := $(shell $(PKG_CONFIG) --cflags sdl3)
SDL_LIBS    := $(shell $(PKG_CONFIG) --libs sdl3)
ifeq ($(HAVE_QT),yes)
  # Qt headers as system headers, so their warnings don't drown ours.
  QT_CFLAGS := $(patsubst -I%,-isystem %,$(shell $(PKG_CONFIG) --cflags $(QT_PKGS)))
  QT_LIBS   := $(shell $(PKG_CONFIG) --libs $(QT_PKGS))
endif

# Core: everything the CLI and the GUI share.
CLI_ONLY  = src/main.cpp src/play_sdl.cpp
CORE_SRC  = $(filter-out $(CLI_ONLY),$(wildcard src/*.cpp))
CORE_OBJ  = $(CORE_SRC:src/%.cpp=$(BUILD)/%.o)
CLI_OBJ   = $(CLI_ONLY:src/%.cpp=$(BUILD)/%.o)
GUI_SRC   = $(wildcard src/gui/*.cpp)
GUI_MOC_H = $(shell grep -l Q_OBJECT src/gui/*.h)
GUI_OBJ   = $(GUI_SRC:src/gui/%.cpp=$(BUILD)/gui/%.o) $(GUI_MOC_H:src/gui/%.h=$(BUILD)/gui/moc_%.o) \
            $(BUILD)/gui/qrc_icons.o
HEADERS   = $(wildcard src/*.h)

BIN     = $(BUILD)/quillreel
GUI_BIN = $(BUILD)/quillreel-gui

all: $(BIN) $(if $(HAVE_QT),$(GUI_BIN))
ifneq ($(HAVE_QT),yes)
	@echo "note: Qt 6 ($(QT_PKGS)) not found; built the CLI only"
endif

$(BIN): $(CORE_OBJ) $(CLI_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(FFMPEG_LIBS) $(SDL_LIBS)

$(GUI_BIN): $(CORE_OBJ) $(GUI_OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(FFMPEG_LIBS) $(QT_LIBS)

$(BUILD)/play_sdl.o: src/play_sdl.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/gui/%.o: src/gui/%.cpp $(HEADERS) $(wildcard src/gui/*.h) | $(BUILD)/gui
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -Isrc -Isrc/gui -c -o $@ $<

$(BUILD)/gui/moc_%.cpp: src/gui/%.h | $(BUILD)/gui
	$(MOC) $< -o $@

$(BUILD)/gui/moc_%.o: $(BUILD)/gui/moc_%.cpp $(HEADERS) $(wildcard src/gui/*.h)
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -Isrc -Isrc/gui -c -o $@ $<

# The application icon, embedded for the window (data/icons.qrc).
$(BUILD)/gui/qrc_icons.cpp: data/icons.qrc $(wildcard data/icons/*.png) | $(BUILD)/gui
	$(RCC) --name icons $< -o $@

$(BUILD)/gui/qrc_icons.o: $(BUILD)/gui/qrc_icons.cpp
	$(CXX) $(CXXFLAGS) $(QT_CFLAGS) -c -o $@ $<

$(BUILD) $(BUILD)/gui:
	mkdir -p $@

# Set QUILLREEL_SAMPLES to Bink sample directories (separated by ":") to run
# the tests against real files, and QUILLREEL_BINK2_DLL to test Bink 2 video.
test: all
	tests/run.sh $(BIN) $(if $(HAVE_QT),$(GUI_BIN))

# Installs Quillreel only. Bink 2 video needs RAD's decoder (bink2w64.dll),
# which is never shipped or installed; users add their own copy with
# `quillreel bink2 install PATH`.
install: all
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/quillreel
ifeq ($(HAVE_QT),yes)
	install -Dm755 $(GUI_BIN) $(DESTDIR)$(PREFIX)/bin/quillreel-gui
	install -Dm644 data/quillreel.desktop $(DESTDIR)$(PREFIX)/share/applications/quillreel.desktop
	for s in $(ICON_SIZES); do \
	  install -Dm644 data/icons/quillreel-$$s.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/quillreel.png; \
	done
	install -Dm644 data/icons/src/quillreel.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/quillreel.svg
endif

# Release packages built in containers with podman (see packaging/README.md),
# written to dist/: a .deb for Debian 13 plus the AppImage (built on Debian 13),
# and a .deb for Ubuntu 26.04.
PACKAGE_IMAGE ?= quillreel-packaging
MAINTAINER    ?= b1g-b8lls <b1gb8lls@tutamail.com>
package: package-debian13 package-ubuntu26.04

package-debian13: BASE = docker.io/library/debian:trixie
package-debian13: APPIMAGE = 1
package-ubuntu26.04: BASE = docker.io/library/ubuntu:26.04
package-ubuntu26.04: APPIMAGE = 0
package-debian13 package-ubuntu26.04:
	podman build --build-arg BASE=$(BASE) -t $(PACKAGE_IMAGE):$(@:package-%=%) packaging
	mkdir -p dist
	podman run --rm -v "$(CURDIR)":/src:ro,Z -v "$(CURDIR)/dist":/out:Z -e BUILD_APPIMAGE=$(APPIMAGE) \
	    -e MAINTAINER="$(MAINTAINER)" $(PACKAGE_IMAGE):$(@:package-%=%) /src/packaging/build.sh

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/quillreel $(DESTDIR)$(PREFIX)/bin/quillreel-gui \
	      $(DESTDIR)$(PREFIX)/share/applications/quillreel.desktop
	for s in $(ICON_SIZES); do rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/quillreel.png; done
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/quillreel.svg

clean:
	rm -rf $(BUILD)

.PHONY: all test install uninstall clean package package-debian13 package-ubuntu26.04
.SECONDARY:
