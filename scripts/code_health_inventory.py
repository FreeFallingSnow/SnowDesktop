"""Summarize native source dependencies and release history. Run at repository root."""

import argparse
import csv
import itertools
import json
import posixpath
import re
import subprocess
from collections import Counter, defaultdict
from pathlib import Path


ROOTS = ("src", "tools/snowwidget", "steam_bridge/src")
GENERATED = "src/ui/render/acrylic_noise_asset.h"
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc"}
MAX_PAIR_FILES = 20


def git(*args, input_text=None):
    return subprocess.check_output(
        ["git", "-c", "core.quotePath=false", *args], encoding="utf-8", input=input_text
    )


def native(path):
    return path != GENERATED and posixpath.splitext(path)[1] in SUFFIXES


def physical_line_counts(revision):
    counts = {}
    # Relative to an empty tree, numstat counts every physical line as an addition.
    empty_tree = git("hash-object", "-t", "tree", "--stdin", input_text="").strip()
    entries = git("diff", "--numstat", "-z", "--text", "--no-renames", "--no-ext-diff",
                  "--no-textconv", empty_tree, revision, "--", *ROOTS, ":(exclude)" + GENERATED)
    for entry in entries.rstrip("\0").split("\0"):
        if entry:
            added, _, path = entry.split("\t", 2)
            counts[path] = int(added)
    return counts


def include_edges(revision, files):
    includes = defaultdict(set)
    users = defaultdict(set)
    lines = git("grep", "-I", "-E", r'^[[:space:]]*#[[:space:]]*include[[:space:]]*"',
                revision, "--", *ROOTS)
    for line in lines.splitlines():
        _, path, directive = line.split(":", 2)
        if path not in files:
            continue
        name = re.search(r'#\s*include\s*"([^"]+)"', directive)
        if not name:
            continue
        source_root = "steam_bridge/src" if path.startswith("steam_bridge/") else "src"
        for directory in (posixpath.dirname(path), source_root):
            target = posixpath.normpath(posixpath.join(directory, name[1]))
            if target in files:
                includes[path].add(target)
                users[target].add(path)
                break
    return includes, users


def reverse_reach(path, users):
    visited = {path}
    pending = [path]
    while pending:
        for user in users[pending.pop()] - visited:
            visited.add(user)
            pending.append(user)
    return len(visited) - 1


def changes(history, files):
    touches, small, pairs = Counter(), Counter(), Counter()
    commits = broad = 0
    for block in history.split("@")[1:]:
        paths = {p for p in block.splitlines()[1:] if native(p)}
        if not paths:
            continue
        commits += 1
        touches.update(paths)
        if len(paths) > MAX_PAIR_FILES:
            broad += 1
            continue
        current = sorted(paths & files)
        small.update(current)
        pairs.update(itertools.combinations(current, 2))
    return touches, small, pairs, commits, broad


def write_csv(path, header, rows):
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new evidence directory")
    parser.add_argument("--replay", type=Path, help="reuse revisions from a prior inventory.json")
    args = parser.parse_args()
    if args.replay:
        frozen = json.loads(args.replay.read_text(encoding="utf-8"))
        revision, mainline, tips = (frozen[k] for k in ("source", "main", "releaseTips"))
    else:
        revision = git("rev-parse", "HEAD").strip()
        mainline = git("rev-parse", "main").strip()
        refs = git("for-each-ref", "--format=%(refname:short) %(objectname)", "refs/heads/release")
        tips = dict(line.split() for line in refs.splitlines())
        if not tips:
            parser.error("local release branches are required for development history")
    args.output.mkdir(parents=True, exist_ok=False)
    files = {p for p in git("ls-tree", "-r", "--name-only", revision, "--", *ROOTS).splitlines()
             if native(p)}
    includes, users = include_edges(revision, files)
    line_counts = physical_line_counts(revision)
    history = git("log", "--no-merges", "--no-renames", "--format=@%H", "--name-only",
                  *sorted(set(tips.values())), "--not", mainline, "--", *ROOTS)
    touches, small, pairs, commits, broad = changes(history, files)
    write_csv(args.output / "files.csv", ["path", "directIn", "reachableIn", "directOut",
              "touches", "smallCommitTouches", "physicalLines"],
              ((p, len(users[p]), reverse_reach(p, users), len(includes[p]), touches[p], small[p],
                line_counts.get(p, 0))
               for p in sorted(files)))
    write_csv(args.output / "includes.csv", ["consumer", "includes"],
              ((p, dep) for p in sorted(files) for dep in sorted(includes[p])))
    write_csv(args.output / "cochange.csv", ["first", "second", "together", "touches1", "touches2"],
              ((a, b, n, small[a], small[b]) for (a, b), n in
               sorted(pairs.items(), key=lambda item: (-item[1], item[0]))))
    metadata = dict(source=revision, main=mainline, releaseTips=tips, roots=ROOTS,
                    maxPairFiles=MAX_PAIR_FILES, sourceFiles=len(files),
                    nativeCommits=commits, broadCommits=broad, git=git("--version").strip())
    (args.output / "inventory.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{len(files)} source files, {commits} development commits, {broad} broad commits")


if __name__ == "__main__":
    main()
