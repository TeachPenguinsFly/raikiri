# Thin orchestration over Conan + CMake presets.
#   make deps PRESET=dev-asan   # install third-party deps for the preset's build type
#   make build PRESET=dev-asan  # configure (if needed) + build
#   make test PRESET=dev-asan
#   make up                     # gateways + admind + the terminal, locally
#   make demo                   # end-to-end sim run with latency table
#   make bench                  # Release benchmarks
PRESET ?= dev-asan
FUZZ_SECONDS ?= 60

UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  PROFILE ?= profiles/macos-appleclang
else
  PROFILE ?= profiles/linux-gcc14
endif

ifeq ($(PRESET),rel)
  BUILD_TYPE := RelWithDebInfo
else ifeq ($(PRESET),bench)
  BUILD_TYPE := Release
else
  BUILD_TYPE := Debug
endif
DEPS_DIR := build/deps/$(shell echo $(BUILD_TYPE) | tr '[:upper:]' '[:lower:]')

.PHONY: deps configure build test bench demo up down fuzz fmt clean

deps:
	conan install . --output-folder=$(DEPS_DIR) --build=missing \
	  -pr:a $(PROFILE) -s build_type=$(BUILD_TYPE)

configure:
	cmake --preset $(PRESET)

build:
	@test -f build/$(PRESET)/CMakeCache.txt || cmake --preset $(PRESET)
	cmake --build --preset $(PRESET)

test: build
	ctest --preset $(PRESET)

# The whole stack against live venues. VENUES= picks which; credentials are
# optional and their absence is stated rather than fatal.
up: build
	@mkdir -p $${RAIKIRI_DATA_DIR:-/tmp/raikiri-dev}
	PRESET=$(PRESET) ./tools/dev_up.sh

# For when a run was killed rather than stopped: a gateway that did not clean
# up leaves a segment whose owner is gone, and the next start refuses it.
down:
	-@pkill -f 'apps/gatewayd/gatewayd' 2>/dev/null || true
	-@pkill -f 'apps/admind/admind' 2>/dev/null || true
	-@pkill -f 'vite' 2>/dev/null || true
	@echo "stopped; segments in $${RAIKIRI_DATA_DIR:-/tmp/raikiri-dev}"

demo:
	@test -f build/deps/release/conan_toolchain.cmake || $(MAKE) deps PRESET=bench
	cmake --preset bench > /dev/null
	cmake --build --preset bench --target launcher gatewayd strategyd
	./build/bench/apps/launcher/launcher --duration 10

fuzz:
	@test -f build/deps/fuzz/conan_toolchain.cmake || \
	  conan install . --output-folder=build/deps/fuzz --build=missing \
	    -pr:a profiles/macos-clang-fuzz -s build_type=Debug
	cmake --preset fuzz > /dev/null
	cmake --build --preset fuzz --target fuzz_corpus bybit_wire_fuzz journal_frame_fuzz decimal_fuzz
	./build/fuzz/fuzz/bybit_wire_fuzz fuzz/corpus/bybit -max_total_time=$(FUZZ_SECONDS)

bench:
	$(MAKE) build PRESET=bench
	@echo "--- benchmarks in build/bench/bench/"

fmt:
	git ls-files '*.hpp' '*.cpp' | xargs clang-format -i

clean:
	rm -rf build
