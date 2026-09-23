# Thin orchestration over Conan + CMake presets.
#   make deps PRESET=dev-asan   # install third-party deps for the preset's build type
#   make build PRESET=dev-asan  # configure (if needed) + build
#   make test PRESET=dev-asan
#   make up                     # gateways + admind + the terminal, locally
#   make demo                   # end-to-end sim run with latency table
#   make bench                  # Release benchmarks
#   make db-up                  # the cold plane: postgres in docker
#   make db-migrate             # apply sql/migrations to it
PRESET ?= dev-asan

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

.PHONY: deps configure build test bench demo up dup web down fmt clean db-up db-down db-migrate db-shell

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
	PRESET=$(PRESET) ./scripts/dev_up.sh

# Postgres, the schema, and every backend process in one go; the terminal is
# `make web`, in its own shell.
dup: build db-up db-migrate
	@mkdir -p $${RAIKIRI_DATA_DIR:-/tmp/raikiri-dev}
	RAIKIRI_PG_DSN=$(PG_DSN) WEB=none PRESET=$(PRESET) ./scripts/dev_up.sh

web:
	@test -d frontend/node_modules || (cd frontend && npm install)
	cd frontend && npm run dev -- --host 127.0.0.1 --port $${WEB_PORT:-5173}

# For when a run was killed rather than stopped: a gateway that did not clean
# up leaves a segment whose owner is gone, and the next start refuses it.
down:
	-@pkill -f 'apps/gatewayd/gatewayd' 2>/dev/null || true
	-@pkill -f 'apps/admind/admind' 2>/dev/null || true
	-@pkill -f 'apps/persisterd/persisterd' 2>/dev/null || true
	-@pkill -f 'vite' 2>/dev/null || true
	@echo "stopped; segments in $${RAIKIRI_DATA_DIR:-/tmp/raikiri-dev}"

demo:
	@test -f build/deps/release/conan_toolchain.cmake || $(MAKE) deps PRESET=bench
	cmake --preset bench > /dev/null
	cmake --build --preset bench --target launcher gatewayd strategyd
	./build/bench/apps/launcher/launcher --duration 10

bench:
	$(MAKE) build PRESET=bench
	@echo "--- benchmarks in build/bench/bench/"

fmt:
	git ls-files '*.hpp' '*.cpp' | xargs clang-format -i

clean:
	rm -rf build

# --- the cold plane -----------------------------------------------------------
# Postgres only. It is not on the order path: nothing here is required to trade,
# and a gateway runs with the database down. What it holds is history — fills,
# the ledger, reconciliation breaks — which is why admind answers 503 rather
# than an empty page when it is missing.
PG_DSN ?= postgresql://raikiri:raikiri@localhost:5432/raikiri

db-up:
	docker compose up -d db
	@printf 'waiting for postgres'
	@until docker compose exec -T db pg_isready -U raikiri -d raikiri >/dev/null 2>&1; do \
		printf '.'; sleep 1; done; echo ' ready'
	@echo 'export RAIKIRI_PG_DSN=$(PG_DSN)'

# A SIBLING DATABASE FOR THE TESTS, so a test run can never write a row the
# terminal then reports as a position. `tests/integration` drains a real journal
# into Postgres and its rows are indistinguishable from trading once they land.
db-test-up:
	docker compose exec -T db psql -U raikiri -d postgres \
		-c "CREATE DATABASE raikiri_test" 2>/dev/null || true
	@for f in sql/migrations/*.sql; do \
		docker compose exec -T db psql -v ON_ERROR_STOP=1 -U raikiri -d raikiri_test < "$$f" >/dev/null || exit 1; \
	done
	@echo 'raikiri_test ready — pytest tests/integration -m pg'

db-test-down:
	docker compose exec -T db psql -U raikiri -d postgres -c "DROP DATABASE IF EXISTS raikiri_test"

db-down:
	docker compose down

# Applied through the container's own psql, so a working copy needs no local
# client. ON_ERROR_STOP because a migration that half-applies leaves a schema
# nobody can reason about; the files are written to be re-runnable.
db-migrate:
	@for f in sql/migrations/*.sql; do \
		echo "applying $$f"; \
		docker compose exec -T db psql -v ON_ERROR_STOP=1 -U raikiri -d raikiri < "$$f" >/dev/null || exit 1; \
	done
	@echo 'schema applied'

db-shell:
	docker compose exec db psql -U raikiri -d raikiri
