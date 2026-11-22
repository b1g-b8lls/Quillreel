CXX      ?= g++
PKGS      = libavformat libavcodec libavfilter libavutil libswscale libswresample sdl3

PKG_CONFIG ?= pkg-config
BUILD      ?= build
PREFIX     ?= /usr/local

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++23 -Wall -Wextra -Wpedantic $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS))

SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:src/%.cpp=$(BUILD)/%.o)
BIN = $(BUILD)/quillreel

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.cpp $(wildcard src/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

# Set QUILLREEL_SAMPLES to Bink sample directories (separated by ":") to run
# the tests against real files.
test: $(BIN)
	tests/run.sh $(BIN)

# Installs Quillreel only. Bink 2 video needs RAD's decoder (bink2w64.dll),
# which is never shipped or installed; users add their own copy with
# `quillreel bink2 install PATH`.
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/quillreel

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/quillreel

clean:
	rm -rf $(BUILD)

.PHONY: all test install uninstall clean
