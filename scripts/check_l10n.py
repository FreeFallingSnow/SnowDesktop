#!/usr/bin/env python3
"""Validate SnowDesktop localization resources and source usage."""

import argparse
import collections
import json
import re
import sys
from pathlib import Path
from typing import Dict, Iterable, Iterator, List, Sequence, Tuple


SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".h", ".hpp", ".rc"}
CJK_PATTERN = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]")
TRANSLATION_CALL_PATTERN = re.compile(
    r"\b(?:_L(?:FW|F|W)?|L10N_KEY)\s*\(\s*\"([^\"\\]*(?:\\.[^\"\\]*)*)\""
)
LUA_TRANSLATION_CALL_PATTERN = re.compile(
    r"\bl10n\.tr\s*\(\s*\"([^\"\\]*(?:\\.[^\"\\]*)*)\""
)
BRACE_PLACEHOLDER_PATTERN = re.compile(r"\{\d+\}")
PRINTF_PLACEHOLDER_PATTERN = re.compile(
    r"%(?!%)(?:\d+\$)?[-+ #0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?"
    r"(?:hh|h|ll|l|j|z|t|L)?[diuoxXfFeEgGaAcspn]"
)
ALLOW_MARKER = "l10n-allow"

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")


class DuplicateKeyError(ValueError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Check language JSON files, translation-key references, placeholders, "
            "and hard-coded Chinese in C/C++ and Lua source string literals."
        )
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the parent of this script directory)",
    )
    parser.add_argument(
        "--strict-unused",
        action="store_true",
        help="treat translation keys unused by literal localization calls as errors",
    )
    return parser.parse_args()


def unique_object_pairs(pairs: Sequence[Tuple[str, object]]) -> Dict[str, object]:
    result: Dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError("duplicate key: {0}".format(key))
        result[key] = value
    return result


def load_language_file(path: Path) -> Dict[str, str]:
    try:
        with path.open("r", encoding="utf-8-sig") as stream:
            data = json.load(stream, object_pairs_hook=unique_object_pairs)
    except (OSError, UnicodeError, json.JSONDecodeError, DuplicateKeyError) as exc:
        raise ValueError("{0}: {1}".format(path, exc))

    if not isinstance(data, dict):
        raise ValueError("{0}: top-level JSON value must be an object".format(path))

    invalid = [
        key
        for key, value in data.items()
        if not isinstance(key, str) or not isinstance(value, str)
    ]
    if invalid:
        raise ValueError(
            "{0}: every translation key and value must be a string; invalid keys: {1}".format(
                path, ", ".join(str(key) for key in invalid[:5])
            )
        )
    return data  # type: ignore[return-value]


def source_files(source_root: Path) -> List[Path]:
    return sorted(
        path
        for path in source_root.rglob("*")
        if path.is_file() and path.suffix.lower() in SOURCE_EXTENSIONS
    )


def line_number_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def skip_quoted(text: str, start: int, quote: str) -> int:
    index = start + 1
    while index < len(text):
        if text[index] == "\\":
            index += 2
            continue
        if text[index] == quote:
            return index + 1
        index += 1
    return len(text)


def raw_string_start(text: str, quote_index: int) -> Tuple[int, str]:
    prefix_start = quote_index
    while prefix_start > 0 and (
        text[prefix_start - 1].isalnum() or text[prefix_start - 1] == "_"
    ):
        prefix_start -= 1
    prefix = text[prefix_start:quote_index]
    if prefix not in {"R", "u8R", "uR", "UR", "LR"}:
        return -1, ""

    delimiter_end = text.find("(", quote_index + 1)
    if delimiter_end < 0:
        return -1, ""
    delimiter = text[quote_index + 1:delimiter_end]
    if len(delimiter) > 16 or any(ch.isspace() or ch in "()\\" for ch in delimiter):
        return -1, ""
    return delimiter_end + 1, ")" + delimiter + "\""


def iter_cpp_string_literals(text: str) -> Iterator[Tuple[int, str]]:
    """Yield (line, source content) for string literals while ignoring comments."""

    index = 0
    length = len(text)
    while index < length:
        if text.startswith("//", index):
            newline = text.find("\n", index + 2)
            index = length if newline < 0 else newline + 1
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            index = length if end < 0 else end + 2
            continue
        if text[index] == "'":
            index = skip_quoted(text, index, "'")
            continue
        if text[index] != '"':
            index += 1
            continue

        line = line_number_at(text, index)
        raw_content_start, raw_end_marker = raw_string_start(text, index)
        if raw_content_start >= 0:
            raw_end = text.find(raw_end_marker, raw_content_start)
            if raw_end < 0:
                yield line, text[raw_content_start:]
                return
            yield line, text[raw_content_start:raw_end]
            index = raw_end + len(raw_end_marker)
            continue

        content_start = index + 1
        index = content_start
        while index < length:
            if text[index] == "\\":
                index += 2
                continue
            if text[index] == '"':
                break
            index += 1
        yield line, text[content_start:index]
        index = min(index + 1, length)


def lua_long_bracket(text: str, start: int) -> Tuple[int, str]:
    if start >= len(text) or text[start] != "[":
        return -1, ""
    index = start + 1
    while index < len(text) and text[index] == "=":
        index += 1
    if index >= len(text) or text[index] != "[":
        return -1, ""
    return index + 1, "]" + text[start + 1:index] + "]"


def iter_lua_string_literals(text: str) -> Iterator[Tuple[int, str]]:
    """Yield Lua strings while ignoring line and long-bracket comments."""

    index = 0
    length = len(text)
    while index < length:
        if text.startswith("--", index):
            content_start, end_marker = lua_long_bracket(text, index + 2)
            if content_start >= 0:
                end = text.find(end_marker, content_start)
                index = length if end < 0 else end + len(end_marker)
            else:
                newline = text.find("\n", index + 2)
                index = length if newline < 0 else newline + 1
            continue

        if text[index] in {"'", '"'}:
            quote = text[index]
            line = line_number_at(text, index)
            content_start = index + 1
            index = content_start
            while index < length:
                if text[index] == "\\":
                    index += 2
                    continue
                if text[index] == quote:
                    break
                index += 1
            yield line, text[content_start:index]
            index = min(index + 1, length)
            continue

        content_start, end_marker = lua_long_bracket(text, index)
        if content_start >= 0:
            line = line_number_at(text, index)
            end = text.find(end_marker, content_start)
            if end < 0:
                yield line, text[content_start:]
                return
            yield line, text[content_start:end]
            index = end + len(end_marker)
            continue

        index += 1


def find_translation_references(
    files: Iterable[Path], root: Path
) -> Tuple[Dict[str, List[str]], List[str]]:
    references: Dict[str, List[str]] = collections.defaultdict(list)
    read_errors: List[str] = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8-sig")
        except (OSError, UnicodeError) as exc:
            read_errors.append("{0}: {1}".format(path.relative_to(root), exc))
            continue
        pattern = (
            LUA_TRANSLATION_CALL_PATTERN
            if path.suffix.lower() == ".lua"
            else TRANSLATION_CALL_PATTERN
        )
        for match in pattern.finditer(text):
            key = bytes(match.group(1), "utf-8").decode("unicode_escape")
            location = "{0}:{1}".format(
                path.relative_to(root), line_number_at(text, match.start())
            )
            references[key].append(location)
    return references, read_errors


def validate_widget_manifests(
    files: Iterable[Path],
    lua_references: Dict[Path, Dict[str, List[str]]],
    required_languages: Sequence[str],
    root: Path,
    strict_unused: bool,
) -> Tuple[List[str], List[str], int]:
    errors: List[str] = []
    warnings: List[str] = []
    reference_count = 0
    paired_lua_files = set()
    for path in files:
        relative = path.relative_to(root)
        try:
            with path.open("r", encoding="utf-8-sig") as stream:
                data = json.load(stream, object_pairs_hook=unique_object_pairs)
        except (OSError, UnicodeError, json.JSONDecodeError, DuplicateKeyError) as exc:
            errors.append("{0}: {1}".format(relative, exc))
            continue
        if not isinstance(data, dict):
            errors.append("{0}: top-level JSON value must be an object".format(relative))
            continue

        references: Dict[str, List[str]] = collections.defaultdict(list)
        if path.name == "widget.json":
            entry = data.get("entry", "main.lua")
            if not isinstance(entry, str) or not entry:
                errors.append("{0}: entry must be a non-empty string".format(relative))
                package_lua_files = []
            else:
                entry_path = path.parent / Path(entry)
                if not entry_path.is_file():
                    errors.append(
                        "{0}: package entry does not exist: {1}".format(
                            relative, entry
                        )
                    )
                package_lua_files = sorted(path.parent.rglob("*.lua"))
        else:
            stem = path.name[:-len(".widget.json")]
            package_lua_files = [path.with_name(stem + ".lua")]
        for lua_path in package_lua_files:
            paired_lua_files.add(lua_path)
            for key, locations in lua_references.get(lua_path, {}).items():
                references[key].extend(locations)
        for field in ("nameKey", "descriptionKey"):
            key = data.get(field)
            if key is None:
                errors.append("{0}: missing localization field {1}".format(relative, field))
            elif not isinstance(key, str) or not key:
                errors.append("{0}: {1} must be a non-empty string".format(relative, field))
            else:
                references[key].append("{0}:{1}".format(relative, field))
        title_keys = data.get("titleKeys", [])
        if not isinstance(title_keys, list) or any(
            not isinstance(key, str) or not key for key in title_keys
        ):
            errors.append("{0}: titleKeys must be an array of non-empty strings".format(
                relative
            ))
        else:
            for key in title_keys:
                references[key].append("{0}:titleKeys".format(relative))

        for field in ("name", "description"):
            fallback = data.get(field)
            if isinstance(fallback, str) and CJK_PATTERN.search(fallback):
                errors.append(
                    "{0}: hard-coded Chinese in manifest {1}: {2}".format(
                        relative, field, fallback
                    )
                )

        locales = data.get("locales")
        if not isinstance(locales, dict) or not locales:
            errors.append("{0}: locales must be a non-empty object".format(relative))
            continue

        catalogs: Dict[str, Dict[str, str]] = {}
        for language, catalog in locales.items():
            if not isinstance(language, str) or not isinstance(catalog, dict):
                errors.append(
                    "{0}: every locales entry must map a language to an object".format(
                        relative
                    )
                )
                continue
            invalid = [
                key
                for key, value in catalog.items()
                if not isinstance(key, str) or not isinstance(value, str)
            ]
            if invalid:
                errors.append(
                    "{0}: locale {1} contains non-string keys or values".format(
                        relative, language
                    )
                )
                continue
            catalogs[language] = catalog
            for key, value in catalog.items():
                if not value:
                    errors.append(
                        "{0}: empty {1} translation for {2}".format(
                            relative, language, key
                        )
                    )
                if language.lower().startswith("en") and CJK_PATTERN.search(value):
                    errors.append(
                        "{0}: Chinese text remains in {1} translation {2}: {3}".format(
                            relative, language, key, value.replace("\n", "\\n")
                        )
                    )

        for language in required_languages:
            if language not in catalogs:
                errors.append(
                    "{0}: missing widget locale {1}".format(relative, language)
                )
        if not catalogs:
            continue

        baseline_name = "zh-CN" if "zh-CN" in catalogs else sorted(catalogs)[0]
        baseline = catalogs[baseline_name]
        baseline_keys = set(baseline)
        for language, catalog in sorted(catalogs.items()):
            current_keys = set(catalog)
            for key in sorted(baseline_keys - current_keys):
                errors.append(
                    "{0}: locale {1} missing key present in {2}: {3}".format(
                        relative, language, baseline_name, key
                    )
                )
            for key in sorted(current_keys - baseline_keys):
                errors.append(
                    "{0}: locale {1} has extra key absent from {2}: {3}".format(
                        relative, language, baseline_name, key
                    )
                )

        for key, locations in sorted(references.items()):
            for language, catalog in sorted(catalogs.items()):
                if key not in catalog:
                    errors.append(
                        "{0}: locale {1} missing referenced key {2} (used at {3})".format(
                            relative, language, key, ", ".join(locations[:3])
                        )
                    )

        common_keys = set.intersection(*(set(item) for item in catalogs.values()))
        for key in sorted(common_keys):
            expected = placeholders(baseline[key])
            for language, catalog in sorted(catalogs.items()):
                actual = placeholders(catalog[key])
                if actual != expected:
                    errors.append(
                        "{0}: placeholder mismatch for {1} in {2}: {3!r} vs {4} {5!r}".format(
                            relative, key, language, actual, baseline_name, expected
                        )
                    )

        unused = sorted(baseline_keys - set(references))
        if unused:
            message = "{0}: {1} widget locale keys are unused".format(
                relative, len(unused)
            )
            if strict_unused:
                errors.append(message + ": " + ", ".join(unused))
            else:
                warnings.append(message)
        reference_count += len(references)

    for lua_path in sorted(set(lua_references) - paired_lua_files):
        errors.append(
            "{0}: missing containing widget.json or matching .widget.json manifest".format(
                lua_path.relative_to(root)
            )
        )
    return errors, warnings, reference_count


def find_hardcoded_chinese(files: Iterable[Path], root: Path) -> List[str]:
    findings: List[str] = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8-sig")
        except (OSError, UnicodeError):
            continue
        lines = text.splitlines()
        iterator = (
            iter_lua_string_literals(text)
            if path.suffix.lower() == ".lua"
            else iter_cpp_string_literals(text)
        )
        for line_number, content in iterator:
            if not CJK_PATTERN.search(content):
                continue
            source_line = lines[line_number - 1] if line_number <= len(lines) else ""
            if ALLOW_MARKER in source_line.lower():
                continue
            compact = content.replace("\r", "\\r").replace("\n", "\\n")
            if len(compact) > 100:
                compact = compact[:97] + "..."
            findings.append(
                "{0}:{1}: {2}".format(path.relative_to(root), line_number, compact)
            )
    return findings


def placeholders(value: str) -> Tuple[collections.Counter, collections.Counter]:
    braces = collections.Counter(BRACE_PLACEHOLDER_PATTERN.findall(value))
    printf = collections.Counter(PRINTF_PLACEHOLDER_PATTERN.findall(value))
    return braces, printf


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    language_dir = root / "lang"
    source_root = root / "src"
    widget_root = root / "widgets"

    errors: List[str] = []
    warnings: List[str] = []

    language_paths = sorted(language_dir.glob("*.json"))
    if not language_paths:
        errors.append("no language files found under {0}".format(language_dir))

    languages: Dict[str, Dict[str, str]] = {}
    for path in language_paths:
        try:
            languages[path.name] = load_language_file(path)
        except ValueError as exc:
            errors.append(str(exc))

    for name, translations in sorted(languages.items()):
        for key, value in sorted(translations.items()):
            if not value:
                errors.append("{0}: empty translation for {1}".format(name, key))
            if name.lower().startswith("en") and CJK_PATTERN.search(value):
                errors.append(
                    "{0}: Chinese text remains in translation {1}: {2}".format(
                        name, key, value.replace("\n", "\\n")
                    )
                )

    cpp_files = source_files(source_root) if source_root.is_dir() else []
    lua_files = sorted(widget_root.rglob("*.lua")) if widget_root.is_dir() else []
    manifest_files = (
        sorted(
            set(widget_root.rglob("*.widget.json"))
            | set(widget_root.rglob("widget.json"))
        )
        if widget_root.is_dir()
        else []
    )
    files = cpp_files + lua_files
    if not files:
        errors.append("no source files found under {0} or {1}".format(
            source_root, widget_root
        ))

    references, read_errors = find_translation_references(cpp_files, root)
    errors.extend(read_errors)

    lua_references: Dict[Path, Dict[str, List[str]]] = {}
    for path in lua_files:
        per_file_references, per_file_errors = find_translation_references(
            [path], root
        )
        lua_references[path] = per_file_references
        errors.extend(per_file_errors)

    manifest_errors, manifest_warnings, widget_reference_count = (
        validate_widget_manifests(
            manifest_files,
            lua_references,
            [path.stem for path in language_paths],
            root,
            args.strict_unused,
        )
    )
    errors.extend(manifest_errors)
    warnings.extend(manifest_warnings)

    if languages:
        baseline_name = "zh-CN.json" if "zh-CN.json" in languages else sorted(languages)[0]
        baseline_keys = set(languages[baseline_name])
        for name, translations in sorted(languages.items()):
            current_keys = set(translations)
            for key in sorted(baseline_keys - current_keys):
                errors.append("{0}: missing key present in {1}: {2}".format(
                    name, baseline_name, key
                ))
            for key in sorted(current_keys - baseline_keys):
                errors.append("{0}: extra key absent from {1}: {2}".format(
                    name, baseline_name, key
                ))

        for key, locations in sorted(references.items()):
            for name, translations in sorted(languages.items()):
                if key not in translations:
                    errors.append(
                        "{0}: missing referenced key {1} (used at {2})".format(
                            name, key, ", ".join(locations[:3])
                        )
                    )

        common_keys = set.intersection(*(set(item) for item in languages.values()))
        baseline = languages[baseline_name]
        for key in sorted(common_keys):
            expected = placeholders(baseline[key])
            for name, translations in sorted(languages.items()):
                actual = placeholders(translations[key])
                if actual != expected:
                    errors.append(
                        "{0}: placeholder mismatch for {1}: {2!r} vs {3} {4!r}".format(
                            name, key, actual, baseline_name, expected
                        )
                    )

        unused = sorted(baseline_keys - set(references))
        if unused:
            message = "{0} global translation keys are not referenced by literal C/C++ localization calls".format(
                len(unused)
            )
            if args.strict_unused:
                errors.append(message + ": " + ", ".join(unused))
            else:
                warnings.append(message + " (use --strict-unused to list as an error)")

    hardcoded = find_hardcoded_chinese(files, root)
    for finding in hardcoded:
        errors.append(
            "hard-coded Chinese string: {0} (translate it or add a line {1} marker with a reason)".format(
                finding, ALLOW_MARKER
            )
        )

    for warning in warnings:
        print("WARNING: " + warning)
    for error in errors:
        print("ERROR: " + error)

    print(
        "Checked {0} source files, {1} referenced keys, {2} language files.".format(
            len(files), len(references) + widget_reference_count, len(languages)
        )
    )
    if errors:
        print("Localization check failed with {0} error(s).".format(len(errors)))
        return 1
    print("Localization check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
