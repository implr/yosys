py_binary(
        name = "cellhelp",
        srcs = ["cellhelp.py",],
)

filegroup(
        name = "techmap_v",
        srcs = ["techmap.v"],
        visibility = ["//passes/techmap:__subpackages__"],
)

genrule(
        name = "simcells_help",
        srcs = ["simcells.v"],
        outs = ["simcells_help.inc"],
        tools = [":cellhelp"],
        cmd = "./$(location :cellhelp) $< > $@",
        visibility = ["//techlibs:__subpackages__"],
)
genrule(
        name = "simlib_help",
        srcs = ["simlib.v"],
        outs = ["simlib_help.inc"],
        tools = [":cellhelp"],
        cmd = "./$(location :cellhelp) $< > $@",
        visibility = ["//techlibs:__subpackages__"],
)

cc_library(
        name = "common",
        srcs = glob(["*.cc"]) + [
                "//techlibs/common:simcells_help",
                "//techlibs/common:simlib_help",
        ],
        hdrs = glob(["*.h"]),
        visibility = ["//visibility:public"],
        deps = [
                "//kernel:kernel",
        ],
)
