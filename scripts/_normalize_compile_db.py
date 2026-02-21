"""Normalize Ninja's clang-cl compile_commands.json for clang-tidy.

The Ninja generator + clang-cl combo emits a hybrid of gcc-style and
MSVC-style flags. clang-tidy forced into `--driver-mode=cl` (which we
need so it accepts `/MT`, `/EHa`, and friends) silently drops gcc-only
flags like `-isystem` and `-std=gnu++NN`, which loses our vcpkg include
paths and leaves the C++ standard unset.

This pass rewrites the JSON in-place:

  -isystem <path>          ->  -imsvc <path>      MSVC equivalent of -isystem.
  -std=gnu++NN | c++NN     ->  /std:c++latest     clang-cl C++ standard flag.
  -I<third-party-path>     ->  -imsvc <path>      Marks external/, _deps/,
                                                  and vcpkg_installed/ paths
                                                  as system. Suppresses
                                                  clang-tidy diagnostics
                                                  from headers we do not own
                                                  (CommonLibSSE-NG, vcpkg).

Project-owned `-I` paths (e.g. -Isrc) are left untouched so first-party
warnings still surface. Other gcc-style flags (-D, -Xclang, -O3,
-DNDEBUG) are accepted by clang-cl as-is.

Usage:
    python scripts/_normalize_compile_db.py            # build-cdb/compile_commands.json
    python scripts/_normalize_compile_db.py <path>     # custom path
"""

import json
import re
import sys
from pathlib import Path


_ISYSTEM_STRING_RE = re.compile(r"-isystem\s+")
_STD_STRING_RE = re.compile(r"-std=(?:gnu|c)\+\+\d+\b")
_INCLUDE_STRING_RE = re.compile(r'-I(\S+)')

_THIRD_PARTY_MARKERS = (
    "/external/",
    "\\external\\",
    "/_deps/",
    "\\_deps\\",
    "/vcpkg_installed/",
    "\\vcpkg_installed\\",
)


def _is_third_party_include(path: str) -> bool:
    """Heuristic: does this include path live in a vendored / fetched tree?"""
    if any(marker in path for marker in _THIRD_PARTY_MARKERS):
        return True
    # Cover the case where the path ends at the marker directory itself,
    # e.g. -IC:/.../external without a trailing slash.
    return path.endswith(("/external", "\\external"))


def normalize_command_string(cmd: str) -> tuple[str, int]:
    """Return (new_command, change_count). Operates on the joined string form."""
    changes = 0

    def replace_isystem(_m: re.Match) -> str:
        nonlocal changes
        changes += 1
        return "-imsvc "

    cmd = _ISYSTEM_STRING_RE.sub(replace_isystem, cmd)

    def replace_std(_m: re.Match) -> str:
        nonlocal changes
        changes += 1
        return "/std:c++latest"

    cmd = _STD_STRING_RE.sub(replace_std, cmd)

    def replace_include(m: re.Match) -> str:
        nonlocal changes
        path = m.group(1)
        if _is_third_party_include(path):
            changes += 1
            return f"-imsvc {path}"
        return m.group(0)

    cmd = _INCLUDE_STRING_RE.sub(replace_include, cmd)
    return cmd, changes


def normalize_arguments_list(args: list[str]) -> tuple[list[str], int]:
    """Return (new_args, change_count). Operates on the arguments-array form."""
    out: list[str] = []
    changes = 0
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-isystem" and i + 1 < len(args):
            out.append("-imsvc")
            out.append(args[i + 1])
            i += 2
            changes += 1
            continue
        if _STD_STRING_RE.fullmatch(a):
            out.append("/std:c++latest")
            i += 1
            changes += 1
            continue
        if a.startswith("-I") and len(a) > 2:
            path = a[2:]
            if _is_third_party_include(path):
                out.append("-imsvc")
                out.append(path)
                i += 1
                changes += 1
                continue
        out.append(a)
        i += 1
    return out, changes


def main() -> int:
    cdb_path = Path(sys.argv[1] if len(sys.argv) > 1 else "build-cdb/compile_commands.json")
    if not cdb_path.exists():
        print(f"error: {cdb_path} not found", file=sys.stderr)
        return 1

    entries = json.loads(cdb_path.read_text(encoding="utf-8"))
    total_changes = 0

    for entry in entries:
        if "command" in entry:
            new_cmd, n = normalize_command_string(entry["command"])
            if n:
                entry["command"] = new_cmd
                total_changes += n
        elif "arguments" in entry:
            new_args, n = normalize_arguments_list(entry["arguments"])
            if n:
                entry["arguments"] = new_args
                total_changes += n

    cdb_path.write_text(json.dumps(entries, indent=2), encoding="utf-8")
    print(f"normalized {total_changes} flag(s) across {len(entries)} entries in {cdb_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
