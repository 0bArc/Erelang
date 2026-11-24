"""Checks that every builtin in the registry has a runtime implementation.

Reads `erelang --dump-builtins --json` and looks for each dispatch name as a string literal in the
runtime sources. A name the registry promises but nothing implements would type-check and then do
nothing at run time.

    python tools/check_builtin_registry.py [path/to/erelang.exe]
"""
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    name = "erelang.exe" if os.name == "nt" else "erelang"
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "release", "bin", "Release", name)
    manifest = json.loads(subprocess.run([exe, "--dump-builtins", "--json"], capture_output=True, text=True,
                                         check=True).stdout)
    sources = []
    for pattern in ("src/**/*.cpp", "experimental/**/*.cpp"):
        for path in glob.glob(os.path.join(ROOT, pattern), recursive=True):
            if os.path.basename(path) == "builtin_registry.cpp":
                continue
            with open(path, encoding="utf-8", errors="ignore") as f:
                sources.append(f.read())
    text = "\n".join(sources)

    missing = set()
    for module in manifest["modules"]:
        for member in module["members"]:
            if f'"{member["builtin"]}"' not in text:
                missing.add((module["id"], member["builtin"]))
    for member in manifest["core"]:
        if f'"{member["builtin"]}"' not in text:
            missing.add(("core", member["builtin"]))
    for module_id, name in sorted(missing):
        print(f"missing implementation: {module_id}: {name}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
