# CI

Five workflows in `.github/workflows/`. Each one exists to catch a failure the
others structurally cannot.

| workflow | what it runs | the failure it catches |
|---|---|---|
| `build.yml` | ubuntu-24.04, {gcc-14, clang-18} × {dev-asan, rel}, plus clang-18/dev-tsan | UB and memory errors under ASan/UBSan; data races in the shm rings under TSan; anything that compiles under one standard library but not the other (libstdc++ vs libc++); any new warning (`-Werror` is on for every preset except `bench`) |
| `macos.yml` | macos-14, AppleClang, `dev-asan`, then `make demo` | the portability layer rotting: file-backed mmap instead of tmpfs, `F_PREALLOCATE` instead of `fallocate`, `pin_thread()` returning `ENOTSUP`. A Linux-only change that breaks the documented dev platform |
| `integration.yml` | ubuntu-24.04 + a `postgres:16` service, full ctest with `RAIKIRI_PG_DSN` set, then the demo and a double drain of its journal | replay non-idempotence. Without a DSN the persister integration test is a `GTEST_SKIP`, so a missing `ON CONFLICT` target or a natural key that is not unique passes everywhere else |
| `fuzz.yml` | clang-18 + libFuzzer; 60s/target on PRs, 900s/target nightly | crashes and UB in the three untrusted-input boundaries: venue JSON, journal frame recovery, decimal parse/format |
| `lint.yml` | clang-format (whole tree), clang-tidy (changed files), include-layering grep | formatting drift; tidy findings attached to the commit that introduced them; a strategy reaching into venue or transport headers |

## Local equivalents

Every red job reproduces without pushing.

```sh
make deps PRESET=dev-asan && make test PRESET=dev-asan   # build.yml, ASan/UBSan entries
make deps PRESET=dev-tsan && make test PRESET=dev-tsan   # build.yml, TSan entry
make deps PRESET=rel      && make test PRESET=rel        # build.yml, rel entries
make demo                                                # macos.yml / integration.yml demo step
make fuzz FUZZ_SECONDS=60                                # fuzz.yml smoke run (bybit target)
make fmt                                                 # fixes what lint.yml's format job reports
```

For the integration job, point at any Postgres and the skipped test wakes up:

```sh
export RAIKIRI_PG_DSN=postgresql://postgres:postgres@localhost:5432/raikiri_ci
psql "$RAIKIRI_PG_DSN" -v ON_ERROR_STOP=1 -f sql/migrations/0001_init.sql
make test PRESET=dev-asan
```

## Differences that bite

- **ASan on Linux includes LeakSanitizer; on macOS it does not.** A leak found
  by `build.yml` will not reproduce with `make test` on a Mac. Reproduce it on
  Linux, or in a Linux container.
- **UBSan halts on the first finding in CI** (`UBSAN_OPTIONS=halt_on_error=1`).
  Locally it prints and continues, so a green local `make test` can be sitting
  on undefined behaviour. Export the same option to match.
- **CI pins the profile, not the machine.** `conan profile detect` is never run;
  `profiles/linux-gcc14` and `profiles/linux-clang18` fix os, arch, compiler,
  libcxx and cppstd. Use `make deps PROFILE=profiles/<name>` locally to build
  the same package ids.
- **`fuzz.yml` does not use the `fuzz` preset.** That preset resolves its
  toolchain from `profiles/macos-clang-fuzz`, which is macOS/armv8 and hard-wired
  to Homebrew LLVM — correct locally, where the point is to bypass AppleClang's
  missing libFuzzer runtime, and unusable on a Linux runner. CI installs
  `profiles/linux-clang18` and configures with an explicit `-DRAIKIRI_BUILD_FUZZ=ON`
  instead. `make fuzz` remains the local command; the binaries land in the same
  `build/fuzz/fuzz/` path either way, so `fuzz/README.md`'s reproduce
  instructions apply to CI artifacts unchanged.
- **clang-format and clang-tidy are pinned to 18.** `make fmt` calls whatever
  `clang-format` is on your PATH; if that is not 18, expect the CI check to
  disagree with it.
- **clang-tidy only sees changed `.cpp` files** (plus the sources of a library
  whose headers changed). Header-only libraries — `strategy`, `strategies`,
  `observ`, `schema` — have no sources and are the known gap.

## Fuzz findings

A crash uploads as an artifact (`fuzz-repro-<target>-<run_id>`) containing the
`crash-*`/`leak-*`/`timeout-*` reproducer and the corpus that produced it. The
fuzz build carries ASan and UBSan, so the reproducer behaves identically under
`dev-asan`: replay it, then move it into `tests/venues/bybit/fixtures/` and
assert on it there so the case stays covered after the fuzzer moves on.

The nightly corpus is cached per target and carried between runs; PR runs
neither restore nor save it, so a branch cannot poison the baseline.
