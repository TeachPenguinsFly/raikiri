# Thin orchestration over Conan + CMake presets.
#   make deps PRESET=dev-asan   # install third-party deps for the preset's build type
#   make build PRESET=dev-asan  # configure (if needed) + build
#   make test PRESET=dev-asan
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

.PHONY: deps configure build test bench demo fuzz fmt clean

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
