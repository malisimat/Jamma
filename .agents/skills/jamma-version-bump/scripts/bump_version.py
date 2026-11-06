#!/usr/bin/env python3
"""Preview or apply one guarded Jamma patch-version increment."""

import argparse
import json
import re
import subprocess
from pathlib import Path


def fail(message):
    raise SystemExit(message)


def one(pattern, data, label):
    matches = list(re.finditer(pattern, data, re.MULTILINE))
    if len(matches) != 1:
        fail(f"Expected exactly one {label}; found {len(matches)}")
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--expected", required=True, help="Version before this surge's bump, such as 5.0.2")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()

    repo = args.repo.resolve(strict=True)
    installed_script = repo / ".agents/skills/jamma-version-bump/scripts/bump_version.py"
    if Path(__file__).resolve() != installed_script.resolve():
        fail("Run the version script from the selected Jamma worktree, not another checkout")
    root = subprocess.run(
        ["git", "-C", str(repo), "rev-parse", "--show-toplevel"],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    if Path(root).resolve() != repo or not (repo / "Jamma.sln").is_file():
        fail("--repo must be the active Jamma Git worktree root")
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", args.expected):
        fail("--expected must be a three-part numeric version")
    major, minor, patch = map(int, args.expected.split("."))
    next_version = f"{major}.{minor}.{patch + 1}"

    constants = repo / "JammaLib/include/Constants.h"
    readme = repo / "README.md"
    source = constants.read_bytes()
    notes = readme.read_bytes()
    numbers = {}
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = one(rb"^#define[ \t]+" + name.encode() + rb"_VERSION[ \t]+([0-9]+)[ \t]*\r?$", source, name)
        numbers[name] = int(match.group(1))
    source_version = f"{numbers['MAJOR']}.{numbers['MINOR']}.{numbers['PATCH']}"
    readme_match = one(rb"^Current Version:[ \t]*v([0-9]+\.[0-9]+\.[0-9]+)[ \t]*\r?$", notes, "README version")
    readme_version = readme_match.group(1).decode("ascii")
    if source_version != readme_version:
        fail(f"Version mismatch: Constants.h={source_version}, README.md={readme_version}")
    if source_version == next_version:
        print(json.dumps({"status": "already_applied", "version": source_version}))
        return
    if source_version != args.expected:
        fail(f"Expected {args.expected}, found {source_version}; stop and reconcile")

    patch_match = one(rb"^#define[ \t]+PATCH_VERSION[ \t]+([0-9]+)[ \t]*\r?$", source, "PATCH")
    updated_source = source[:patch_match.start(1)] + str(patch + 1).encode() + source[patch_match.end(1):]
    updated_notes = notes[:readme_match.start(1)] + next_version.encode() + notes[readme_match.end(1):]
    status = "preview"
    if args.apply:
        # The preflight checks above run before either write. Preserve all other bytes.
        constants.write_bytes(updated_source)
        readme.write_bytes(updated_notes)
        status = "applied"
    print(json.dumps({"status": status, "old": args.expected, "new": next_version,
                      "files": [str(constants), str(readme)]}))


if __name__ == "__main__":
    main()
