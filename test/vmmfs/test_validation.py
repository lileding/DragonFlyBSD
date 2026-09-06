"""Construction invariants belong to constructors, not repeated consumers."""
from pathlib import Path
import unittest
from refactor_mount import Client, Document, TARGETS, walk
from refactor_redundancy import ancestor, compact


class ValidationBoundaries(unittest.TestCase):
    def test_ancestor_is_not_runtime_field(self):
        self.assertTrue(ancestor("vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot))"))
        self.assertFalse(ancestor("vmmfs_pciroot_machine(pciroot)->pciroot_vnode"))
        self.assertFalse(ancestor("vmmfs_pciroot_machine(pciroot)->memory.run_vmspace"))
        self.assertFalse(ancestor("vmmfs_pciroot_machine(pciroot) == other"))

    def test_constructor_contract(self):
        root = Path(__file__).resolve().parents[2] / "sys/vfs/vmmfs"
        client = Client(root)
        found = set()
        try:
            for path in sorted(root.glob("vmmfs*.c")):
                text = path.read_text()
                doc = Document(path, text, client.tree(path, text))
                for function in doc.tree.get("children", []):
                    name = function.get("detail")
                    if function.get("kind") != "Function" or name not in TARGETS:
                        continue
                    if not any(n.get("kind") == "Compound" for n in function.get("children", [])):
                        continue
                    found.add(name)
                    for node in walk(function):
                        if node.get("kind") == "Var":
                            self.assertNotIn("'struct vmmfs_mount *'", node.get("arcana", ""), name)
                        if node.get("kind") == "If":
                            condition = compact(doc.code(node["children"][0]))
                            self.assertNotIn("_vops == NULL", condition, name)
                            self.assertNotRegex(condition, r"\broot == NULL", name)
                            self.assertNotEqual(condition, "parent != NULL", name)
        finally:
            client.close()
        self.assertEqual(found, TARGETS)


if __name__ == "__main__":
    unittest.main()
