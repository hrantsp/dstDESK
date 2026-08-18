from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, cmake_layout


class DstDeskConan(ConanFile):
    name = "kobayashi"
    settings = "os", "compiler", "build_type", "arch"

    def requirements(self):
        # Official Qt, delivered by the local recipe in rec/qt-official.
        # ConanCenter's `qt` cannot supply a usable prebuilt binary — see decision 11
        # in dstOMNI/DESIGN.md. Run once per machine:
        #     conan create dstDESK/rec/qt-official
        self.requires("qt-official/6.8.3")

        # Qt's TLS support is a plugin, and which plugins exist differs by platform.
        # Windows Qt ships a Schannel backend, so the platform's own TLS stack serves
        # wss:// and no OpenSSL is wanted: requiring it there costs a from-source
        # build of OpenSSL, because no prebuilt binary matches every MSVC release.
        # Elsewhere Qt offers only its OpenSSL backend, which opens libssl by name at
        # runtime and finds nothing unless the package carries it — hence shared,
        # since a static OpenSSL leaves no file for the plugin to open however
        # correctly it is linked. Decision 12.
        if self.settings.os != "Windows":
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
