cc_library(
        name = "ast",
        hdrs = glob(["ast/*.h"]),
        deps = [
                "//kernel:kernel",
        ],
        visibility = ["//visibility:public"],
)
cc_library(
        name = "ast_internal",
        srcs = glob(["ast/*.cc"]),
        deps = [
                ":ast",
                "//kernel:calc",
                "//frontends/verilog:verilog",
        ],
        copts = ["-Os", "-fno-ipa-cp-clone"],
)

cc_library(
        name = "blif",
        hdrs = glob(["blif/*.h"]),
        deps = [
                "//kernel:kernel",
        ],
        visibility = ["//visibility:public"],
)
cc_library(
        name = "blif_internal",
        srcs = glob(["blif/*.cc"]),
        deps = [
                ":blif",
        ],
)

cc_library(
        name = "all",
        deps = [
                ":ast_internal",
                ":blif_internal",
                "//frontends/verilog:internal",
        ],
        visibility = ["//kernel:__subpackages__"],
)
