"""
@brief Copy nested class groups to top-level documentation paths.
@author Alex (<https://github.com/lextpf>)

`_clean_docs.py` links to these paths. Existing top-level destinations are skipped.
The optional command-line path defaults to `docs/`; group names come from `doxide.yml`
in the current working directory. Nested source directories remain available.
"""

import re
import shutil
import sys
from pathlib import Path


def parse_group_hierarchy(config_path: Path) -> list[tuple[str, str]]:
    """
    @fn parse_group_hierarchy(config_path: Path) -> list[tuple[str, str]]
    @brief Read parent-child group pairs from the expected config indentation.
    @author Alex (<https://github.com/lextpf>)

    This recognizes the repository's group layout with line patterns; it is not a
    YAML parser. Parent entries use two leading spaces, followed by nested groups.
    """
    text = config_path.read_text(encoding="utf-8")
    pairs = []
    parent_name = ""
    in_child_groups = False

    for line in text.split("\n"):
        stripped = line.rstrip()
        indent = len(line) - len(line.lstrip())

        m = re.match(r"^  - name:\s+(.+)", stripped)
        if m:
            parent_name = m.group(1).strip()
            in_child_groups = False
            continue

        if re.match(r"^\s{4,6}groups:\s*$", stripped):
            in_child_groups = True
            continue

        if in_child_groups and indent >= 6:
            m = re.match(r"^\s+- name:\s+(.+)", stripped)
            if m:
                pairs.append((parent_name, m.group(1).strip()))
                continue

        if indent < 4 and stripped and not stripped.startswith("#"):
            in_child_groups = False

    return pairs


def promote_class_to_index(top_level: Path, child_name: str) -> None:
    """
    @fn promote_class_to_index(top_level: Path, child_name: str) -> None
    @brief Use the same-named class page as the subgroup index.
    @author Alex (<https://github.com/lextpf>)

    If the class page exists, overwrite `index.md` with its contents and delete the class
    page. If it is absent, leave the directory unchanged.
    """
    class_page = top_level / f"{child_name}.md"
    index_page = top_level / "index.md"

    if not class_page.exists():
        return

    content = class_page.read_text(encoding="utf-8")
    index_page.write_text(content, encoding="utf-8")
    class_page.unlink()


def main():
    """
    @fn main()
    @brief Copy missing subgroup destinations and promote their class indexes.
    @author Alex (<https://github.com/lextpf>)

    Require the documentation directory and `doxide.yml`; missing inputs exit with status 1.
    Existing destinations are skipped, so regenerate the documentation tree before using
    this command to refresh promoted content.
    """
    docs_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("docs")
    config_path = Path("doxide.yml")

    if not config_path.exists():
        print("error: doxide.yml not found", file=sys.stderr)
        sys.exit(1)
    if not docs_dir.is_dir():
        print(f"error: {docs_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    pairs = parse_group_hierarchy(config_path)
    promoted = 0

    for parent_name, child_name in pairs:
        nested = docs_dir / parent_name / child_name
        top_level = docs_dir / child_name

        if nested.is_dir() and not top_level.exists():
            shutil.copytree(nested, top_level)
            promote_class_to_index(top_level, child_name)
            promoted += 1
            print(f"  {parent_name}/{child_name}/ -> {child_name}/")

    print(f"done: {promoted} subgroup(s) promoted")


if __name__ == "__main__":
    main()
