"""Shared VOP ownership and callback wiring."""
from pathlib import Path
import re
import unittest

BASE = Path(__file__).resolve().parents[2] / "sys/vfs/vmmfs"
OBJECTS = ("root", "machine", "pciroot", "pcislot", "serialroot",
           "vcpu", "memory", "loader", "machine_id")

class SharedVops(unittest.TestCase):
    def test_objects_share_mount_vector(self):
        for name in OBJECTS:
            s = (BASE / ("vmmfs_" + name + ".c")).read_text()
            self.assertNotIn("struct vop_ops vmmfs_" + name + "_vops", s)
            self.assertIn("->node_vops", s)
        s = (BASE / "vmmfs.c").read_text()
        self.assertEqual(len(re.findall(r"vfs_add_vnodeops\(mount, &vmmfs_node_vops", s)), 1)
        self.assertEqual(len(re.findall(r"vfs_rm_vnodeops\(mount, NULL, &state->node_vops", s)), 2)

    def test_directory_callbacks(self):
        for name in ("serialroot", "pcislot"):
            s = (BASE / ("vmmfs_" + name + ".c")).read_text()
            for op in ("get_item", "read_item", "remove_item"):
                self.assertIn(".node." + op, s.replace("->node.", ".node."))
            for op in ("nresolve", "readdir", "nremove", "ncreate"):
                self.assertNotIn("vmmfs_" + name + "_" + op + "(", s)
        self.assertIn("node.create_item = vmmfs_serialroot_create_item",
                      (BASE / "vmmfs_serialroot.c").read_text())
