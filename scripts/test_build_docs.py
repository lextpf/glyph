"""
@brief Check Doxide source adaptation and build isolation.
@author Alex (<https://github.com/lextpf>)
"""

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import yaml

import _build_docs


class BuildDocsTest(unittest.TestCase):
    """
    @class BuildDocsTest
    @brief Verify metadata filtering without changing source or build configuration.
    @author Alex (<https://github.com/lextpf>)
    """

    def test_removes_wrapped_metadata_and_preserves_line_numbers(self):
        """
        @fn test_removes_wrapped_metadata_and_preserves_line_numbers(self)
        @brief Keep the brief and source positions after removing a wrapped signature.
        @author Alex (<https://github.com/lextpf>)
        """
        source = (
            "/**\n"
            " * @fn const std::vector<Item>& Read(\n"
            " *     const State& state)\n"
            " * @brief Borrow the current items.\n"
            " */\n"
            "const std::vector<Item>& Read(const State& state);\n"
        )
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=newline):
                original = source.replace("\n", newline)
                result = _build_docs.prepare_source(original)
                self.assertNotIn("@fn", result)
                self.assertIn("@brief Borrow the current items.", result)
                self.assertTrue(
                    result.endswith("const std::vector<Item>& Read(const State& state);" + newline)
                )
                self.assertEqual(result.count("\n"), original.count("\n"))

    def test_preserves_literals_ordinary_comments_and_examples(self):
        """
        @fn test_preserves_literals_ordinary_comments_and_examples(self)
        @brief Keep annotation-like text outside leading callable metadata.
        @author Alex (<https://github.com/lextpf>)
        """
        source = (
            'const char* text = "/**\\n * @fn void Fake()\\n * @brief Fake.\\n */";\n'
            'const char* raw = R"sample(/**\n'
            ' * @fn void Fake()\n * @brief Fake.\n */)sample";\n'
            "// @fn void Fake()\n"
            "/*\n * @fn void Fake()\n * @brief Fake.\n */\n"
            "/**\n * @brief Metadata example.\n"
            " * @code\n * @fn void Example()\n * @brief Example.\n * @endcode\n */\n"
            "/**\n * @fn void MissingBrief()\n */\n"
        )
        self.assertEqual(_build_docs.prepare_source(source), source)

    def test_preserves_digit_separators_before_multiline_documentation(self):
        """
        @fn test_preserves_digit_separators_before_multiline_documentation(self)
        @brief Parse numeric separators without consuming later documentation as character text.
        @author Alex (<https://github.com/lextpf>)
        """
        for number in ("1'000", "0xFE'FF", "0b10'01", "1'000.5'0", "1e1'0"):
            with self.subTest(number=number):
                declaration = f"constexpr auto value = {number};\n"
                source = (
                    declaration
                    + "/**\n * @fn void Read()\n"
                    + " * @brief Read the actor's values.\n */\nvoid Read();\n"
                    + "const char apostrophe = '\\'';\n"
                )
                result = _build_docs.prepare_source(source)
                self.assertTrue(result.startswith(declaration))
                self.assertNotIn("@fn", result)
                self.assertIn("@brief Read the actor's values.", result)
                self.assertEqual(result.count("\n"), source.count("\n"))

    def test_respects_configuration_and_propagates_exit_without_source_mutation(self):
        """
        @fn test_respects_configuration_and_propagates_exit_without_source_mutation(self)
        @brief Stage only selected sources and preserve configuration and process status.
        @author Alex (<https://github.com/lextpf>)
        """
        temporary_root = Path(__file__).resolve().parents[1] / ".tmp"
        temporary_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as directory:
            root = Path(directory).resolve()
            self.assertEqual(root.parent, temporary_root)
            (root / "src").mkdir()
            config = (
                "title: Fixture\noutput: custom-docs\n"
                "files:\n  - src/selected.hpp\n"
                "defines:\n  FLAG: 1\n"
            )
            (root / "doxide.yml").write_text(config, encoding="utf-8")
            original = b"/**\r\n * @fn void Run()\r\n * @brief Run.\r\n */\r\nvoid Run();\r\n"
            selected = root / "src/selected.hpp"
            selected.write_bytes(original)
            (root / "src/omitted.hpp").write_text("void Omitted();", encoding="utf-8")
            staged_paths = []

            def run(command, cwd, check):
                """
                @fn run(command, cwd, check)
                @brief Inspect the isolated parser input before its cleanup.
                @author Alex (<https://github.com/lextpf>)
                """
                staged = Path(cwd)
                staged_paths.append(staged)
                self.assertEqual(command, ["doxide", "build"])
                self.assertFalse(check)
                staged_config = yaml.safe_load(
                    (staged / "doxide.yml").read_text(encoding="utf-8")
                )
                expected_config = yaml.safe_load(config)
                expected_config["output"] = str(root / "custom-docs")
                self.assertEqual(staged_config, expected_config)
                self.assertNotIn("@fn", (staged / "src/selected.hpp").read_text(encoding="utf-8"))
                self.assertFalse((staged / "src/omitted.hpp").exists())
                return SimpleNamespace(returncode=7)

            with patch.object(_build_docs.subprocess, "run", side_effect=run):
                self.assertEqual(_build_docs.build(root), 7)
            self.assertEqual(selected.read_bytes(), original)
            self.assertEqual((root / "doxide.yml").read_text(encoding="utf-8"), config)
            self.assertFalse(staged_paths[0].exists())

    def test_applies_output_override_and_cleans_up_on_launch_failure(self):
        """
        @fn test_applies_output_override_and_cleans_up_on_launch_failure(self)
        @brief Keep output overrides absolute and remove staged input after a launch error.
        @author Alex (<https://github.com/lextpf>)
        """
        temporary_root = Path(__file__).resolve().parents[1] / ".tmp"
        temporary_root.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=temporary_root) as directory:
            root = Path(directory).resolve()
            self.assertEqual(root.parent, temporary_root)
            (root / "doxide.yml").write_text("files: []\noutput: docs\n", encoding="utf-8")
            staged_paths = []

            def fail(command, cwd, check):
                """
                @fn fail(command, cwd, check)
                @brief Check the staged output before simulating a launch failure.
                @author Alex (<https://github.com/lextpf>)
                """
                staged_paths.append(Path(cwd))
                config = yaml.safe_load((Path(cwd) / "doxide.yml").read_text(encoding="utf-8"))
                self.assertEqual(config["output"], str(root / "preview"))
                raise OSError("Unavailable")

            with patch.object(_build_docs.subprocess, "run", side_effect=fail):
                with self.assertRaises(OSError):
                    _build_docs.build(root, Path("preview"))
            self.assertFalse(staged_paths[0].exists())


if __name__ == "__main__":
    unittest.main()
