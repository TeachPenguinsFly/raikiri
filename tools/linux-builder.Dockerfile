# The Linux toolchain, reproduced so a build need not happen on the server.
#
# THE SERVER IS TWO vCPUs AND ALSO RECORDS FOUR VENUES. Measured 2026-09-18:
# a full build there ran at one translation unit a minute against the running
# gateways — seven hours — and at seven a minute with them stopped, which
# costs a hole in the recording. The same build on a fourteen-core desk under
# x86_64 emulation is about five times faster than the free server and twenty
# times faster than the busy one; emulation itself costs only 1.5x, measured
# on one heavy translation unit at 29.3 s against 19.3 native.
#
# EVERY VERSION HERE MATCHES THE SERVER and that is the whole point of pinning
# them: Ubuntu 24.04 for glibc 2.39, gcc-14, cmake 3.28, conan 2.32. A binary
# built against a newer glibc links fine here and dies there with a symbol
# version error, which is the one failure this file exists to prevent.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        build-essential \
        ca-certificates \
        cmake \
        g++-14 \
        gcc-14 \
        git \
        libtool \
        ninja-build \
        pkg-config \
        python3 \
        python3-venv \
    && rm -rf /var/lib/apt/lists/*

# THE UNVERSIONED COMPILER IS NOT A SPARE, it is what the dependencies build
# themselves with. The conan profile points the PROJECT at gcc-14, but conan
# also builds some packages from their own recipes, and those bring their own
# build systems: crc32c wants Unix Makefiles, flex runs an autotools configure
# that looks for plain `cc`. build-essential supplies both, and it supplies
# gcc-13 as the default exactly as the server has it — so the dependency set
# is built by the same compiler there and here.

# In a venv because Ubuntu 24.04 refuses pip into the system python (PEP 668),
# and pinned because a conan that resolves different package ids than the
# server's would silently build a different dependency set.
RUN python3 -m venv /opt/conan \
    && /opt/conan/bin/pip install --no-cache-dir --quiet 'conan==2.32.0'
ENV PATH=/opt/conan/bin:$PATH
