"""Conan recipe delivering official Qt binaries.

ConanCenter's `qt` recipe cannot supply a usable prebuilt binary for this project:

  * only two binaries exist per platform, and both are built with
    `qtwebsockets=False` — the module this project needs;
  * even matching settings and options exactly, those binaries are unreachable,
    because they were built against older transitive dependencies and today's
    resolution yields a different package_id
    (`conan graph explain`: "same settings and options, but different dependencies").

Either way, `qt` from ConanCenter means compiling Qt and its dependency tree from
source on every machine, including a reviewer's. This recipe keeps `conan install` as
the single entry point while fetching the official prebuilt Qt through aqtinstall.

Rationale and rejected alternatives are recorded in dstOMNI/DESIGN.md.
"""

import glob
import os
import sys

from conan import ConanFile
from conan.errors import ConanException, ConanInvalidConfiguration
from conan.tools.files import copy


class QtOfficialConan(ConanFile):
    name = "qt-official"
    version = "6.8.3"
    package_type = "shared-library"

    license = "LGPL-3.0-only"
    homepage = "https://www.qt.io"
    description = "Official prebuilt Qt binaries, fetched via aqtinstall"
    topics = ("qt", "gui", "websockets")

    settings = "os", "arch", "compiler", "build_type"
    options = {"modules": ["ANY"]}
    default_options = {"modules": "qtwebsockets"}

    # aqt host name per Conan os, and the architecture identifier it expects.
    # Verified against `aqt list-qt <host> desktop --arch <version>`.
    _AQT_HOST = {"Linux": "linux", "Windows": "windows", "Macos": "mac"}
    _AQT_ARCH = {
        ("Linux", "x86_64"): "linux_gcc_64",
        ("Windows", "x86_64"): "win64_msvc2022_64",
        ("Macos", "x86_64"): "clang_64",  # universal binary
        ("Macos", "armv8"): "clang_64",
    }

    def validate(self):
        key = (str(self.settings.os), str(self.settings.arch))
        if key not in self._AQT_ARCH:
            raise ConanInvalidConfiguration(
                f"No official Qt build is published for {key[0]}/{key[1]}. "
                f"Supported: {sorted(self._AQT_ARCH)}"
            )

    def package_id(self):
        # One official binary serves every compiler version and both build types on a
        # given platform, so those must not fragment the package id. The compiler
        # itself is kept: msvc and mingw Qt builds are not interchangeable.
        del self.info.settings.build_type
        del self.info.settings.compiler.version
        for sub in ("cppstd", "libcxx", "runtime", "runtime_type"):
            try:
                delattr(self.info.settings.compiler, sub)
            except Exception:
                pass

    def build(self):
        host = self._AQT_HOST[str(self.settings.os)]
        arch = self._AQT_ARCH[(str(self.settings.os), str(self.settings.arch))]
        outdir = os.path.join(self.build_folder, "qt")

        # `sys.executable -m aqt` binds to the interpreter running Conan, so aqtinstall
        # is picked up from the same environment Conan was installed into. Invoking a
        # bare `aqt` would depend on PATH ordering instead.
        cmd = (
            f'"{sys.executable}" -m aqt install-qt '
            f"{host} desktop {self.version} {arch} "
            f"-m {self.options.modules} -O \"{outdir}\""
        )
        self.output.info(f"Fetching official Qt: {cmd}")
        try:
            self.run(cmd)
        except ConanException as exc:
            raise ConanException(
                "aqtinstall failed. It must be installed in the same environment as "
                f"Conan: {sys.executable} -m pip install aqtinstall\n{exc}"
            ) from exc

        self._qt_prefix = self._find_prefix(outdir)
        self.output.info(f"Qt prefix: {self._qt_prefix}")

    def _find_prefix(self, outdir):
        """Locate the installed Qt prefix.

        aqt lays out <outdir>/<version>/<folder>, where <folder> does not match the
        architecture identifier passed in (linux_gcc_64 installs as gcc_64, clang_64 as
        macos). Rather than maintaining a second mapping that can silently rot, the
        prefix is discovered by looking for Qt's own CMake package config.
        """
        hits = glob.glob(os.path.join(outdir, "**", "lib", "cmake", "Qt6"), recursive=True)
        if not hits:
            raise ConanException(
                f"Qt was downloaded to {outdir} but no lib/cmake/Qt6 was found in it. "
                "The aqtinstall layout may have changed."
            )
        # .../<prefix>/lib/cmake/Qt6 -> <prefix>
        return os.path.dirname(os.path.dirname(os.path.dirname(hits[0])))

    def package(self):
        # Rediscover rather than relying on state from build(): `conan build` followed
        # by a separate `conan export-pkg` runs them in different processes.
        prefix = getattr(self, "_qt_prefix", None) or self._find_prefix(
            os.path.join(self.build_folder, "qt")
        )
        copy(self, "*", src=prefix, dst=self.package_folder, keep_path=True)

    def package_info(self):
        # Qt ships its own CMake package config; generating a competing one would
        # shadow it and lose Qt's tooling (moc, uic, deployment).
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs = ["."]

        self.buildenv_info.define_path("QT_ROOT_DIR", self.package_folder)
        self.buildenv_info.append_path("PATH", os.path.join(self.package_folder, "bin"))
        self.runenv_info.define_path(
            "QT_PLUGIN_PATH", os.path.join(self.package_folder, "plugins")
        )
