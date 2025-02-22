from conan import ConanFile


class RaikiriConan(ConanFile):
    name = "raikiri"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    def requirements(self):
        # Later phases add: boost (asio+beast), openssl, simdjson, abseil,
        # libcurl, yyjson, glaze.
        self.requires("fmt/12.2.0")
        self.requires("quill/12.1.0")
        self.requires("sbepp/1.8.0")  # SBE codec support headers
        self.requires("crc32c/1.1.2")   # journal frame integrity

    def build_requirements(self):
        self.tool_requires("sbepp/1.8.0")  # sbeppc schema compiler
        self.test_requires("gtest/1.17.0")
        self.test_requires("benchmark/1.9.5")
