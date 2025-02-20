from conan import ConanFile


class RaikiriConan(ConanFile):
    name = "raikiri"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Later phases add: boost (asio+beast), openssl, simdjson, abseil,
        # libcurl, yyjson, glaze.
        self.requires("fmt/12.2.0")
        self.requires("crc32c/1.1.2")   # journal frame integrity
        self.requires("quill/12.1.0")

    def build_requirements(self):
        self.test_requires("gtest/1.17.0")
        self.test_requires("benchmark/1.9.5")
