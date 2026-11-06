#!/usr/bin/env python3
"""Read-only Jamma size, growth and static-candidate inventory (stdlib only)."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone


EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl"}
ROOTS = ("Jamma/src/", "Jamma/include/", "Jamma/Resource.h", "JammaLib/src/", "JammaLib/include/", "test/JammaLib_Tests/src/")
PRODUCTION_ROOTS = ROOTS[:-1]
STATIC_PATTERNS = {
    "new_conditional": re.compile(r"\b(?:else\s+)?if\s*\(|\bswitch\s*\("),
    "new_mode_or_flag": re.compile(r"\b(?:mode|flag|fallback|specialCase|bypass|override)\b", re.I),
    "new_callback_hazard": re.compile(r"\b(?:scoped_lock|lock_guard|unique_lock|make_shared|make_unique|new|throw|cout|cerr|printf|push_back|emplace_back)\b"),
}


def git(root, *args):
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True)
    if result.returncode:
        raise ValueError(result.stderr.decode("utf-8", "replace").strip() or "Git failed")
    return result.stdout


def verify_root(root):
    root = root.resolve()
    actual = Path(git(root, "rev-parse", "--show-toplevel").decode().strip()).resolve()
    if root != actual or not all((root / name).is_file() for name in ("Jamma.sln", "AGENTS.md", "doc/glossary.md")):
        raise ValueError("Pass the verified Jamma Git root containing Jamma.sln and guidance")
    return root


def first_party(path):
    return path.startswith(ROOTS) and Path(path).suffix.lower() in EXTENSIONS


def physical_lines(data):
    """Count lines consistently for LF, CRLF and a final unterminated line."""
    return data.count(b"\n") + (bool(data) and not data.endswith(b"\n"))


def current_files(root):
    paths = git(root, "ls-files", "-z").decode("utf-8", "surrogateescape").split("\0")
    files = {}
    for name in paths:
        if not first_party(name):
            continue
        path = root / name
        if not path.is_file() or path.is_symlink():
            continue
        data = path.read_bytes()
        if b"\0" in data:
            continue
        files[name] = {"lines": physical_lines(data), "sha256": hashlib.sha256(data).hexdigest(),
                       "production": name.startswith(PRODUCTION_ROOTS)}
    return files


def git_changes(root, previous_commit, head):
    if not previous_commit or previous_commit == head:
        return [], [], []
    previous_commit = git(root, "rev-parse", "--verify", "--end-of-options", previous_commit + "^{commit}").decode().strip()
    raw = git(root, "diff", "--numstat", "--find-renames", previous_commit, head, "--").decode("utf-8", "replace")
    changes = []
    for line in raw.splitlines():
        parts = line.split("\t", 2)
        if len(parts) != 3 or not first_party(parts[2]):
            continue
        added, removed, path = parts
        if added.isdigit() and removed.isdigit():
            changes.append({"path": path, "added": int(added), "removed": int(removed),
                            "net": int(added) - int(removed)})
    changes.sort(key=lambda row: (-row["added"], row["path"]))
    commits = git(root, "log", "--no-merges", "--format=%h %s", "-20", previous_commit + ".." + head).decode("utf-8", "replace").splitlines()
    diff = git(root, "diff", "--unified=0", previous_commit, head, "--", *ROOTS).decode("utf-8", "replace")
    candidates = []
    path = None
    line_no = 0
    for row in diff.splitlines():
        if row.startswith("+++ b/"):
            path = row[6:]
        elif row.startswith("@@"):
            match = re.search(r"\+(\d+)(?:,(\d+))?", row)
            if match:
                line_no = int(match.group(1))
        elif row.startswith("+") and not row.startswith("+++"):
            if path and first_party(path):
                for kind, pattern in STATIC_PATTERNS.items():
                    if pattern.search(row[1:]):
                        candidates.append({"kind": kind, "path": path, "line": line_no,
                                           "text": row[1:].strip()[:180]})
            line_no += 1
        elif row.startswith(" "):
            line_no += 1
    return changes, commits, candidates[:100]


def compare(files, previous):
    old_files = previous.get("files", {}) if previous else {}
    rows = []
    for path, data in files.items():
        earlier = old_files.get(path)
        earlier_lines = earlier["lines"] if isinstance(earlier, dict) else earlier
        rows.append({"path": path, "lines": data["lines"], "previous_lines": earlier_lines,
                     "delta": data["lines"] - earlier_lines if earlier_lines is not None else None,
                     "production": data["production"]})
    rows.sort(key=lambda row: (-row["lines"], row["path"]))
    growth = sorted((row for row in rows if row["delta"] is not None and row["delta"] > 0),
                    key=lambda row: (-row["delta"], row["path"]))
    return rows, growth


def report(root, previous=None):
    root = verify_root(root)
    head = git(root, "rev-parse", "HEAD").decode().strip()
    branch = git(root, "branch", "--show-current").decode().strip() or None
    status = git(root, "status", "--porcelain=v1", "--untracked-files=normal").decode("utf-8", "replace").splitlines()
    files = current_files(root)
    rows, growth = compare(files, previous)
    earlier = previous.get("commit") if previous else None
    changes, commits, candidates = git_changes(root, earlier, head)
    snapshot = {"schema": 1, "repo": str(root), "at_utc": datetime.now(timezone.utc).isoformat(),
                "branch": branch, "commit": head, "files": files}
    result = {"snapshot": snapshot, "dirty_status": status, "previous":
              {key: previous.get(key) for key in ("at_utc", "branch", "commit")} if previous else None,
              "cross_branch": bool(previous and previous.get("branch") != branch),
              "top_20": rows[:20], "largest_production": [row for row in rows if row["production"]][:20],
              "largest_growth": growth[:20], "git_changes": changes[:50], "recent_commits": commits,
              "static_candidates": candidates, "notes": [
                  "Physical lines include blanks and comments; size and static matches are inspection candidates.",
                  "Tracked first-party C/C++ files only; working-tree content is counted, Git diff covers committed changes only.",
                  "Cross-branch comparisons show numeric deltas but do not attribute them to the past week."
              ]}
    return result


def markdown(result):
    snap = result["snapshot"]
    previous = result["previous"]
    lines = [f"# Jamma structure inventory — {snap['at_utc']}", "",
             f"Checkout: `{snap['repo']}`; branch: `{snap['branch'] or '(detached)'}`; HEAD: `{snap['commit']}`.", ""]
    if previous:
        lines += [f"Previous snapshot: `{previous['branch']}` at `{previous['commit']}` ({previous['at_utc']}).", ""]
        if result["cross_branch"]:
            lines += ["Cross-branch comparison: inspect merge base before calling a delta weekly growth.", ""]
    else:
        lines += ["No prior snapshot: weekly line deltas are unavailable.", ""]
    lines += ["## Top 20 first-party code files", "", "| Rank | File | Lines | Previous | Delta |", "| ---: | --- | ---: | ---: | ---: |"]
    for index, row in enumerate(result["top_20"], 1):
        old = "—" if row["previous_lines"] is None else str(row["previous_lines"])
        delta = "—" if row["delta"] is None else f"{row['delta']:+d}"
        lines.append(f"| {index} | `{row['path']}` | {row['lines']} | {old} | {delta} |")
    lines += ["", "## Largest increases", ""]
    lines += [f"- `{row['path']}`: {row['delta']:+d} to {row['lines']} lines" for row in result["largest_growth"]] or ["- No comparable growth."]
    lines += ["", "## Committed change candidates", ""]
    lines += [f"- `{row['path']}`: +{row['added']}/-{row['removed']}" for row in result["git_changes"][:20]] or ["- No committed changes against the prior snapshot."]
    lines += ["", f"Static candidates: {len(result['static_candidates'])} shown in JSON output; inspect before reporting.",
              f"Dirty checkout entries: {len(result['dirty_status'])}.", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--previous", type=Path)
    parser.add_argument("--snapshot", type=Path, help="Save snapshot outside repository after successful scan")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    args = parser.parse_args()
    try:
        root = verify_root(Path(args.repo))
        if args.snapshot and (args.snapshot.resolve() == root or root in args.snapshot.resolve().parents):
            raise ValueError("Snapshot output must be outside Jamma")
        previous = json.loads(args.previous.read_text(encoding="utf-8-sig")) if args.previous else None
        if previous and (previous.get("schema") != 1 or Path(previous.get("repo", "")).resolve() != root):
            raise ValueError("Previous snapshot is not a matching Jamma audit snapshot")
        result = report(root, previous)
        if args.snapshot:
            args.snapshot.parent.mkdir(parents=True, exist_ok=True)
            args.snapshot.write_text(json.dumps(result["snapshot"], indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2) if args.format == "json" else markdown(result))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Audit failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
