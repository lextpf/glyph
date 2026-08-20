"""
@brief Prepare generated Markdown for the site build.
@author Alex (<https://github.com/lextpf>)

Rewrite only Markdown whose first 200 characters contain `generator: doxide`.
The optional command-line path defaults to `docs/`. The home page gets a member list
and a version from the sibling `src/Version.hpp`; changed files are written in place.
"""

import re
import sys
from pathlib import Path


def is_doxide_generated(text: str) -> bool:
    """
    @fn is_doxide_generated(text: str) -> bool
    @brief Check for the generator marker near the start of a page.
    @author Alex (<https://github.com/lextpf>)

    This checks the first 200 characters; it does not parse YAML frontmatter.
    """
    return "generator: doxide" in text[:200]


def fix_admonition_indent(text: str) -> str:
    """
    @fn fix_admonition_indent(text: str) -> str
    @brief Normalize generated admonition bodies to four-space indentation.
    @author Alex (<https://github.com/lextpf>)

    An unindented line ends the body. Nested indentation is flattened, so use this only
    for the generated admonition layout.
    """
    lines = text.split("\n")
    result = []
    in_admonition = False

    for line in lines:
        if re.match(r"^!!! \w+", line):
            in_admonition = True
            result.append(line)
            continue

        if in_admonition:
            m = re.match(r"^ (\S.*)", line)
            if m:
                result.append("    " + m.group(1))
                continue
            if line.startswith("  "):
                result.append("    " + line.lstrip())
                continue
            in_admonition = False

        result.append(line)

    return "\n".join(result)


PAGE_TITLE_ICONS = {
    "Core":           ":material-cube-outline:",
    "Rendering":      ":material-palette:",
    "Configuration":  ":material-cog-outline:",
    "Hooks":          ":material-hook:",
    "Utilities":      ":material-toolbox-outline:",
}


SECTION_ICONS = {
    "Types":               ":material-shape-outline:",
    "Functions":           ":material-function:",
    "Variables":           ":material-variable:",
    "Macros":              ":material-pound:",
    "Operators":           ":material-math-compass:",
    "Type Aliases":        ":material-link-variant:",
    "Type Details":        ":material-shape-outline:",
    "Type Alias Details":  ":material-link-variant:",
    "Function Details":    ":material-function:",
    "Variable Details":    ":material-variable:",
    "Macro Details":       ":material-pound:",
    "Operator Details":    ":material-math-compass:",
}


def add_page_title_icons(text: str) -> str:
    """
    @fn add_page_title_icons(text: str) -> str
    @brief Add icons to recognized generated page titles.
    @author Alex (<https://github.com/lextpf>)

    Match complete title lines and change at most one occurrence of each title.
    """
    for title, icon in PAGE_TITLE_ICONS.items():
        text = re.sub(rf"^# {re.escape(title)}$", f"# {icon} {title}", text, count=1, flags=re.MULTILINE)
    return text


def add_section_icons(text: str) -> str:
    """
    @fn add_section_icons(text: str) -> str
    @brief Add icons to recognized generated section headings.
    @author Alex (<https://github.com/lextpf>)
    """
    for title, icon in SECTION_ICONS.items():
        text = text.replace(f"## {title}", f"## {icon} {title}")
    return text


def trim_function_table_descriptions(text: str) -> str:
    """
    @fn trim_function_table_descriptions(text: str) -> str
    @brief Keep the first sentence in function summary rows.
    @author Alex (<https://github.com/lextpf>)

    Full descriptions remain in the function details. A period followed by whitespace
    ends the summary; this helper does not parse Markdown inline syntax.
    """
    lines = text.split("\n")
    out = []
    in_functions_table = False

    for line in lines:
        stripped = line.strip()

        if stripped in {"## Functions", "## :material-function: Functions"}:
            in_functions_table = True
            out.append(line)
            continue

        if in_functions_table and stripped.startswith("## "):
            in_functions_table = False
            out.append(line)
            continue

        if in_functions_table and stripped.startswith("| [") and stripped.endswith("|"):
            parts = [p.strip() for p in stripped.strip("|").split("|", 1)]
            if len(parts) == 2:
                name_col, desc_col = parts
                desc_col = re.sub(r"\s+", " ", desc_col).strip()
                m = re.match(r"^(.*?\.)\s+.*$", desc_col)
                brief = m.group(1) if m else desc_col
                out.append(f"| {name_col} | {brief} |")
                continue

        out.append(line)

    return "\n".join(out)


def flatten_namespace_lists(text: str) -> str:
    """
    @fn flatten_namespace_lists(text: str) -> str
    @brief Keep namespace names and descriptions on one list line.
    @author Alex (<https://github.com/lextpf>)

    Consume an immediately following definition line and at most one blank separator.
    """
    lines = text.split("\n")
    out = []
    i = 0

    while i < len(lines):
        line = lines[i]
        term = line.strip()
        if term.startswith(":material-package:") or term.startswith(":material-format-section:"):
            desc = ""
            if i + 1 < len(lines):
                m = re.match(r"^:\s+(.*)$", lines[i + 1])
                if m:
                    desc = m.group(1).strip()
                    i += 1

            if desc:
                out.append(f"- {term} - {desc}")
            else:
                out.append(f"- {term}")

            i += 1
            if i < len(lines) and lines[i].strip() == "":
                i += 1
            continue

        out.append(line)
        i += 1

    return "\n".join(out)


def collect_members(index_path: Path, prefix: str) -> list[tuple[str, str, str]]:
    """
    @fn collect_members(index_path: Path, prefix: str) -> list[tuple[str, str, str]]
    @brief Read member rows from a group index.
    @author Alex (<https://github.com/lextpf>)

    @param prefix Path from the documentation root, including its trailing slash.
    @return Tuples of name, path, and description; an empty list if the index is missing.
    """
    if not index_path.exists():
        return []
    text = index_path.read_text(encoding="utf-8")
    results = []

    for m in re.finditer(
        r"^\| \[([^\]]+)\]\(([^)]+)\) \|(.+)\|",
        text,
        re.MULTILINE,
    ):
        name = m.group(1)
        rel_path = m.group(2).strip()
        desc = m.group(3).strip()
        desc = re.sub(r"^@brief\s+", "", desc)
        # Anchors resolve against the group index.
        if rel_path.startswith("#"):
            full_path = f"{prefix}index.md{rel_path}"
        else:
            full_path = f"{prefix}{rel_path}"
        results.append((name, full_path, desc))

    return results


def collect_group_members(docs_dir: Path, group_dir: str) -> list[tuple[str, str, str]]:
    """
    @fn collect_group_members(docs_dir: Path, group_dir: str) -> list[tuple[str, str, str]]
    @brief Include direct members and one level of subgroups.
    @author Alex (<https://github.com/lextpf>)

    If a nested subgroup has no member rows, try its top-level namespace index.
    No recursive subgroup traversal occurs.
    """
    group_index = docs_dir / group_dir / "index.md"
    prefix = f"{group_dir}/"
    members = []

    members.extend(collect_members(group_index, prefix))

    if group_index.exists():
        text = group_index.read_text(encoding="utf-8")
        for m in re.finditer(
            r":material-format-section: \[([^\]]+)\]\(([^)]+)/index\.md\)",
            text,
        ):
            sub_dir = m.group(2)
            sub_index = docs_dir / group_dir / sub_dir / "index.md"
            sub_members = collect_members(sub_index, f"{group_dir}/{sub_dir}/")
            if sub_members:
                members.extend(sub_members)
                continue
            # Namespace content lives at the top level; nested indexes can be empty stubs.
            top_index = docs_dir / sub_dir / "index.md"
            members.extend(collect_members(top_index, f"{sub_dir}/"))

    return members


def inject_group_members(text: str, docs_dir: Path) -> str:
    """
    @fn inject_group_members(text: str, docs_dir: Path) -> str
    @brief Replace the home page member list from group indexes.
    @author Alex (<https://github.com/lextpf>)

    Remove existing package entries, then insert collected members after the last group.
    Repeated calls replace the injected list.
    """
    lines = text.split("\n")
    lines = [l for l in lines if not re.match(r"^-?\s*- :material-package:", l)]

    out = []
    all_members = []
    last_group_idx = -1

    for line in lines:
        out.append(line)

        m = re.match(
            r"^- :material-format-section: \[.*\]\(([^/]+)/index\.md\)",
            line,
        )
        if not m:
            continue

        last_group_idx = len(out) - 1
        group_dir = m.group(1)
        members = collect_group_members(docs_dir, group_dir)
        for name, path, desc in members:
            sentence = re.match(r"^(.*?\.)\s", desc)
            brief = sentence.group(1) if sentence else desc
            all_members.append(f"- :material-package: [{name}]({path}) - {brief}")

    if all_members and last_group_idx >= 0:
        insert_at = last_group_idx + 1
        out.insert(insert_at, "")
        for j, entry in enumerate(all_members):
            out.insert(insert_at + 1 + j, entry)

    return "\n".join(out)


def parse_version(repo_root: Path) -> str:
    """
    @fn parse_version(repo_root: Path) -> str
    @brief Read the numeric version macros from the source tree.
    @author Alex (<https://github.com/lextpf>)

    @return MAJOR.MINOR.PATCH, or an empty string if the header or a component is missing.
    """
    version_h = repo_root / "src" / "Version.hpp"
    if not version_h.exists():
        return ""
    content = version_h.read_text(encoding="utf-8")
    parts = {}
    for key in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(rf"#define\s+GLYPH_VERSION_{key}\s+(\d+)", content)
        if m:
            parts[key] = m.group(1)
    if len(parts) == 3:
        return f"{parts['MAJOR']}.{parts['MINOR']}.{parts['PATCH']}"
    return ""


def inject_version(text: str, version: str) -> str:
    """
    @fn inject_version(text: str, version: str) -> str
    @brief Insert the version in the home page subtitle once.
    @author Alex (<https://github.com/lextpf>)

    Leave the text unchanged if the version is empty or already appears in bold.
    """
    if not version or f"**v{version}**" in text:
        return text
    return re.sub(
        r"^(# glyph)\n\n(.+)$",
        rf"\1\n\n**v{version}** | \2",
        text,
        count=1,
        flags=re.MULTILINE,
    )


def clean(text: str) -> str:
    """
    @fn clean(text: str) -> str
    @brief Apply generated-page cleanup in the required order.
    @author Alex (<https://github.com/lextpf>)

    Remove unsupported prose tags before normalizing admonitions, adding icons,
    shortening function summaries, and flattening namespace lists. This helper does
    not read or write files.
    """
    text = re.sub(r"^\s*@author\b.*\n?", "", text, flags=re.MULTILINE)

    text = re.sub(r"@brief\s+", "", text)
    text = re.sub(r"@details\s*\n?", "", text)

    text = fix_admonition_indent(text)

    text = add_page_title_icons(text)

    text = add_section_icons(text)

    text = trim_function_table_descriptions(text)

    text = flatten_namespace_lists(text)

    return text


def main():
    """
    @fn main()
    @brief Clean generated pages and update home-page navigation.
    @author Alex (<https://github.com/lextpf>)

    Read group indexes from the supplied documentation tree. Write a file only if its
    cleaned contents differ; handwritten pages without the generator marker are skipped.
    """
    docs_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("docs")

    if not docs_dir.is_dir():
        print(f"error: {docs_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    repo_root = docs_dir.resolve().parent
    version = parse_version(repo_root)
    if version:
        print(f"  version: {version}")

    changed = 0
    for md in docs_dir.rglob("*.md"):
        original = md.read_text(encoding="utf-8")
        if not is_doxide_generated(original):
            continue

        cleaned = clean(original)

        is_home = md.name == "index.md" and md.parent == docs_dir

        if is_home:
            cleaned = inject_group_members(cleaned, docs_dir)
            if version:
                cleaned = inject_version(cleaned, version)
        else:
            # Rewrite only the index preamble, before member sections.
            parts = cleaned.split("\n## ", 1)
            parts[0] = re.sub(
                r"^- :material-format-section:",
                "- :material-package:",
                parts[0],
                flags=re.MULTILINE,
            )
            # Subgroup content lives one directory above the nested stub.
            parts[0] = re.sub(
                r"\]\((\w+/index\.md)\)",
                r"](../\1)",
                parts[0],
            )
            cleaned = "\n## ".join(parts)

        if cleaned != original:
            md.write_text(cleaned, encoding="utf-8")
            changed += 1
            print(f"  cleaned {md.relative_to(docs_dir)}")

    print(f"done: {changed} file(s) cleaned")


if __name__ == "__main__":
    main()
