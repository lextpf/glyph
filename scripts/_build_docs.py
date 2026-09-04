"""
@brief Build API pages through the Doxide metadata compatibility adapter.
@author Alex (<https://github.com/lextpf>)

Doxide 0.9 consumes only one token after a function metadata tag. Keep complete
signatures in source documentation and omit that metadata from temporary parser input.
Use the repository configuration and write pages to its configured output directory.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml


_CPP_TOKENS = re.compile(
    r'R"(?P<delimiter>[^\\\s()]{0,16})\(.*?\)(?P=delimiter)"'
    r"|(?:\b\d|\.\d)(?:[\w.]|'(?=\w)|(?<=[eEpP])[+-])*"
    r'|"(?:\\.|[^"\\\r\n])*"|\'(?:\\.|[^\'\\\r\n])*\''
    r'|//(?:\\\r?\n|[^\n])*|/\*.*?\*/',
    re.DOTALL,
)
_FN_METADATA = re.compile(
    r"\A(/\*\*[ \t]*\r?\n)([ \t]*\*[ \t]+@fn\b.*?)"
    r"(?=^[ \t]*\*[ \t]+@brief\b)",
    re.MULTILINE | re.DOTALL,
)


def prepare_source(text: str) -> str:
    """
    @fn prepare_source(text: str) -> str
    @brief Omit leading callable metadata from structured C++ comments.
    @author Alex (<https://github.com/lextpf>)

    Preserve line numbers, literals, ordinary comments, and documentation examples.
    Only the required metadata span before the brief is removed.
    """

    def replace_token(match: re.Match) -> str:
        """
        @fn replace_token(match: re.Match) -> str
        @brief Adapt a structured comment while preserving other C++ tokens.
        @author Alex (<https://github.com/lextpf>)
        """
        token = match.group(0)
        if not token.startswith("/**"):
            return token
        return _FN_METADATA.sub(
            lambda metadata: metadata.group(1) + re.sub(r"[^\r\n]", "", metadata.group(2)),
            token,
            count=1,
        )

    return _CPP_TOKENS.sub(replace_token, text)


def build(repo_root: Path, output: Path | None = None) -> int:
    """
    @fn build(repo_root: Path, output: Path | None = None) -> int
    @brief Run Doxide with adapted copies of the configured source files.
    @author Alex (<https://github.com/lextpf>)

    Source patterns must resolve inside the repository. Change only the staged output field
    so relative output paths still refer to the repository.
    Temporary input is removed after the process exits, including on failure.

    @param output Optional output override, relative to the repository or absolute.
    @return The Doxide process exit status.
    """
    repo_root = repo_root.resolve()
    config_path = repo_root / "doxide.yml"
    config = yaml.safe_load(config_path.read_text(encoding="utf-8"))
    output_path = (repo_root / (output or config.get("output", "docs"))).resolve()
    sources = {
        path.resolve()
        for pattern in config["files"]
        for path in repo_root.glob(pattern)
        if path.is_file()
    }
    relative_sources = [path.relative_to(repo_root) for path in sorted(sources)]
    temporary_root = repo_root / ".tmp"
    temporary_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="doxide-input-", dir=temporary_root) as directory:
        staged = Path(directory).resolve()
        if staged.parent != temporary_root.resolve():
            raise ValueError("Doxide input must stay inside the repository .tmp directory")
        config["output"] = str(output_path)
        (staged / "doxide.yml").write_text(
            yaml.safe_dump(config, sort_keys=False), encoding="utf-8"
        )
        for relative in relative_sources:
            destination = staged / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            text = (repo_root / relative).read_text(encoding="utf-8")
            destination.write_text(prepare_source(text), encoding="utf-8")
        return subprocess.run(
            ["doxide", "build"],
            cwd=staged,
            check=False,
        ).returncode


def main() -> int:
    """
    @fn main() -> int
    @brief Build repository API pages with an optional output-directory override.
    @author Alex (<https://github.com/lextpf>)

    Run from the repository root. The optional first argument overrides the configured output.
    """
    output = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    return build(Path.cwd(), output)


if __name__ == "__main__":
    sys.exit(main())
