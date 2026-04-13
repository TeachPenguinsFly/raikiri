# Fuzzing

Four targets, all off by default because AppleClang does not ship the
libFuzzer runtime — a plain macOS build must not fail over it.

| target | what it guards |
|---|---|
| `bybit_wire_fuzz` | venue JSON parsers: untrusted input at line rate on a process holding live orders |
| `cryptocom_wire_fuzz` | the same for crypto.com, whose payload parsers read a `result.data` the envelope cut out and never checked the shape of |
| `journal_frame_fuzz` | journal recovery: runs at every startup, including the one after a crash, over bytes a failing disk may have mangled |
| `decimal_fuzz` | every venue price and quantity, plus a parse/format round-trip invariant |

## Running

```sh
make deps PRESET=fuzz          # clang toolchain (Homebrew locally, clang-18 in CI)
cmake --preset fuzz
cmake --build --preset fuzz --target fuzz_corpus bybit_wire_fuzz cryptocom_wire_fuzz
./build/fuzz/fuzz/bybit_wire_fuzz fuzz/corpus/bybit -max_total_time=300
./build/fuzz/fuzz/cryptocom_wire_fuzz fuzz/corpus/cryptocom -max_total_time=300
```

The corpus directory is writable by libFuzzer and therefore not tracked;
`fuzz_corpus` regenerates the seeds from the same captured frames the unit
tests use, cross-seeded so every payload reaches every parser.

## Reproducing a finding

libFuzzer writes the offending input to `crash-<hash>`; replay it with
`./build/fuzz/fuzz/<target> crash-<hash>`. Because the fuzz preset also
enables ASan and UBSan, a reproducer behaves identically under the normal
debug preset — add it to `tests/venues/bybit/fixtures/` and assert on it
there so the case stays covered after the fuzzer moves on.

## Note on instrumentation

Coverage instrumentation is applied to every first-party object
(`cmake/Sanitizers.cmake`), not just the fuzz target's own translation unit.
Without that, libFuzzer gets no feedback from the code under test; the
symptom is a fuzzer running at over a million executions per second whose
corpus never grows, and it is easy to mistake for a clean run.
