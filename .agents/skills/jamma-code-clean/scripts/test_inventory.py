#!/usr/bin/env python3
"""Heuristic inventory of tracked Jamma native tests and presentation assertions."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


DECLARATION = re.compile(r"\b(TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(([^)]*)\)")
ASSERTION = re.compile(r"\b(?:ASSERT|EXPECT)_[A-Z_]+\s*\(|\b(?:CHECK|REQUIRE)\s*\(")
PRESENTATION = re.compile(r"\b(?:ASSERT|EXPECT)_[A-Z_]+\s*\([\s\S]*?\);")
GEOMETRY = re.compile(r"\b(?:Position|GlobalPosition|GetSize|SetSize|FontSize|Padding|BandBounds)\b")


def git(root, *args):
    proc = subprocess.run(["git", "-C", str(root), *args], capture_output=True)
    if proc.returncode:
        raise ValueError(proc.stderr.decode("utf-8", "replace").strip() or "Git failed")
    return proc.stdout


def inventory(root):
    root = root.resolve()
    actual = Path(git(root, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if actual != root or not (root / "Jamma.sln").is_file():
        raise ValueError("Pass the verified Jamma Git root")
    project = (root / "test/JammaLib_Tests/JammaLib_Tests.vcxproj").read_text(encoding="utf-8-sig")
    includes = {match.replace("\\", "/").lower() for match in
                re.findall(r'<ClCompile\s+Include="([^"]+\.cpp)"', project)}
    paths = git(root, "ls-files", "-z", "--", "test/JammaLib_Tests/src").decode("utf-8", "surrogateescape").split("\0")
    tests = []
    for name in sorted(paths):
        if not name.endswith(".cpp"):
            continue
        path = root / name
        if not path.is_file() or path.is_symlink():
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").split("\n")
        compiled = name.removeprefix("test/JammaLib_Tests/").lower() in includes
        for index, line in enumerate(lines):
            match = DECLARATION.search(line)
            if not match:
                continue
            start = next((item for item in range(index, min(len(lines), index + 20)) if "{" in lines[item]), None)
            if start is None:
                continue
            depth = 0
            end = start
            for item in range(start, len(lines)):
                depth += lines[item].count("{") - lines[item].count("}")
                end = item
                if depth <= 0:
                    break
            body = "\n".join(lines[start:end + 1])
            presentation = []
            for assertion in PRESENTATION.finditer(body):
                if GEOMETRY.search(assertion.group()):
                    presentation.append({"line": start + 1 + body.count("\n", 0, assertion.start()),
                                         "assertion": assertion.group(),
                                         "review": "Classify cosmetic mirroring versus functional clipping, reachability or hit testing"})
            normalized = re.sub(r"\s+", "", re.sub(r"//[^\n]*", "", body))
            tests.append({"path": name, "line": index + 1, "name": match.group(2), "kind": match.group(1),
                          "compiled": compiled, "direct_assertion_candidate_count": len(ASSERTION.findall(body)),
                          "presentation_assertion_candidates": presentation, "heuristic_span_end": end + 1,
                          "body_sha256": hashlib.sha256(normalized.encode()).hexdigest()})
    by_hash = {}
    for test in tests:
        if test["compiled"]:
            by_hash.setdefault(test["body_sha256"], []).append(f"{test['path']}:{test['line']}")
    return {"repo": str(root), "commit": git(root, "rev-parse", "HEAD").decode().strip(),
            "declarations": len(tests), "compiled_declarations": sum(test["compiled"] for test in tests),
            "tests": tests, "duplicate_compiled_body_candidates": [group for group in by_hash.values() if len(group) > 1],
            "notes": ["Lexical inventory only; comments/macros/helpers and exact brace parsing require manual inspection.",
                      "Compilation flag is based on direct ClCompile entries, not imported item groups or preprocessor guards."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(inventory(Path(args.repo)), indent=2))
    except (OSError, ValueError) as error:
        print(f"Inventory failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
