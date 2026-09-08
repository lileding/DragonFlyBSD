#!/usr/bin/env python3
"""One-shot clangd AST migration of VMMFS mount ownership.

Run plan/apply for layout, fields, signatures in order. Plans contain exact
edits and hashes; apply never reparses or guesses at changed input.
"""
import argparse
import ast
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

TARGETS = {
    "vmmfs_" + name for name in (
        "boot_init", "events_init", "launch_create", "loader_init",
        "machine_create", "machine_id_init", "memory_init", "pciroot_init",
        "pcislot_create", "pcislot_config_init", "pcislot_descriptor_init",
        "pcislot_events_init", "serialport_create", "serialroot_init",
        "stopped_create", "vcpu_init",
    )
}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def walk(node):
    yield node
    for child in node.get("children", []):
        yield from walk(child)


class Client:
    def __init__(self, root):
        self.process = subprocess.Popen(
            ["clangd", "--background-index=false", "--clang-tidy=false",
             "--log=error", "--compile-commands-dir=" + str(root)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        )
        self.serial = 0
        self.request("initialize", {
            "processId": None, "rootUri": root.as_uri(), "capabilities": {},
        })
        self.send("initialized", {})

    def send(self, method, params, serial=None):
        value = {"jsonrpc": "2.0", "method": method, "params": params}
        if serial is not None:
            value["id"] = serial
        body = json.dumps(value).encode()
        self.process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
        self.process.stdin.flush()

    def receive(self):
        length = None
        while True:
            line = self.process.stdout.readline()
            require(line, "clangd exited before replying")
            if line in (b"\r\n", b"\n"):
                break
            key, value = line.split(b":", 1)
            if key.lower() == b"content-length":
                length = int(value)
        require(length is not None, "missing LSP message length")
        return json.loads(self.process.stdout.read(length))

    def request(self, method, params):
        self.serial += 1
        self.send(method, params, self.serial)
        while True:
            value = self.receive()
            if value.get("id") != self.serial:
                continue
            require("error" not in value, str(value))
            return value.get("result")

    def tree(self, path, text):
        uri = path.as_uri()
        self.send("textDocument/didOpen", {"textDocument": {
            "uri": uri, "languageId": "c", "version": 1, "text": text,
        }})
        tree = self.request("textDocument/ast", {"textDocument": {"uri": uri}})
        require(tree and tree.get("kind") == "TranslationUnit", f"no AST: {path}")
        return tree

    def close(self):
        try:
            self.request("shutdown", None)
            self.send("exit", None)
            self.process.wait(timeout=5)
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.process.stdin.close()
            self.process.stdout.close()


class Document:
    def __init__(self, path, text, tree):
        self.path, self.text, self.tree = path, text, tree
        self.lines = text.splitlines(keepends=True)
        require(text.isascii(), f"non-ASCII source needs UTF-16 conversion: {path}")
        self.offsets = [0]
        for line in self.lines:
            self.offsets.append(self.offsets[-1] + len(line))
        self.edits = []

    def position(self, pos):
        return self.offsets[pos["line"]] + pos["character"]

    def span(self, node):
        r = node["range"]
        return self.position(r["start"]), self.position(r["end"])

    def code(self, node):
        a, b = self.span(node)
        return self.text[a:b]

    def edit(self, a, b, replacement, why):
        self.edits.append({"start": a, "end": b, "old": self.text[a:b],
                           "new": replacement, "why": why})

    def statement(self, node):
        a, b = self.span(node)
        require(self.text[b:b + 1] == ";", f"not a statement: {self.code(node)}")
        return a, b + 1

    def delete_line(self, node, why):
        a, b = self.statement(node)
        start = self.text.rfind("\n", 0, a) + 1
        end = self.text.find("\n", b)
        require(not self.text[start:a].strip() and not self.text[b:end].strip(),
                "expected an isolated declaration/assignment")
        self.edit(start, end + 1, "", why)

    def result(self):
        edits = sorted(self.edits, key=lambda e: (e["start"], e["end"]))
        for left, right in zip(edits, edits[1:]):
            require(left["end"] <= right["start"], f"overlapping edits: {self.path}")
        text = self.text
        for edit in reversed(edits):
            text = text[:edit["start"]] + edit["new"] + text[edit["end"]:]
        return text


def member(node, name, receiver):
    return (node.get("kind") == "Member" and node.get("detail") == name
            and node.get("children")
            and "'" + receiver + "'" in node["children"][0].get("arcana", ""))


def parent_mount(doc, rhs):
    while rhs.get("kind") in ("ImplicitCast", "Paren"):
        rhs = rhs["children"][0]
    if rhs.get("kind") == "UnaryOperator" and rhs.get("detail") == "&":
        return doc.code(rhs["children"][0]) + ".mount"
    require(rhs.get("kind") == "DeclRef" and rhs.get("detail") == "parent",
            "unexpected parent expression: " + doc.code(rhs))
    return "parent->mount"


def layout(doc):
    count = 0
    for node in walk(doc.tree):
        if node.get("kind") == "Record" and node.get("detail") == "vmmfs_node":
            fields = [n for n in node.get("children", []) if n.get("kind") == "Field"]
            if fields:
                require(not any(n.get("detail") == "mount" for n in fields), "already migrated")
                parent = next(n for n in fields if n.get("detail") == "parent")
                _, end = doc.statement(parent)
                doc.edit(end, end, "\n\tstruct vmmfs_mount *mount;", "node mount field")
                forward = next(n for n in doc.tree["children"]
                               if n.get("kind") == "Record" and n.get("detail") == "mount")
                _, a = doc.statement(forward)
                doc.edit(a, a, "\nstruct vmmfs_mount;", "mount forward declaration")
        if node.get("kind") != "BinaryOperator" or node.get("detail") != "=":
            continue
        lhs, rhs = node["children"]
        if not member(lhs, "parent", "struct vmmfs_node"):
            continue
        _, end = doc.statement(node)
        line = doc.lines[node["range"]["start"]["line"]]
        indent = line[:len(line) - len(line.lstrip())]
        target = doc.code(lhs).removesuffix("parent") + "mount"
        value = "state" if target == "root->node.mount" else parent_mount(doc, rhs)
        doc.edit(end, end, f"\n{indent}{target} = {value};", "inherit mount: " + target)
        count += 1
    return count


def fields(doc):
    removed = []
    for node in walk(doc.tree):
        if node.get("kind") == "Record" and node.get("detail") == "vmmfs_machine":
            for field in node.get("children", []):
                if field.get("kind") == "Field" and field.get("detail") == "mount":
                    doc.delete_line(field, "remove duplicate machine mount field")
        if node.get("kind") == "BinaryOperator" and node.get("detail") == "=":
            lhs = node["children"][0]
            if member(lhs, "mount", "struct vmmfs_machine *"):
                require(doc.code(node) == "machine->mount = mount", "unexpected mount writer")
                doc.delete_line(node, "remove redundant mount initialization")
                removed.append(doc.span(node))
    for node in walk(doc.tree):
        if member(node, "mount", "struct vmmfs_machine *"):
            a, b = doc.span(node)
            if any(x <= a and b <= y for x, y in removed):
                continue
            require(doc.text[b - 5:b] == "mount", "invalid member range")
            doc.edit(b - 5, b, "node.mount", "machine mount reference")


def signatures(doc, counts, calls):
    for node in walk(doc.tree):
        name = node.get("detail")
        if node.get("kind") == "Function" and name in TARGETS:
            proto = next(n for n in node["children"] if n.get("kind") == "FunctionProto")
            params = [n for n in proto["children"] if n.get("kind") == "ParmVar"]
            require("'struct vmmfs_mount *'" in params[0]["arcana"], "first parameter type")
            require("'struct vmmfs_node *'" in params[1]["arcana"], "second parameter type")
            a, _ = doc.span(params[0]); b, _ = doc.span(params[1])
            doc.edit(a, b, "", "remove mount parameter: " + name)
            body = next((n for n in node["children"] if n.get("kind") == "Compound"), None)
            if body:
                require(params[1].get("detail") == "parent", "unexpected parent parameter")
                a, _ = doc.span(body)
                indent = "    " if name == "vmmfs_serialport_create" else "\t"
                doc.edit(a + 1, a + 1,
                         "\n" + indent + "struct vmmfs_mount *mount = parent == NULL ? NULL : parent->mount;",
                         "derive local mount: " + name)
                counts.add(name)
        if node.get("kind") == "Call":
            children = node.get("children", [])
            refs = [n for n in walk(children[0]) if n.get("kind") == "DeclRef"]
            if len(refs) != 1 or refs[0].get("detail") not in TARGETS:
                continue
            name = refs[0]["detail"]
            first, second = children[1:3]
            require("'struct vmmfs_mount *'" in first["arcana"], "call first argument type")
            allowed = {"ImplicitCast", "DeclRef", "Member"}
            require(all(n.get("kind") in allowed for n in walk(first)),
                    "mount argument may have side effects: " + doc.code(first))
            a, _ = doc.span(first); b, _ = doc.span(second)
            doc.edit(a, b, "", "remove mount argument: " + name)
            calls.append({"file": str(doc.path), "callee": name,
                          "mount": doc.code(first), "parent": doc.code(second)})
    # These two callback signatures stay unchanged; their old mount lookup
    # served only the now-deleted constructor argument.
    for node in doc.tree.get("children", []):
        if node.get("kind") != "Function" or node.get("detail") not in (
                "vmmfs_root_create_object", "vmmfs_pciroot_create_object"):
            continue
        for statement in walk(node):
            if statement.get("kind") != "Decl":
                continue
            variables = statement.get("children", [])
            if len(variables) == 1 and variables[0].get("detail") == "state":
                require(doc.code(statement) == "struct vmmfs_mount *state = (struct vmmfs_mount *)mount->mnt_data;",
                        "unexpected callback mount lookup")
                a, b = doc.span(statement)
                a = doc.text.rfind("\n", 0, a) + 1
                b = doc.text.find("\n", b) + 1
                doc.edit(a, b, "", "remove unused callback state lookup")

# C fixtures are Python string literals, not production translation units.
# Locate the owning Python symbol and edit only explicitly listed literals.
FIXTURES = {
    "test_constructors.py": {
        "HARNESS": [
            ("struct vmmfs_node {\n", "struct vmmfs_node {\n    struct vmmfs_mount *mount;\n"),
            ("struct vmmfs_node node; struct vmmfs_mount *mount;", "struct vmmfs_node node;"),
            ("child_init(struct vmmfs_mount *m, struct vmmfs_node *p,", "child_init(struct vmmfs_node *p,"),
            ("assert(m); *vp = NULL;", "assert(p->mount); *vp = NULL;"),
            ("child_init(m->mount, &m->node,", "child_init(&m->node,"),
            ("c->node.parent = p;", "c->node.parent = p; c->node.mount = p->mount;"),
        ],
        "test_every_composite_construction_failure_returns_ownership": [
            ("parent = { .references = 1 }", "parent = { .references = 1, .mount = &mount }"),
            ("vmmfs_machine_create(&mount, &parent,", "vmmfs_machine_create(&parent,"),
            ("vmmfs_pcislot_create(&mount, &parent,", "vmmfs_pcislot_create(&parent,"),
        ],
        "test_launch_creation_releases_cdev_and_parent_on_failure": [
            ("struct vmmfs_node {\n", "struct vmmfs_node {\n    struct vmmfs_mount *mount;\n"),
            ("struct vmmfs_mount mount = { &parent, &parent, &root };", "struct vmmfs_mount mount = { &parent, &parent, &root };\n    parent.mount = &mount;"),
            ("vmmfs_launch_create(&mount, &parent,", "vmmfs_launch_create(&parent,"),
            ("assert(launch->result == EINPROGRESS", "assert(launch->node.mount == parent.mount);\n            assert(launch->result == EINPROGRESS"),
        ],
    },
    "test_regress.py": {
        "test_identity_failed_vnode_releases_parent": [
            ("struct vmmfs_node {\n", "struct vmmfs_node {\n    struct vmmfs_mount *mount;\n"),
            ("machine.node.references = 1;", "machine.node.references = 1;\n    machine.node.mount = &mount;"),
            ("vmmfs_machine_id_init(&mount, &machine.node,", "vmmfs_machine_id_init(&machine.node,"),
        ],
    },
    "test_parent.py": {
        "test_stop_admission_rejects_retired_siblings": [
            ("void *machine, *mount;", "void *machine;"),
            ("vmmfs_stopped_create(void *mount, struct vmmfs_node *parent,", "vmmfs_stopped_create(struct vmmfs_node *parent,"),
            ("(void)mount; assert(parent->token.held == 0);", "assert(parent->token.held == 0);"),
        ],
    },
    "test_prepare.py": {
        "test_stopped_publication_token_order": [
            ("void *machine, *mount;", "void *machine;"),
            ("vmmfs_stopped_create(void *mount, struct vmmfs_node *parent,", "vmmfs_stopped_create(struct vmmfs_node *parent,"),
            ("(void)mount; assert(parent == &machine.node);", "assert(parent == &machine.node);"),
        ],
        "test_private_prepare_retains_vcpu": [
            ("struct vmmfs_node {", "struct vmmfs_node { struct vmmfs_mount *mount;"),
            ("void *mount; vmm_machine_t machine;", "vmm_machine_t machine;"),
            ("vmmfs_launch_create(void *m, struct vmmfs_node *parent,", "vmmfs_launch_create(struct vmmfs_node *parent,"),
            ("(void)m; (void)size;", "(void)size;"),
            ("launch.node.parent = parent;", "launch.node.parent = parent; launch.node.mount = parent->mount;"),
        ],
    },
}


def fixture_documents(repo):
    docs = []
    for name, scopes in FIXTURES.items():
        path = repo / "test/vmmfs" / name
        text = path.read_text()
        tree = ast.parse(text)
        doc = Document(path, text, {})
        for owner, replacements in scopes.items():
            matches = [n for n in ast.walk(tree) if
                       (isinstance(n, ast.FunctionDef) and n.name == owner) or
                       (isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == owner for t in n.targets))]
            require(len(matches) == 1, "ambiguous fixture owner: " + owner)
            strings = [n for n in ast.walk(matches[0]) if isinstance(n, ast.Constant) and isinstance(n.value, str)]
            segments = {id(n): ast.get_source_segment(text, n) for n in strings}
            for old, new in replacements:
                require(sum(s.count(old) for s in segments.values()) == 1,
                        f"fixture pattern mismatch: {owner}: {old}")
                for key, value in segments.items():
                    if old in value:
                        segments[key] = value.replace(old, new, 1)
                        break
            for node in strings:
                before = ast.get_source_segment(text, node)
                after = segments[id(node)]
                if before != after:
                    a = doc.offsets[node.lineno - 1] + node.col_offset
                    b = doc.offsets[node.end_lineno - 1] + node.end_col_offset
                    doc.edit(a, b, after, "fixture literal: " + owner)
        ast.parse(doc.result())
        docs.append(doc)
    return docs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("plan", "apply"))
    parser.add_argument("stage", choices=("layout", "fields", "signatures", "fixtures"))
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    root = repo / "sys/vfs/vmmfs"
    if args.mode == "apply":
        plan = json.loads(args.manifest.read_text())
        require(plan["stage"] == args.stage and plan["root"] == str(root), "wrong plan")
        require(plan["head"] == subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(), "HEAD changed")
        for name, sha in plan["inputs"].items():
            require(digest(Path(name).read_bytes()) == sha, "changed input: " + name)
        updates = []
        for name, edits in plan["edits"].items():
            doc = Document(Path(name), Path(name).read_text(), {})
            doc.edits = edits
            for edit in edits:
                require(doc.text[edit["start"]:edit["end"]] == edit["old"], "stale edit")
            updates.append((doc.path, doc.result()))
        for path, text in updates:
            path.write_text(text)
        print(f"Applied {args.stage}: {len(updates)} files")
        return
    names = subprocess.check_output(["git", "ls-files", "sys/vfs/vmmfs"], cwd=repo, text=True).splitlines()
    paths = [repo / n for n in names if n.endswith((".c", ".h"))]
    if args.stage == "fixtures":
        paths += [repo / "test/vmmfs" / n for n in FIXTURES]
    paths.append(Path(__file__).resolve())
    inputs = {str(p): digest(p.read_bytes()) for p in paths}
    inputs[str(root / "compile_commands.json")] = digest((root / "compile_commands.json").read_bytes())
    docs, counts, calls, inheritance = [], set(), [], 0
    client = None if args.stage == "fixtures" else Client(root)
    try:
        if args.stage == "fixtures":
            docs = fixture_documents(repo)
        for path in paths:
            if args.stage == "fixtures" or path.suffix not in (".c", ".h"):
                continue
            text = path.read_text()
            doc = Document(path, text, client.tree(path, text))
            if args.stage == "layout": inheritance += layout(doc)
            elif args.stage == "fields": fields(doc)
            else: signatures(doc, counts, calls)
            doc.result()
            docs.append(doc)
    finally:
        if client is not None:
            client.close()
    if args.stage == "signatures": require(counts == TARGETS, f"missing definitions: {TARGETS - counts}")
    if args.stage == "layout": require(inheritance == 20, f"unexpected parent assignments: {inheritance}")
    plan = {"stage": args.stage, "root": str(root), "inputs": inputs,
            "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
            "calls": calls, "edits": {str(d.path): d.edits for d in docs if d.edits}}
    args.manifest.write_text(json.dumps(plan, indent=2) + "\n")
    patch = "".join("".join(difflib.unified_diff(d.text.splitlines(True), d.result().splitlines(True),
                    fromfile=str(d.path), tofile=str(d.path))) for d in docs if d.edits)
    args.manifest.with_suffix(".diff").write_text(patch)
    print(json.dumps({"files": len(plan["edits"]), "edits": sum(len(d.edits) for d in docs),
                      "inheritance": inheritance, "definitions": sorted(counts), "calls": calls}, indent=2))


if __name__ == "__main__":
    main()
