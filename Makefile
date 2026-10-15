# terep2 in C++ ("C with classes").
#   make            headless test host, terep2-headless (no dependencies)
#   make sdl        SDL2 version (needs SDL2 development files and the host/imgui submodule)
#   make test       replay the Win16 recordings in tests/ and compare the frames
#   make coverage   which lines of src/ the tests run (clang/LLVM); REPLAY="recdir mapdir": a recording's
#   make -j8 ...    in parallel: every file is its own object in build/
CXX      ?= c++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=gnu++17 -fwrapv -Wall -Wno-unused-result
DEPFLAGS  = -MMD -MP                    # header (and #embed) dependencies, in the .d files

GAME     := $(wildcard src/*.cpp)
GAME_OBJ := $(patsubst src/%.cpp,build/game/%.o,$(GAME))
HOST_OBJ := build/host/common.o
TEST_OBJ := $(patsubst host_test/%.cpp,build/test/%.o,$(wildcard host_test/*.cpp))

all: terep2-headless

src/dataseg.hpp: src/dataseg.def tools/gen_dataseg.py
	python3 tools/gen_dataseg.py

build build/game build/host build/test build/sdl build/imgui/backends:
	@mkdir -p $@

# The game and what the hosts share; dataseg.hpp is generated before anything is compiled.
build/game/%.o: src/%.cpp | build/game src/dataseg.hpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<
build/host/%.o: host/%.cpp | build/host src/dataseg.hpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<
build/test/%.o: host_test/%.cpp | build/test src/dataseg.hpp
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

terep2-headless: $(GAME_OBJ) $(HOST_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm

# The SDL host and its map chooser (OpenGL 3.3); Dear ImGui is the git submodule in host/imgui.
ifeq ($(OS),Windows_NT)
GL_LIBS  := -lopengl32
else
GL_LIBS  := -lGL
endif
# The external OpenGL renderer (TODO-lib.md):
#   make sdl GLR=1 GLR_DIR=its/include GLR_LIB="path/to/lib.a and what it needs"
ifdef GLR
GLR_FLAGS := -DTEREP2_GLR $(if $(GLR_DIR),-I$(GLR_DIR))
endif
IMGUI    := host/imgui
OPL3     := host/Nuked-OPL3
SDL_SRC  := sdl_main launcher voxel_preview pcx opl_sound minimap gui renderer_api \
            game_renderer renderer_sw renderer_gl
SDL_OBJ  := $(patsubst %,build/sdl/%.o,$(SDL_SRC))
SDL_FLAGS = $(CXXFLAGS) $(GLR_FLAGS) $(shell sdl2-config --cflags) -I$(IMGUI) -I$(IMGUI)/backends -I$(OPL3)
IMGUI_OBJ := $(patsubst $(IMGUI)/%.cpp,build/imgui/%.o,$(addprefix $(IMGUI)/,imgui.cpp imgui_draw.cpp \
             imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_sdl2.cpp backends/imgui_impl_opengl3.cpp))

$(IMGUI)/imgui.h:
	@echo "Dear ImGui is missing: git submodule update --init" >&2; false
$(OPL3)/opl3.h:
	@echo "Nuked-OPL3 is missing: git submodule update --init" >&2; false

# The SDL host's flags (GLR, GLR_DIR, GLR_LIB...), kept in a file that only changes when they
# do: changing them rebuilds what they touch, with no need to remove anything by hand.
build/sdl/flags: FORCE | build/sdl
	@echo '$(SDL_FLAGS) | $(GLR_LIB)' | cmp -s - $@ || echo '$(SDL_FLAGS) | $(GLR_LIB)' > $@
FORCE:

build/sdl/%.o: host/%.cpp build/sdl/flags | build/sdl src/dataseg.hpp $(IMGUI)/imgui.h $(OPL3)/opl3.h
	$(CXX) $(SDL_FLAGS) $(DEPFLAGS) -c -o $@ $<
build/imgui/%.o: $(IMGUI)/%.cpp | build/imgui/backends $(IMGUI)/imgui.h
	$(CXX) -O2 -std=gnu++17 $(shell sdl2-config --cflags) -I$(IMGUI) $(DEPFLAGS) -c -o $@ $<
build/opl3.o: $(OPL3)/opl3.c | build $(OPL3)/opl3.h
	$(CC) -O2 $(DEPFLAGS) -c -o $@ $<

sdl: terep2vibed
terep2vibed: $(GAME_OBJ) $(HOST_OBJ) $(SDL_OBJ) $(IMGUI_OBJ) build/opl3.o build/sdl/flags
	$(CXX) $(CXXFLAGS) -o $@ $(GAME_OBJ) $(HOST_OBJ) $(SDL_OBJ) $(IMGUI_OBJ) build/opl3.o \
		$(GLR_LIB) $(shell sdl2-config --libs) $(GL_LIBS) -lm

test: terep2-headless
	sh tests/run_tests.sh

# Coverage of the game's code: the headless host built with clang's source-based coverage in
# build/cov, then the tests (the recordings in tests/, pass or not), or with
# REPLAY="recdir mapdir" only that recording.  Summary on the terminal, report in build/cov/html.
COV_CXX       ?= clang++
LLVM_PROFDATA ?= llvm-profdata
LLVM_COV      ?= llvm-cov
COV_FLAGS     := -O0 -g -std=gnu++17 -fwrapv -fprofile-instr-generate -fcoverage-mapping
COV_OBJ       := $(patsubst build/%,build/cov/%,$(GAME_OBJ) $(HOST_OBJ) $(TEST_OBJ))
COV_RUN       := LLVM_PROFILE_FILE=$(CURDIR)/build/cov/prof/%p.profraw
COV_SHOW      := build/cov/terep2-headless -instr-profile=build/cov/all.profdata -ignore-filename-regex='host'

build/cov/game/%.o: src/%.cpp | src/dataseg.hpp
	@mkdir -p $(@D)
	$(COV_CXX) $(COV_FLAGS) $(DEPFLAGS) -c -o $@ $<
build/cov/host/%.o: host/%.cpp | src/dataseg.hpp
	@mkdir -p $(@D)
	$(COV_CXX) $(COV_FLAGS) $(DEPFLAGS) -c -o $@ $<
build/cov/test/%.o: host_test/%.cpp | src/dataseg.hpp
	@mkdir -p $(@D)
	$(COV_CXX) $(COV_FLAGS) $(DEPFLAGS) -c -o $@ $<
build/cov/terep2-headless: $(COV_OBJ)
	$(COV_CXX) $(COV_FLAGS) -o $@ $^ -lm

coverage: build/cov/terep2-headless
	rm -rf build/cov/prof build/cov/html
ifdef REPLAY
	-$(COV_RUN) build/cov/terep2-headless -C $(REPLAY) | tail -1
else
	-$(COV_RUN) sh tests/run_tests.sh build/cov/terep2-headless
endif
	$(LLVM_PROFDATA) merge -o build/cov/all.profdata build/cov/prof/*.profraw
	$(LLVM_COV) report $(COV_SHOW)
	$(LLVM_COV) show $(COV_SHOW) -format=html -show-branches=count -output-dir=build/cov/html > /dev/null
	@echo "report: build/cov/html/index.html"

clean:
	rm -f terep2vibed terep2-headless
	rm -rf build/game build/host build/test build/sdl build/imgui build/opl3.o build/opl3.d build/cov

-include $(wildcard build/*/*.d build/*.d build/imgui/backends/*.d build/cov/*/*.d)

.PHONY: all sdl test coverage clean FORCE
