cc_library(
        name = "ice40",
        srcs = glob(["*.cc"]),
        hdrs = glob(["*.h"]),
        deps = [
                "//kernel:kernel",
                "//passes/techmap:techmap",
        ],
        alwayslink = True,
        visibility = ["//techlibs:__pkg__"],
)
