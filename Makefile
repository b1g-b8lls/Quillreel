CXX      ?= g++
PKGS      = libavformat libavcodec libavfilter libavutil libswscale libswresample sdl3
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++23 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:src/%.cpp=build/%.o)
BIN = build/quillreel

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp $(wildcard src/*.h) | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build:
	mkdir -p build

# Set QUILLREEL_SAMPLES to a directory of .bik/.bk2 files to run the FFmpeg comparison.
test: $(BIN)
	tests/run.sh $(BIN)

clean:
	rm -rf build

.PHONY: all test clean
