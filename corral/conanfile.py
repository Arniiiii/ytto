from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout
from conan.tools.scm import Git 
from conan.tools.files import copy
from conan.tools.build import check_min_cppstd
import os


class corralRecipe(ConanFile):
    name = "corral"
    version = "0.20260206"

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}

    # def requirements(self):
    #     self.test_requires("catch2/2.13.9")
    #     self.test_requires("boost/<1.87.0")
    #     self.test_requires("qt/[~6]")

    def validate(self):
        check_min_cppstd(self, 20)

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["SKIP_TESTS"] = True
        tc.cache_variables["SKIP_EXAMPLES"] = True
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def source(self):
        git = Git(self)
        git.clone(url="https://github.com/hudson-trading/corral", target=".")
        git.checkout("282a82cf77f6fa2df9828ce1789e39773a749827")

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "COPYING", self.source_folder, os.path.join(self.package_folder, "licenses"))
        # rmdir(self, os.path.join(self.package_folder, "lib"))
        # rmdir(self, os.path.join(self.package_folder, "share"))

    def package_info(self):
        # For header-only packages, libdirs and bindirs are not used
        # so it's necessary to set those as empty.
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.set_property("cmake_file_name", "corral")
        self.cpp_info.set_property("cmake_target_name", "corral::corral")
        self.cpp_info.set_property("pkg_config_name", "corral")

    def package_id(self):
        self.info.clear()
