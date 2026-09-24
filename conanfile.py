from conan import ConanFile


class RaikiriConan(ConanFile):
    name = "raikiri"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    # asio and beast only, both header-only
    default_options = {"boost/*:header_only": True}

    def requirements(self):
        self.requires("boost/1.91.0")     # asio + beast only
        self.requires("openssl/3.6.3")    # TLS + HMAC (3.x: libpq caps at <4)
        self.requires("simdjson/4.6.3")   # hot-path venue frame parsing
        self.requires("glaze/7.8.4")      # typed config (cold path)
        self.requires("fmt/12.2.0")
        self.requires("quill/12.1.0")
        self.requires("sbepp/1.8.0")  # SBE codec support headers
        self.requires("crc32c/1.1.2")   # journal frame integrity
        self.requires("libpqxx/8.0.2")

    def build_requirements(self):
        self.tool_requires("sbepp/1.8.0")  # sbeppc schema compiler
        self.test_requires("gtest/1.17.0")
        self.test_requires("benchmark/1.9.5")
