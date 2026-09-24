# versions match the server: ubuntu 24.04 (glibc 2.39), gcc-14, cmake 3.28, conan 2.32
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

# build-essential stays: conan builds some dependencies with plain cc and make

# venv because ubuntu 24.04 refuses system pip; pinned to the server's conan
RUN python3 -m venv /opt/conan \
    && /opt/conan/bin/pip install --no-cache-dir --quiet 'conan==2.32.0'
ENV PATH=/opt/conan/bin:$PATH
