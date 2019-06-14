genrule(
        name = "version",
        srcs = glob([".git/**"]),
        outs = ["version.txt"],
        local = True,
        cmd = """
                echo "Yosys 0.8+$$(git log --author=clifford@clifford.at --oneline 4d4665b.. 2> /dev/null | wc -l) (git sha1 $$(git rev-parse --short HEAD), built by Bazel)" > "$@"
        """,
        visibility = ["//visibility:public"],
)
