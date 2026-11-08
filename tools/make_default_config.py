#!/usr/bin/env python3
"""Write Release/kufmod.yml from the DEFAULT_CONFIG text in src/kufmod.cpp, so the shipped file and
the one the mod writes on first start are always the same.

    py -3 make_default_config.py
"""
import ast
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, "src", "kufmod.cpp"), encoding="utf-8").read()
body = re.split(r'"\s*;', src.split("DEFAULT_CONFIG =", 1)[1], maxsplit=1)[0] + '"'
text = "".join(ast.literal_eval('"' + part + '"') for part in re.findall(r'"((?:[^"\\]|\\.)*)"', body))
out = os.path.join(ROOT, "Release", "kufmod.yml")
with open(out, "w", newline="\r\n") as f:
    f.write(text)
print("wrote %s (%d lines)" % (out, text.count("\n")))
