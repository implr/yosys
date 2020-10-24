cc_library(
        name = "kernel",
        hdrs = [
                "bitpattern.h",
                "cellaigs.h",
                "celledges.h",
                "celltypes.h",
                "consteval.h",
                "cost.h",
                "hashlib.h",
                "log.h",
                "macc.h",
                "modtools.h",
                "register.h",
                "rtlil.h",
                "satgen.h",
                "sigtools.h",
                "utils.h",
                "yosys.h",
                "constids.inc",
                "ffinit.h",
                "ff.h",
                "timinginfo.h",
        ],
        deps = [
                "//libs:sha1",
        ],
        defines = ["_YOSYS_"],
        visibility = ["//visibility:public"],
)

cc_library(
        name = "calc",
        srcs = ["calc.cc"],
        visibility = ["//visibility:public"],
        deps = [
                ":kernel",
                "//libs:bigint",
        ],
)

cc_library(
        name = "internal",
        srcs = [
                "cellaigs.cc",
                "celledges.cc",
                "log.cc",
                "macc.h",
                "register.cc",
                "rtlil.cc",
                "yosys.cc",
        ],
        copts = ["-Os", "-fno-ipa-cp-clone"],
        deps = [
                ":kernel",
                "//backends:ilang",
                "//frontends/verilog:verilog",
                "//libs:ezsat",
                "//passes:cmds",
                "//techlibs/common:common",
        ],
)

cc_binary(
        name = "driver",
        srcs = [
                "driver.cc",
                ":version",
        ],
        deps = [
                ":internal",
                "//libs:sha1",
                "//frontends:all",
                "//techlibs:all",
                "//frontends/verilog:parser",
                "//frontends/verilog:lexer",
        ],
        copts = ["-Os", "-fno-ipa-cp-clone"],
)

genrule(
        name = "version",
        outs = ["version.cc"],
        srcs = ["//:version"],
        cmd = """
                echo 'namespace Yosys { extern const char *yosys_version_str; const char *yosys_version_str = "'$$(cat $(location //:version))'"; }' > $@
        """,
)
