"""The public event set contains only emitted, named events."""
from pathlib import Path
import re
import unittest
from test_regress import function, run_c, COMMON

class MachineEvents(unittest.TestCase):
    def test_event_set_and_names(self):
        root = Path(__file__).resolve().parents[2]
        header = (root/"sys/sys/vmmfs.h").read_text()
        enum = re.search(r"enum vmmfs_machine_event \{.*?\};", header, re.S).group(0)
        names = re.findall(r"VMMFS_MACHINE_EVENT_[A-Z_]+", enum)
        source = root/"sys/vfs/vmmfs"
        emitters = "\n".join(p.read_text() for p in source.glob("*.c") if p.name != "vmmfs_events.c")
        body = function("vmmfs_events.c", "vmmfs_machine_event_name")
        self.assertEqual(len(names), 15)
        for name in names:
            self.assertIn(name, emitters)
            self.assertIn("case " + name + ":", body)
        checks = "\n".join("assert(vmmfs_machine_event_name("+name+") != NULL);" for name in names)
        run_c(COMMON + enum + "\nstatic const char *\n" + body +
              "\nint main(void) {" + checks +
              "assert(vmmfs_machine_event_name(0) == NULL); return 0;}")
