cc_library(
        name = "cmds",
        srcs = glob(["cmds/*.cc"]),
        visibility = ["//kernel:__subpackages__"],
        deps = [
                "//kernel:kernel",
                "//backends:rtlil",
                #"//frontends:ast",
                "//frontends/verilog:verilog",
                "//passes/techmap:techmap",
        ],
)
