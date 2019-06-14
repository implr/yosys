cc_library(
        name = "verilog",
        hdrs = glob(["*.h"]),
        deps = [
                "//kernel:kernel",
                "//frontends:ast",
        ],
        visibility = ["//visibility:public"],
)
cc_library(
        name = "internal",
        srcs = glob(["*.cc"]),
        deps = [
                ":verilog",
        ],
        alwayslink = True,
        visibility = [
                "//frontends:__pkg__",
        ],
)

genrule(
        name = "parser_gen",
        srcs = ["verilog_parser.y"],
        outs = ["verilog_parser.tab.cc", "verilog_parser.tab.hh"],
        cmd = """
                M4=$(M4) $(BISON) -o $(location verilog_parser.tab.cc) -d -r all -b frontends/verilog/verilog_parser $(location verilog_parser.y)
        """,
        toolchains = [
                "@rules_bison//bison:toolchain",
                "@rules_m4//m4:toolchain",
        ],
)

cc_library(
        name = "parser",
        srcs = [":parser_gen"],
        copts = ["-DYYMAXDEPTH=10000000"],
        deps = [
                ":verilog",
        ],
        visibility = [
                "//kernel:__pkg__",
        ],
)

genrule(
        name = "lexer_gen",
        srcs = [
                "verilog_lexer.l",
        ],
        outs = ["verilog_lexer.cc"],
        cmd = """
                M4=$(M4) $(FLEX) -o "$@" $(location verilog_lexer.l)
        """,
        toolchains = [
                "@rules_flex//flex:toolchain",
                "@rules_m4//m4:toolchain",
        ],
)

cc_library(
        name = "lexer",
        srcs = [":lexer_gen", ":parser_gen"],
        deps = [
                ":verilog",
        ],
        visibility = [
                "//kernel:__pkg__",
        ],
)

