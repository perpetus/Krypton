# Copyright 2026 The Krypton Authors
# Use of this source code is governed by the GPL-3.0 license in LICENSE.
"""Compile Windows C/C++ with the same pinned inputs on local and SSH workers."""

load("@builtin//encoding.star", "json")
load("@builtin//struct.star", "module")
load("./chromium.star", chromium_init = "init")
load("./clang_all.star", "clang_all")
load("./mapping.star", "MAPPINGS")

def _compile(ctx, cmd):
    args = list(cmd.args)
    if args[0].replace("\\", "/").split("/")[-1].lower() in ["sccache", "sccache.exe"]:
        args = args[1:]
    fixed = []
    for arg in args:
        if arg.startswith("-imsvc"):
            original = arg[len("-imsvc"):].replace("\\", "/").lower()
            original = original.replace("//", "/")
            matched = False
            for source, destination in MAPPINGS:
                if original == source:
                    arg = "-imsvc" + destination
                    matched = True
                    break
            if not matched:
                fail("An SDK include was not staged: " + arg)
        fixed.append(arg)
    ctx.actions.fix(args = fixed)

def init(ctx):
    chromium = chromium_init(ctx)
    step = json.decode(chromium.step_config)
    step["input_deps"].update(clang_all.input_deps(ctx))
    # Generators, libraries and final links retain their Windows host paths.
    # This worker pool accepts only native clang-cl compilation actions.
    for rule in step["rules"]:
        rule["remote"] = False
    step["platforms"]["krypton-windows"] = {"OSFamily": "Windows"}
    inputs = [
        "third_party/llvm-build/Release+Asserts/bin/clang-cl.exe",
        "third_party/llvm-build/Release+Asserts:headers",
        "build/krypton_remote/toolchain:headers",
        "build/krypton_remote/toolchain-manifest.json",
    ]
    rules = []
    for action in ["cc", "cxx", "cxx_module"]:
        for prefix in ["sccache ", ""]:
            rules.append({
                "name": "krypton-clang/" + action + ("-cache-wrapper" if prefix else ""),
                "action": "(.*_)?" + action,
                "command_prefix": prefix + "..\\..\\third_party\\llvm-build\\Release+Asserts\\bin\\clang-cl.exe",
                "handler": "krypton_compile",
                "inputs": inputs,
                "platform_ref": "krypton-windows",
                "remote": True,
                "timeout": "15m",
            })
    step["rules"] = rules + step["rules"]
    groups = dict(chromium.filegroups)
    groups["build/krypton_remote/toolchain:headers"] = {"type": "glob", "includes": ["*"]}
    handlers = dict(chromium.handlers)
    handlers["krypton_compile"] = _compile
    return module("config", step_config = json.encode(step), filegroups = groups, handlers = handlers)
