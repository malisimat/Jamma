"""Hermetic scope/regression checks; creates only disposable temporary Git repos."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'review_changes.py'
spec = importlib.util.spec_from_file_location('review_changes', SCRIPT)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class ReviewChangesTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git('init', '-q')
        self.git('config', 'user.name', 'Review fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('config', 'core.autocrlf', 'false')
        self.write('Jamma.sln', 'fixture\n')
        self.write('Scene.cpp', 'void Tick() {}\n')
        self.base = self.save()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], stderr=subprocess.PIPE).decode().strip()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def save(self):
        self.git('add', '--all')
        self.git('commit', '-qm', 'fixture')
        return self.git('rev-parse', 'HEAD')

    def test_clean_committed_branch_scans_audiohost_and_counts_growth(self):
        self.write('AudioHost.cpp', 'void Tick() { std::lock_guard guard(mutex); }\n')
        self.write('Scene.cpp', 'void Tick() {}\nvoid NewWork() {}\n')
        head = self.save()
        report = audit.review(self.root, self.base, head)
        self.assertFalse(report['scope']['empty'])
        self.assertTrue(any(c['path'] == 'AudioHost.cpp' and c['kind'] == 'lock_or_wait' for c in report['review_candidates']))
        scene = next(r for r in report['files'] if r['path'] == 'Scene.cpp')
        self.assertEqual((scene['base_lines'], scene['candidate_lines'], scene['net_lines']), (1, 2, 1))
        self.assertTrue(scene['watched_class'])
        self.assertEqual(audit.review(self.root, head, working_tree=True)['files'], [])

    def test_working_scope_includes_staged_unstaged_untracked_and_changes_fingerprint(self):
        self.write('Scene.cpp', 'void Tick() { new Buffer; }\n')
        self.git('add', 'Scene.cpp')
        self.write('Scene.cpp', 'void Tick() { new Buffer; delete buffer; }\n')
        self.write('new helper.h', '\tstd::atomic<int> value;\n')
        report = audit.review(self.root, self.base, working_tree=True)
        self.assertEqual({f['path'] for f in report['files']}, {'Scene.cpp', 'new helper.h'})
        self.assertEqual(report['whitespace']['untracked_candidates'], [])
        committed = audit.review(self.root, self.base, self.base)
        self.assertTrue(committed['scope']['empty'])
        self.assertIn('new helper.h', committed['scope']['untracked_paths'])
        self.write('new helper.h', '\tstd::atomic<long> value;\n')
        self.assertNotEqual(report['scope']['fingerprint'], audit.review(self.root, self.base, working_tree=True)['scope']['fingerprint'])

    def test_rename_deletion_and_binary(self):
        self.write('gone.cpp', 'int gone;\n')
        base = self.save()
        self.git('mv', 'Scene.cpp', 'Renamed Scene.cpp')
        self.git('rm', 'gone.cpp')
        (self.root / 'binary.h').write_bytes(b'\0binary')
        head = self.save()
        report = audit.review(self.root, base, head)
        renamed = next(r for r in report['files'] if r['path'] == 'Renamed Scene.cpp')
        self.assertEqual(renamed['previous_path'], 'Scene.cpp')
        self.assertEqual(renamed['net_lines'], 0)
        self.assertEqual(next(r for r in report['files'] if r['path'] == 'gone.cpp')['candidate_lines'], 0)
        self.assertTrue(next(r for r in report['files'] if r['path'] == 'binary.h')['binary'])

    def test_comments_literals_and_multiline_namespace(self):
        self.write('Scene.cpp', '// namespace { std::mutex m; }\nconst char* s = R"x(namespace { new Thing; })x";\nnamespace\n{\nint real;\n}\n')
        report = audit.review(self.root, self.base, working_tree=True)
        self.assertEqual(report['review_candidates'], [])
        self.assertEqual([(v['kind'], v['line']) for v in report['policy_violations']], [('anonymous_namespace', 3)])

    def test_cli_exit_codes_and_json(self):
        def run(*args):
            return subprocess.run([sys.executable, str(SCRIPT), '--repo', str(self.root), *args], capture_output=True, text=True)
        empty = run('--base', self.base, '--format', 'json')
        self.assertEqual(empty.returncode, 0)
        self.assertTrue(json.loads(empty.stdout)['scope']['empty'])
        self.assertEqual(run('--base', 'no-such-ref').returncode, 2)
        self.write('Bad.h', 'namespace { int bad; }\n')
        self.assertEqual(run('--base', self.base, '--working-tree').returncode, 1)
        self.write('Bad.h', 'int fine; \n')
        self.assertEqual(run('--base', self.base, '--working-tree').returncode, 1)


if __name__ == '__main__':
    unittest.main()
