cc_library(
        name = "bigint",
        srcs = glob(["bigint/*.cc"], exclude=["bigint/sample.cc", "bigint/testsuite.cc"]),
        hdrs = glob(["bigint/*.h"]) + glob(["bigint/*.hh"]),
        visibility = ["//visibility:public"],
)
cc_library(
        name = "sha1",
        srcs = ["sha1/sha1.cpp"],
        hdrs = ["sha1/sha1.h"],
        visibility = ["//visibility:public"],
)
cc_library(
        name = "subcircuit",
        srcs = [
                "subcircuit/subcircuit.cc",
        ],
        hdrs = [
                "subcircuit/subcircuit.h",
        ],
        visibility = ["//visibility:public"],
)
cc_library(
        name = "minisat",
        srcs = glob(["minisat/*.cc"]),
        hdrs = glob(["minisat/*.h"]),
        visibility = ["//visibility:public"],
)
cc_library(
        name = "ezsat",
        srcs = glob(["ezsat/*.cc"], exclude=["ezsat/demo*.cc", "ezsat/puzzle3d.cc", "ezsat/testbench.cc"]),
        hdrs = glob(["ezsat/*.h"]),
        visibility = ["//visibility:public"],
        deps = [":minisat"],
)
