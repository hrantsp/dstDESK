from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout


class DstDeskConan(ConanFile):
    name = "dstdesk"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        # Official Qt, delivered by the local recipe in rec/qt-official.
        # ConanCenter's `qt` cannot supply a usable prebuilt binary — see decision 11
        # in dstOMNI/DESIGN.md. Run once per machine:
        #     conan create dstDESK/rec/qt-official
        self.requires("qt-official/6.8.3")

        # Qt's TLS support is a plugin that dynamically loads OpenSSL; Qt bundles
        # none. Requiring it here keeps wss:// from depending on what the host
        # happens to have installed — decision 12.
        # Shared, deliberately. Qt's TLS support is a plugin that opens libssl by
        # name at runtime, so a static OpenSSL cannot serve it however correctly it is
        # linked — there would be no file for the plugin to find. Decision 12.
        self.requires("openssl/3.6.3", options={"shared": True})

    def build_requirements(self):
        self.test_requires("catch2/3.15.3")

    def layout(self):
        # Build tree is bin/ rather than Conan's default build/, keeping every
        # directory in this repository to three letters.
        cmake_layout(self, build_folder="bin")

    def generate(self):
        CMakeDeps(self).generate()
        CMakeToolchain(self).generate()
