"""
@brief Rewrite the Ninja compile database for clang-cl driver mode.
@author Alex (<https://github.com/lextpf>)

Convert GCC-style flags to the forms used by the clang-cl analysis driver.
Project include paths retain diagnostics; recognized external paths become system includes.
The optional path defaults to `build-cdb/compile_commands.json`; writes are in place.

| Input                        | Output           |
|------------------------------|------------------|
| `-isystem path`              | `-imsvc path`    |
| `-std=gnu++NN`, `-std=c++NN` | `/std:c++latest` |
| `-Ipath` in external code    | `-imsvc path`    |
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
    """
    @fn _is_third_party_include(path: str) -> bool
    @brief Recognize vendored paths by case-sensitive directory markers.
    @author Alex (<https://github.com/lextpf>)
    """
    if any(marker in path for marker in _THIRD_PARTY_MARKERS):
        return True
    # Also accept the marker directory without a trailing separator.
    return path.endswith(("/external", "\\external"))


def normalize_command_string(cmd: str) -> tuple[str, int]:
    """
    @fn normalize_command_string(cmd: str) -> tuple[str, int]
    @brief Normalize a joined compiler command.
    @author Alex (<https://github.com/lextpf>)

    Apply literal pattern substitutions without shell tokenization. Include paths must
    follow `-I` without whitespace; the path pattern stops at whitespace.

    @return The command and number of flag replacements.
    """
    changes = 0

    def replace_isystem(_m: re.Match) -> str:
        """
        @fn replace_isystem(_m: re.Match) -> str
        @brief Replace one system-include flag and increment the enclosing change count.
        @author Alex (<https://github.com/lextpf>)
        """
        nonlocal changes
        changes += 1
        return "-imsvc "

    cmd = _ISYSTEM_STRING_RE.sub(replace_isystem, cmd)

    def replace_std(_m: re.Match) -> str:
        """
        @fn replace_std(_m: re.Match) -> str
        @brief Select the latest C++ standard and increment the enclosing change count.
        @author Alex (<https://github.com/lextpf>)
        """
        nonlocal changes
        changes += 1
        return "/std:c++latest"

    cmd = _STD_STRING_RE.sub(replace_std, cmd)

    def replace_include(m: re.Match) -> str:
        """
        @fn replace_include(m: re.Match) -> str
        @brief Convert a recognized external include and count the replacement.
        @author Alex (<https://github.com/lextpf>)
        """
        nonlocal changes
        path = m.group(1)
        if _is_third_party_include(path):
            changes += 1
            return f"-imsvc {path}"
        return m.group(0)

    cmd = _INCLUDE_STRING_RE.sub(replace_include, cmd)
    return cmd, changes


def normalize_arguments_list(args: list[str]) -> tuple[list[str], int]:
    """
    @fn normalize_arguments_list(args: list[str]) -> tuple[list[str], int]
    @brief Normalize compiler argument tokens into a new list.
    @author Alex (<https://github.com/lextpf>)

    A separate `-isystem` token consumes its following path. Convert joined `-Ipath`
    for recognized external directories; leave separate `-I`, `path` tokens unchanged.

    @return The argument list and number of flag replacements.
    """
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
    """
    @fn main() -> int
    @brief Normalize the selected compile database in place.
    @author Alex (<https://github.com/lextpf>)

    Prefer `command` if an entry also contains `arguments`. Rewrite the JSON file even
    when no flag changes, using two-space indentation.

    @return Zero after writing the database, or one if the input path does not exist.
    """
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
