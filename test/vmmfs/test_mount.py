#!/usr/bin/env python3
"""Check the mount migration with fresh clangd ASTs, not textual matches."""
from pathlib import Path
import subprocess
import unittest

from refactor_mount import Client, Document, TARGETS, member, parent_mount, walk


class MountInheritance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        repo = Path(__file__).resolve().parents[2]
        root = repo / "sys/vfs/vmmfs"
        names = subprocess.check_output(
            ["git", "ls-files", "sys/vfs/vmmfs"], cwd=repo, text=True).splitlines()
        client = Client(root)
        cls.documents = []
        try:
            for name in names:
                path = repo / name
                if path.suffix not in (".c", ".h"):
                    continue
                text = path.read_text()
                cls.documents.append(Document(path, text, client.tree(path, text)))
        finally:
            client.close()

    def test_constructor_parameters(self):
        declarations, definitions, calls = [], set(), []
        for doc in self.documents:
            for node in walk(doc.tree):
                if node.get("kind") == "Function" and node.get("detail") in TARGETS:
                    proto = next(n for n in node["children"] if n.get("kind") == "FunctionProto")
                    params = [n for n in proto["children"] if n.get("kind") == "ParmVar"]
                    self.assertIn("'struct vmmfs_node *'", params[0]["arcana"])
                    self.assertFalse(any("'struct vmmfs_mount *'" in p["arcana"] for p in params))
                    declarations.append(node["detail"])
                    if any(n.get("kind") == "Compound" for n in node["children"]):
                        definitions.add(node["detail"])
                if node.get("kind") == "Call":
                    refs = [n for n in walk(node["children"][0]) if n.get("kind") == "DeclRef"]
                    if len(refs) == 1 and refs[0].get("detail") in TARGETS:
                        self.assertIn("'struct vmmfs_node *'", node["children"][1]["arcana"])
                        calls.append(refs[0]["detail"])
        self.assertEqual(definitions, TARGETS)
        self.assertEqual(len(declarations), 32)
        self.assertEqual(len(calls), 17)

    def test_mount_field_has_one_owner(self):
        found = []
        for doc in self.documents:
            for node in walk(doc.tree):
                if node.get("kind") == "Record" and node.get("detail") in ("vmmfs_node", "vmmfs_machine"):
                    fields = [n for n in node.get("children", []) if n.get("kind") == "Field"]
                    if not fields:
                        continue
                    mounts = [n for n in fields if n.get("detail") == "mount"]
                    if node["detail"] == "vmmfs_node":
                        self.assertEqual(len(mounts), 1)
                        self.assertIn("'struct vmmfs_mount *'", mounts[0]["arcana"])
                        found.append(node["detail"])
                    else:
                        self.assertFalse(mounts)
                self.assertFalse(member(node, "mount", "struct vmmfs_machine *"))
        self.assertEqual(found, ["vmmfs_node"])

    def test_every_parent_assignment_inherits_mount(self):
        inherited = []
        for doc in self.documents:
            for block in walk(doc.tree):
                if block.get("kind") != "Compound":
                    continue
                statements = block.get("children", [])
                for index, statement in enumerate(statements):
                    if statement.get("kind") != "BinaryOperator" or statement.get("detail") != "=":
                        continue
                    lhs, rhs = statement["children"]
                    if not member(lhs, "parent", "struct vmmfs_node"):
                        continue
                    target = doc.code(lhs).removesuffix("parent") + "mount"
                    value = "state" if target == "root->node.mount" else parent_mount(doc, rhs)
                    self.assertEqual(doc.code(statements[index + 1]), target + " = " + value)
                    inherited.append(target)
        self.assertEqual(len(inherited), 19)


if __name__ == "__main__":
    unittest.main(verbosity=2)
