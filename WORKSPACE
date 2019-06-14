load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "rules_m4",
    urls = ["https://github.com/jmillikin/rules_m4/releases/download/v0.1/rules_m4-v0.1.tar.xz"],
    sha256 = "7bb12b8a5a96037ff3d36993a9bb5436c097e8d1287a573d5958b9d054c0a4f7",
)

load("@rules_m4//m4:m4.bzl", "m4_register_toolchains")
m4_register_toolchains()

http_archive(
    name = "rules_flex",
    urls = ["https://github.com/jmillikin/rules_flex/releases/download/v0.1/rules_flex-v0.1.tar.xz"],
    sha256 = "361b14db1569d555afd2a69984e27b83ccfc63a3d50e9a7c15ac8fa973406d0d",
)

load("@rules_flex//flex:flex.bzl", "flex_register_toolchains")
flex_register_toolchains()

http_archive(
    name = "rules_bison",
    urls = ["https://github.com/jmillikin/rules_bison/releases/download/v0.1/rules_bison-v0.1.tar.xz"],
    sha256 = "5c57552a129b0d8eeb9252341ee975ec2720c35baf2f0d154756310c1ff572a0",
)

load("@rules_bison//bison:bison.bzl", "bison_register_toolchains")
bison_register_toolchains()
