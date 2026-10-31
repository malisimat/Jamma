"""Small behavior checks for the deterministic inventory calculations."""

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/structure_audit.py"
SPEC = importlib.util.spec_from_file_location("structure_audit", SCRIPT)
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class StructureAuditTests(unittest.TestCase):
    def test_physical_lines_include_unterminated_last_line(self):
        self.assertEqual(0, audit.physical_lines(b""))
        self.assertEqual(2, audit.physical_lines(b"one\r\ntwo\r\n"))
        self.assertEqual(2, audit.physical_lines(b"one\ntwo"))

    def test_scope_excludes_vendor_and_skill_sources(self):
        self.assertTrue(audit.first_party("JammaLib/src/engine/Scene.cpp"))
        self.assertTrue(audit.first_party("test/JammaLib_Tests/src/gui/GuiRack_Tests.cpp"))
        self.assertFalse(audit.first_party("JammaLib/lib/rtaudio/RtAudio.cpp"))
        self.assertFalse(audit.first_party(".agents/skills/jamma-code-clean/scripts/structure_audit.py"))

    def test_growth_ranks_separately_from_current_size(self):
        files = {"JammaLib/src/engine/Scene.cpp": {"lines": 1000, "production": True},
                 "JammaLib/src/gui/New.cpp": {"lines": 200, "production": True}}
        previous = {"files": {"JammaLib/src/engine/Scene.cpp": {"lines": 990}}}
        rows, growth = audit.compare(files, previous)
        self.assertEqual("JammaLib/src/engine/Scene.cpp", rows[0]["path"])
        self.assertEqual(10, rows[0]["delta"])
        self.assertIsNone(rows[1]["delta"])
        self.assertEqual(["JammaLib/src/engine/Scene.cpp"], [row["path"] for row in growth])


if __name__ == "__main__":
    unittest.main()
