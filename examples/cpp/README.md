# C++ examples

Inside this repository they are targets of the main build:

```sh
cmake --build build/dev-asan --target example_place_and_cancel
```

On their own, against an installed SDK:

```sh
cmake -S examples/cpp -B build/examples -DCMAKE_PREFIX_PATH=<prefix> -DCMAKE_TOOLCHAIN_FILE=<conan_toolchain.cmake>
cmake --build build/examples
```

Start the paper deployment and paste its `export` lines, as in [the README above this one](../README.md), then run
`build/dev-asan/examples/cpp/example_place_and_cancel`. `common.hpp` connects from the environment and refuses a URL
that is not loopback unless `RAIKIRI_EXAMPLES_LIVE=yes`.

Handlers run on the stream's thread and REST calls block, so `market_maker.cpp` has its handlers store the latest book
and trades from the main loop.
