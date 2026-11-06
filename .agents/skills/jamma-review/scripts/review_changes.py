#!/usr/bin/env python3
"""Read-only, explicit-scope Jamma review evidence. Python 3 standard library only."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

CPP = {'.cpp', '.h', '.hpp', '.cxx', '.cc', '.hh', '.inl'}
WATCH = {'Scene', 'Station', 'GuiHud'}
PATTERNS = {
    'lock_or_wait': r'\b(?:mutex|shared_mutex|recursive_mutex|scoped_lock|lock_guard|unique_lock|condition_variable|EnterCriticalSection|WaitFor\w+|Sleep\w*|AcquireSRWLock\w*)\b',
    'heap_or_container_growth': r'\b(?:new|delete|malloc|calloc|realloc|free|make_shared|make_unique|push_back|emplace_back|insert|resize|reserve)\b',
    'io_logging_or_exception': r'\b(?:throw|printf|fprintf|cout|cerr|clog|ofstream|ifstream|OutputDebugString\w*|CreateFile\w*|ReadFile|WriteFile)\b',
    'shared_state': r'\b(?:atomic|atomic_\w+|memory_order\w*|volatile|Interlocked\w*|thread|jthread|SRWLOCK|PublishedAudioSnapshot)\b',
    'ownership': r'\b(?:shared_ptr|unique_ptr|weak_ptr|load|store|exchange|collect|borrow)\b',
}
TEST = re.compile(r'^\s*(?:TEST|TEST_F|TEST_P|TYPED_TEST|TYPED_TEST_P)\s*\(')


def git(root, *args, allowed=(0,)):
    result = subprocess.run(['git', '-C', str(root), *args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if result.returncode not in allowed:
        raise ValueError(result.stderr.decode('utf-8', 'replace').strip()
                         or 'Git command failed: ' + ' '.join(args))
    return result


def commit(root, ref):
    # --end-of-options prevents a ref from being interpreted as a Git option.
    return git(root, 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}').stdout.decode().strip()


def blob(root, revision, path):
    return git(root, 'show', revision + ':' + path).stdout


def code_mask(text):
    """Mask C++ comments/literals, preserving newlines and character offsets.

    This is lexical filtering only, not a parser or preprocessor. Inactive #if
    branches and macros still require inspection.
    """
    token = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|(?:u8|u|U|L)?R"([^\s()\\]{0,16})\([\s\S]*?\)\1"|"(?:\\[\s\S]|[^"\\])*"|\'(?:\\[\s\S]|[^\'\\])*\'')
    return token.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), text)


def changes(old, new):
    added, removed = [], []
    matcher = difflib.SequenceMatcher(a=old, b=new, autojunk=False)
    for tag, a, b, c, d in matcher.get_opcodes():
        if tag in ('replace', 'delete'):
            removed.extend(range(a, b))
        if tag in ('replace', 'insert'):
            added.extend(range(c, d))
    return added, removed


def inspect_file(path, previous_path, status, old_bytes, new_bytes):
    if b'\0' in old_bytes or b'\0' in new_bytes:
        return {'path': path, 'previous_path': previous_path, 'status': status, 'binary': True}, [], []
    old = old_bytes.decode('utf-8', 'replace').splitlines()
    text = new_bytes.decode('utf-8', 'replace')
    new = text.splitlines()
    added, removed = changes(old, new)
    before, after = len(old), len(new)
    row = {'path': path, 'previous_path': previous_path, 'status': status, 'binary': False,
           'base_lines': before, 'candidate_lines': after, 'net_lines': after - before,
           'added_lines': len(added), 'removed_lines': len(removed),
           'growth_percent': round(100 * (after - before) / before, 1) if before else None,
           'watched_class': Path(path).stem in WATCH}
    if Path(path).suffix.lower() not in CPP:
        return row, [], []
    masked_text = code_mask(text)
    masked = masked_text.splitlines()
    candidates, violations = [], []
    for index in added:
        for kind, pattern in PATTERNS.items():
            if re.search(pattern, masked[index]):
                candidates.append({'kind': kind, 'path': path, 'line': index + 1,
                                   'text': new[index].strip()})
    added_set = set(added)
    for match in re.finditer(r'\bnamespace\s*\{', masked_text):
        first = masked_text.count('\n', 0, match.start())
        last = masked_text.count('\n', 0, match.end())
        if added_set.intersection(range(first, last + 1)):
            violations.append({'kind': 'anonymous_namespace', 'path': path, 'line': first + 1,
                               'rule': 'AGENTS.md prohibits anonymous namespaces.'})
    old_masked = code_mask(old_bytes.decode('utf-8', 'replace')).splitlines()
    row['added_test_declarations'] = sum(bool(TEST.search(masked[i])) for i in added)
    row['removed_test_declarations'] = sum(bool(TEST.search(old_masked[i])) for i in removed)
    return row, candidates, violations


def review(root, base_ref, head_ref=None, working_tree=False):
    root = Path(git(root, 'rev-parse', '--show-toplevel').stdout.decode('utf-8').strip())
    if not (root / 'Jamma.sln').is_file():
        raise ValueError('Selected Git root does not contain Jamma.sln.')
    base = commit(root, base_ref)
    checkout_head = commit(root, 'HEAD')
    head = checkout_head if working_tree else commit(root, head_ref or 'HEAD')
    state_before = git(root, 'status', '--porcelain=v1', '-z', '--untracked-files=all').stdout
    target = [] if working_tree else [head]
    raw = git(root, 'diff', '--name-status', '-z', '--find-renames', base, *target, '--').stdout
    parts = raw.decode('utf-8', 'surrogateescape').split('\0')
    selected, index = [], 0
    while index < len(parts) and parts[index]:
        status, previous = parts[index], parts[index + 1]
        index += 2
        if status[0] in ('R', 'C'):
            path = parts[index]
            index += 1
        else:
            path = previous
        selected.append((path, previous, status))
    untracked = git(root, 'ls-files', '--others', '--exclude-standard', '-z').stdout
    untracked_paths = [p for p in untracked.decode('utf-8', 'surrogateescape').split('\0') if p]
    if working_tree:
        selected.extend((p, None, '?') for p in untracked_paths)
    files, candidates, violations = [], [], []
    fingerprint = hashlib.sha256()
    for path, previous, status in sorted(selected):
        if status[0] == 'U':
            raise ValueError('Unmerged path must be resolved before review: ' + path)
        old = b'' if status[0] in ('A', '?') else blob(root, base, previous)
        if status[0] == 'D':
            new = b''
        elif working_tree:
            candidate_path = root / path
            if candidate_path.is_symlink() or not candidate_path.is_file():
                raise ValueError('Cannot inspect non-regular candidate: ' + path)
            new = candidate_path.read_bytes()
        else:
            new = blob(root, head, path)
        fingerprint.update(json.dumps([path, previous, status,
                           hashlib.sha256(old).hexdigest(), hashlib.sha256(new).hexdigest()],
                          ensure_ascii=True).encode())
        row, hits, definite = inspect_file(path, previous, status, old, new)
        files.append(row)
        candidates.extend(hits)
        violations.extend(definite)
    check = git(root, 'diff', '--check', base, *target, '--', allowed=(0, 1, 2, 3))
    whitespace = check.stdout.decode('utf-8', 'replace').strip()
    # git diff --check excludes untracked files. Match its default checks here.
    untracked_whitespace = []
    if working_tree:
        for row in files:
            if row['status'] == '?' and not row['binary']:
                for number, line in enumerate((root / row['path']).read_bytes().decode('utf-8', 'replace').splitlines(), 1):
                    if re.search(r'[ \t]+$', line) or re.match(r'^ +\t', line):
                        untracked_whitespace.append(row['path'] + ':' + str(number) + ': whitespace candidate')
    pairs = {}
    for row in files:
        if row['binary'] or Path(row['path']).suffix.lower() not in CPP:
            continue
        key = str(Path(row['path']).with_suffix('')).replace('\\', '/')
        pair = pairs.setdefault(key, {'path_stem': key, 'changed_files': [], 'base_lines': 0,
                                     'candidate_lines': 0, 'net_lines': 0})
        pair['changed_files'].append(row['path'])
        for field in ('base_lines', 'candidate_lines', 'net_lines'):
            pair[field] += row[field]
    state_after = git(root, 'status', '--porcelain=v1', '-z', '--untracked-files=all').stdout
    if state_before != state_after or commit(root, 'HEAD') != checkout_head:
        raise ValueError('Repository state changed during review; rerun on a stable candidate.')
    return {
        'schema_version': 1,
        'scope': {'root': str(root), 'base': base, 'candidate': 'working-tree' if working_tree else head,
                  'head_at_review': checkout_head, 'fingerprint': fingerprint.hexdigest(),
                  'working_changes_included': working_tree,
                  'local_status': state_after.decode('utf-8', 'replace').replace('\0', '\n').rstrip(),
                  'untracked_paths': untracked_paths, 'empty': not files},
        'files': files, 'changed_cpp_pairs': sorted(pairs.values(), key=lambda p: (-p['net_lines'], p['path_stem'])),
        'policy_violations': violations, 'review_candidates': candidates,
        'whitespace': {'exit_code': check.returncode, 'details': whitespace,
                       'untracked_candidates': untracked_whitespace},
        'limitations': [
            'Keyword candidates are not callback reachability or safety findings.',
            'Lexical filtering does not evaluate macros, inactive preprocessor branches or C++ semantics.',
            'Physical line and test declaration counts do not measure complexity or coverage.',
            'Pair totals cover changed files only; inspect the unchanged counterpart when assessing class size.',
            'Working-tree content must remain stable; the fingerprint identifies bytes read, not an atomic filesystem snapshot.',
        ],
    }


def display(report):
    scope = report['scope']
    print('Base: ' + scope['base'])
    print('Candidate: ' + scope['candidate'])
    print('Scope fingerprint: ' + scope['fingerprint'])
    print('Local changes: ' + ('included' if scope['working_changes_included'] else 'EXCLUDED from committed review'))
    if scope['local_status']:
        print(scope['local_status'])
    if scope['empty']:
        print('EMPTY SCOPE: no changes inspected; this is not review approval.')
    print('\nChanged C++ sizes (physical lines; growth is a review signal):')
    for row in sorted(report['files'], key=lambda f: (-f.get('net_lines', 0), f['path'])):
        if Path(row['path']).suffix.lower() not in CPP:
            continue
        if row['binary']:
            print(row['path'] + ': binary, not scanned')
            continue
        print('{path}: {base_lines} -> {candidate_lines} ({net_lines:+}); +{added_lines}/-{removed_lines}{watch}'.format(
            **row, watch=' [WATCHED CLASS]' if row['watched_class'] else ''))
        if row.get('added_test_declarations') or row.get('removed_test_declarations'):
            print('  Test declarations: +{added_test_declarations}/-{removed_test_declarations} (includes renames/disabled declarations)'.format(**row))
    print('\nChanged header/implementation totals (changed files only):')
    for pair in report['changed_cpp_pairs']:
        print('{path_stem}: {base_lines} -> {candidate_lines} ({net_lines:+})'.format(**pair))
    print('\nPolicy violations: ' + str(len(report['policy_violations'])))
    for item in report['policy_violations']:
        print('{path}:{line}: {rule}'.format(**item))
    print('\nReview candidates (require call-site and ownership inspection): ' + str(len(report['review_candidates'])))
    for item in report['review_candidates']:
        print('{path}:{line}: [{kind}] {text}'.format(**item))
    ws = report['whitespace']
    print('\nDiff whitespace exit: ' + str(ws['exit_code']))
    if ws['details']:
        print(ws['details'])
    for item in ws['untracked_candidates']:
        print(item)
    print('\nLimitations:')
    for item in report['limitations']:
        print('- ' + item)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', default='.', help='Selected Jamma checkout (default: current directory)')
    parser.add_argument('--base', required=True, help='Explicit comparison commit/ref; branch reviews should use a resolved merge base')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--head', help='Committed candidate (default: HEAD)')
    mode.add_argument('--working-tree', action='store_true', help='Compare base with current files, including staged, unstaged and untracked')
    parser.add_argument('--format', choices=['text', 'json'], default='text')
    args = parser.parse_args()
    try:
        report = review(args.repo, args.base, args.head, args.working_tree)
    except (ValueError, OSError) as error:
        print('Review checker error: ' + str(error), file=sys.stderr)
        return 2
    if args.format == 'json':
        print(json.dumps(report, indent=2, ensure_ascii=True))
    else:
        display(report)
    return 1 if report['policy_violations'] or report['whitespace']['exit_code'] or report['whitespace']['untracked_candidates'] else 0


if __name__ == '__main__':
    sys.exit(main())
