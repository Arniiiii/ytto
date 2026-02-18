from conan import ConanFile


class CompressorRecipe(ConanFile):
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeToolchain", "CMakeDeps"
    
    def configure(self):
        self.options["boost"].with_stacktrace = True
        self.options["boost"].without_url = False

    def requirements(self):
        self.requires("corral/[~0]")
        # self.requires("openssl/[~3]")
        self.requires("boost/[>=1.88.0 <1.90.0]")
        # self.requires("gtest/[~1]")
        self.requires("fmt/[~12]")
        self.requires("args-parser/[~6]")
        self.requires("magic_enum/[~0]")
        self.requires("inja/[~3]")
        self.requires("quill/[~11]")
        self.requires("glaze/[~7]")
        self.requires("openssl/[~3]")

    def build_requirements(self):
        self.tool_requires("cmake/3.27.9")
