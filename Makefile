# Omatrack convenience Makefile
# ---------------------------------------------------------------------------
# The real build system is CMake + Ninja (see CMakePresets.json); this file
# only shortens the everyday commands. PRESET selects the preset
# (release|acceptance|debug|asan|ci) and BUILD_DIR follows it unless you
# override it explicitly.

PRESET    ?= release
BUILD_DIR ?= $(patsubst release,build,\
             $(patsubst acceptance,build-acceptance,\
             $(patsubst debug,build-debug,\
             $(patsubst asan,build-asan,\
             $(patsubst ci,build-ci,$(PRESET))))))

BINARY := $(BUILD_DIR)/omatrack
CLI    := $(BUILD_DIR)/omatrack-cli

PREFIX  ?= /usr/local
DESTDIR ?=

.PHONY: all build check lint memcheck install run run-debug clean distclean help

all: build

# -- build ----------------------------------------------------------------
# Configure with the selected preset on first run, then compile. Use the
# GUI under build/, or a CLI like `make build PRESET=debug`.
build:
	@test -f $(BUILD_DIR)/build.ninja || cmake --preset $(PRESET)
	cmake --build $(BUILD_DIR) --parallel

# -- check ----------------------------------------------------------------
# Unit tests, Rust crate tests, and the lint gates in one CTest run.
check: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# -- lint -----------------------------------------------------------------
# Just the formatting / qmllint / rust_clippy gates.
lint: build
	ctest --test-dir $(BUILD_DIR) -L lint --output-on-failure

# -- memcheck --------------------------------------------------------------
# ASan+UBSan unit tests. Valgrind is the wrong tool here: Qt Quick, libmpv,
# and the process-lifetime NetworkIo/CacheIndex objects drown it in noise.
# Skips headless-contract (needs the GUI binary).
memcheck:
	@test -f build-asan/build.ninja || cmake --preset asan
	cmake --build build-asan --parallel --target unit_tests
	ctest --preset asan -L unit -E headless-contract --output-on-failure

# -- install --------------------------------------------------------------
#   make install                  -> /usr/local
#   make install PREFIX=~/.local
#   make install DESTDIR=/tmp/stage PREFIX=/usr   (staged packaging)
install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

# -- run ------------------------------------------------------------------
# Launch the telemetry GUI on the desktop it is launched from. On Linux under
# Omarchy/Hyprland the window is re-anchored to the workspace the launching
# terminal is on, so it appears where you are even if omatrack.yml restored a
# stale monitor/workspace from a previous session. Elsewhere that is a plain
# exec.
#   make run                                  launch the telemetry GUI
#   make run ARGS="~/Documents/Telemetry"     open a telemetry folder
#   make run PRESET=debug                     another preset (see help)
run: build
	@scripts/run-gui.sh $(BINARY) $(ARGS)

# -- run-debug -------------------------------------------------------------
# Build the debug preset (./build-debug, QT_QML_DEBUG) and launch it on the
# current desktop. Inside a Herdr terminal the GUI runs from a find-or-create
# named right split titled "omatrack" so run logs stay visible; outside Herdr
# it degrades to the same-desktop launch used by `make run`.
#   make run-debug ARGS="~/Documents/Telemetry"
run-debug:
	@$(MAKE) --no-print-directory build PRESET=debug
	@scripts/run-gui.sh build-debug/omatrack --herdr $(ARGS)

# -- clean ----------------------------------------------------------------
# Remove the whole build tree (Ninja cannot know about removed sources).
clean:
	cmake -E remove_directory $(BUILD_DIR)

distclean: clean
	cmake -E remove_directory build-acceptance
	cmake -E remove_directory build-debug
	cmake -E remove_directory build-asan
	cmake -E remove_directory build-ci

help:
	@echo "omatrack targets: all build check lint memcheck install run run-debug clean distclean help"
	@echo "presets:         PRESET=release|acceptance|debug|asan|ci (default release)"
	@echo "run args:        ARGS=\"~/Documents/Telemetry\""
