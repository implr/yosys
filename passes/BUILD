cc_library(
        name = "cmds",
        srcs = glob(["cmds/*.cc"]),
        visibility = ["//kernel:__subpackages__"],
        deps = [
                "//kernel:kernel",
                "//backends:ilang",
                "//frontends:ast",
                "//passes/techmap:techmap",
        ],
)
