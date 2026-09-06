#!/usr/bin/env python3
"""Enumerate AST edits that rely on VMMFS construction invariants."""
import argparse
import difflib
import json
from pathlib import Path
import re
import subprocess

from refactor_mount import Client, Document, TARGETS, digest, require, walk

ANCESTORS = {
    "vmmfs_machine_root", "vmmfs_pciroot_machine",
    "vmmfs_serialroot_machine", "vmmfs_pcislot_pciroot",
    "vmmfs_events_machine", "vmmfs_loader_machine",
    "vmmfs_machine_id_machine", "vmmfs_memory_machine",
    "vmmfs_vcpu_machine", "vmmfs_pcislot_config_slot",
    "vmmfs_pcislot_descriptor_slot", "vmmfs_pcislot_events_slot",
    "vmmfs_pcislot_resource_resources", "vmmfs_pcislot_resources_slot",
}


def compact(text):
    return " ".join(text.split())


def ancestor(text):
    if text.split("(", 1)[0] not in ANCESTORS or "=" in text:
        return False
    depth = 0
    for index in range(text.index("("), len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index == len(text) - 1
    return False


def condition_text(doc, condition, parts):
    start, _ = doc.span(condition)
    prefix = doc.text[doc.text.rfind("\n", 0, start) + 1:start]
    indent = prefix[:len(prefix) - len(prefix.lstrip())] + "    "
    lines = [compact(doc.code(parts[0]))]
    for part in parts[1:]:
        value = compact(doc.code(part))
        head = prefix if len(lines) == 1 else ""
        if len((head + lines[-1] + " || " + value).expandtabs(8)) > 78:
            lines[-1] += " ||"
            lines.append(indent + value)
        else:
            lines[-1] += " || " + value
    return "\n".join(lines)


def variable_id(node):
    pattern = r"VarDecl (0x[0-9a-f]+)" if node.get("kind") == "Var" else r"\bVar (0x[0-9a-f]+)"
    match = re.search(pattern, node.get("arcana", ""))
    return match.group(1) if match else None


def covered(doc, node):
    a, b = doc.span(node)
    return any(e["start"] <= a and b <= e["end"] for e in doc.edits)


def line_edit(doc, node, replacement, why):
    a, b = doc.span(node)
    if doc.text[b:b + 1] == ";":
        b += 1
    start = doc.text.rfind("\n", 0, a) + 1
    end = doc.text.find("\n", b)
    require(not doc.text[start:a].strip() and not doc.text[b:end].strip(),
            "expected isolated statement: " + doc.code(node))
    doc.edit(start, end + 1, replacement, why)


def terms(node):
    if node.get("kind") == "BinaryOperator" and node.get("detail") == "||":
        return terms(node["children"][0]) + terms(node["children"][1])
    return [node]


def constructor(doc, function):
    nodes = list(walk(function))
    name = function["detail"]
    variables = {n["detail"]: n for n in nodes if n.get("kind") == "Var"}
    mount = variables["mount"]
    require(compact(doc.code(mount)).endswith("mount = parent == NULL ? NULL : parent->mount"),
            "unexpected mount initialization: " + name)
    aliases = {variable_id(mount): "parent->mount"}
    state = variables.get("state")
    if state and "'struct vmmfs_mount *'" in state.get("arcana", ""):
        assignments = [n for n in nodes if n.get("kind") == "BinaryOperator"
                       and n.get("detail") == "="
                       and variable_id(n["children"][0]) == variable_id(state)]
        require(len(assignments) == 1 and compact(doc.code(assignments[0]["children"][1]))
                in ("mount", "machine->node.mount"), "unexpected state writer")
        line_edit(doc, assignments[0], "", "remove mount alias assignment")
        aliases[variable_id(state)] = "parent->mount"
    for node in nodes:
        if node.get("kind") == "Decl" and len(node.get("children", [])) == 1:
            if variable_id(node["children"][0]) in aliases:
                line_edit(doc, node, "", "remove mount alias declaration")
        if node.get("kind") == "ConditionalOperator" and compact(doc.code(node)) == (
                "mount->root_vnode == NULL ? NULL : mount->root_vnode->v_data"):
            value = "(struct vmmfs_root *)parent" if name == "vmmfs_machine_create" else (
                "parent->mount->root_vnode->v_data")
            doc.edit(*doc.span(node), value, "use constructed root")
        if node.get("kind") != "If":
            continue
        condition = compact(doc.code(node["children"][0]))
        if condition == "parent != NULL":
            require(compact(doc.code(node["children"][1])) == "vmmfs_node_hold(parent)",
                    "unexpected guarded parent operation")
            indent = "    " if name == "vmmfs_serialport_create" else "\t"
            line_edit(doc, node, indent + "vmmfs_node_hold(parent);\n", "parent already validated")
            continue
        parts = terms(node["children"][0])
        def redundant(part):
            text = compact(doc.code(part))
            return text in ("mount == NULL", "state == NULL", "root == NULL", "machine == NULL") or bool(
                re.fullmatch(r"(?:mount|state)->\w+_vops == NULL", text))
        kept = [p for p in parts if not redundant(p)]
        if len(kept) == len(parts):
            continue
        require(len(node["children"]) == 2, "guard has else branch")
        if not kept:
            line_edit(doc, node, "", "construction invariant guard")
        else:
            value = condition_text(doc, node["children"][0], kept)
            doc.edit(*doc.span(node["children"][0]), value, "retain input checks only")
    # References are bound to declaration IDs, never matched by name alone.
    for node in nodes:
        if node.get("kind") == "DeclRef" and variable_id(node) in aliases and not covered(doc, node):
            doc.edit(*doc.span(node), aliases[variable_id(node)], "inline mount alias reference")
    # Removing ancestor guards can leave a local whose sole purpose was that guard.
    machine = variables.get("machine")
    if machine:
        identity = variable_id(machine)
        assignments = [n for n in nodes if n.get("kind") == "BinaryOperator" and n.get("detail") == "="
                       and variable_id(n["children"][0]) == identity]
        if len(assignments) == 1 and ancestor(compact(doc.code(assignments[0]["children"][1]))):
            references = [n for n in nodes if n.get("kind") == "DeclRef" and variable_id(n) == identity
                          and not covered(doc, n) and doc.span(n) != doc.span(assignments[0]["children"][0])]
            if not references:
                line_edit(doc, assignments[0], "", "remove guard-only ancestor lookup")
                declaration = next(n for n in nodes if n.get("kind") == "Decl" and machine in n.get("children", []))
                line_edit(doc, declaration, "", "remove guard-only ancestor local")


def unused_locals(doc, function):
    nodes = list(walk(function))
    changed = True
    while changed:
        changed = False
        for declaration in nodes:
            if declaration.get("kind") != "Decl" or covered(doc, declaration):
                continue
            children = declaration.get("children", [])
            if len(children) != 1 or children[0].get("kind") != "Var":
                continue
            identity = variable_id(children[0])
            writes = [n for n in nodes if n.get("kind") == "BinaryOperator" and n.get("detail") == "="
                      and variable_id(n["children"][0]) == identity and not covered(doc, n)]
            if len(writes) != 1:
                continue
            rhs = compact(doc.code(writes[0]["children"][1]))
            if not (ancestor(rhs) or re.fullmatch(r"\(struct vmmfs_\w+ \*\)parent", rhs)):
                continue
            write_span = doc.span(writes[0]["children"][0])
            refs = [n for n in nodes if n.get("kind") == "DeclRef" and variable_id(n) == identity
                    and not covered(doc, n) and doc.span(n) != write_span]
            if refs:
                continue
            line_edit(doc, writes[0], "", "unused side-effect-free ancestor lookup")
            line_edit(doc, declaration, "", "unused ancestor local")
            changed = True


def internal(doc, function):
    nodes = list(walk(function))
    ancestors = set()
    for declaration in (n for n in nodes if n.get("kind") == "Var"):
        writes = [n for n in nodes if n.get("kind") == "BinaryOperator" and n.get("detail") == "="
                  and variable_id(n["children"][0]) == variable_id(declaration)]
        if len(writes) == 1 and "range" in writes[0]["children"][1] and ancestor(
                compact(doc.code(writes[0]["children"][1]))):
            ancestors.add(declaration["detail"])
    if function.get("detail") in TARGETS:
        for node in (n for n in nodes if n.get("kind") == "Label"):
            if not any(n.get("kind") in ("Goto", "AddrLabel") for n in nodes):
                a, b = doc.span(node)
                doc.edit(a, doc.text.index("\n", a) + 1, "", "remove unused failure label")
    for node in nodes:
        if node.get("kind") != "If" or "range" not in node or covered(doc, node):
            continue
        parts = terms(node["children"][0])
        if any("range" not in part for part in parts):
            continue
        removed = []
        assignments = []
        for part in parts:
            text = compact(doc.code(part))
            if text.endswith(" == NULL") and (ancestor(text[:-8]) or text[:-8] in ancestors):
                removed.append(part)
            elif function.get("detail") in ("vmmfs_pcislot_descriptor_load", "vmmfs_pcislot_descriptor_store"):
                if text.startswith("(") and text.endswith(") == NULL"):
                    assignment = next(n for n in walk(part) if n.get("kind") == "BinaryOperator"
                                      and n.get("detail") == "=")
                    require(ancestor(compact(doc.code(assignment["children"][1]))), "non-parent assignment")
                    assignments.append(doc.code(assignment) + ";")
                    removed.append(part)
            if text in {"ap->a_vp == NULL", "ap->a_dvp == NULL", "ap->a_vap == NULL",
                        "ap->a_lvap == NULL", "ap->a_uio == NULL"}:
                removed.append(part)
        if not removed:
            continue
        kept = [p for p in parts if p not in removed]
        require(len(node["children"]) == 2, "guard has else branch")
        if assignments:
            require(len(kept) == 1 and compact(doc.code(kept[0])) == "descriptor == NULL", "assignment ordering")
            _, end = doc.statement(node)
            doc.edit(end, end, "\n\t" + "\n\t".join(assignments), "preserve parent assignments after argument guard")
        if kept:
            doc.edit(*doc.span(node["children"][0]), condition_text(doc, node["children"][0], kept),
                     "retain outer argument check, trust parent/VOP contract")
        else:
            line_edit(doc, node, "", "parent held until child drop")


def finish(doc, function):
    name = function.get("detail")
    nodes = list(walk(function))
    for node in nodes:
        kind = node.get("kind")
        if "range" not in node or covered(doc, node):
            continue
        text = compact(doc.code(node))
        if name == "vmmfs_root_create" and kind == "If" and compact(doc.code(node["children"][0])) == (
                "state == NULL || state->root_vops == NULL"):
            line_edit(doc, node, "", "mount installs vops before creating root")
        if name == "vmmfs_node_parent" and kind == "ConditionalOperator" and text == (
                "node == NULL || node->parent == NULL ? NULL : node->parent"):
            doc.edit(*doc.span(node), "node == NULL ? NULL : node->parent", "one-level parent accessor")
        if name != "vmmfs_pcislot_resources_create":
            continue
        if kind == "Var" and text in ("struct vmmfs_mount *mount", "struct vmmfs_machine *machine_owner"):
            line_edit(doc, node, "", "remove indirect mount local")
        if kind == "BinaryOperator" and text in (
                "machine_owner = vmmfs_pciroot_machine(vmmfs_pcislot_pciroot(slot))",
                "mount = machine_owner == NULL ? NULL : machine_owner->node.mount"):
            line_edit(doc, node, "", "remove indirect mount lookup")
        if kind == "If" and compact(doc.code(node["children"][0])) == (
                "mount == NULL || mount->pcislot_resource_vops == NULL"):
            line_edit(doc, node, "", "resource inherits initialized mount")
        if kind == "Call" and text == "vmmfs_root_allocate_inode( vmmfs_machine_root(machine_owner))":
            doc.edit(*doc.span(node), "vmmfs_root_allocate_inode(\n\t\t    slot->node.mount->root_vnode->v_data)",
                     "use inherited root for inode allocation")
        if kind == "DeclRef" and node.get("detail") == "mount":
            doc.edit(*doc.span(node), "slot->node.mount", "use slot mount")


def formatting(doc, function):
    if function.get("detail") not in TARGETS:
        return
    for node in walk(function):
        if "range" not in node or covered(doc, node):
            continue
        if node.get("kind") == "If":
            condition = node["children"][0]
            if "range" in condition and len(doc.code(condition).expandtabs(8)) > 78:
                parts = terms(condition)
                if all("range" in part for part in parts):
                    value = condition_text(doc, condition, parts)
                    if value != doc.code(condition):
                        doc.edit(*doc.span(condition), value, "wrap condition after removed terms")
        if node.get("kind") != "Call" or not any(len(line.expandtabs(8)) > 72 for line in doc.code(node).splitlines()):
            continue
        children = node["children"]
        callee = doc.code(children[0])
        if callee not in ("vmmfs_vnode_create_regular", "vmmfs_vnode_create_cdev", "vmmfs_root_allocate_inode"):
            continue
        start, _ = doc.span(node)
        prefix = doc.text[doc.text.rfind("\n", 0, start) + 1:start]
        indent = prefix[:len(prefix) - len(prefix.lstrip())] + "    "
        lines = [callee + "("]
        for index, argument in enumerate(children[1:]):
            value = compact(doc.code(argument)) + ("," if index < len(children) - 2 else ")")
            head = prefix if len(lines) == 1 else ""
            space = "" if lines[-1].endswith("(") else " "
            if len((head + lines[-1] + space + value).expandtabs(8)) > 78:
                lines.append(indent + value)
            else:
                lines[-1] += space + value
        doc.edit(*doc.span(node), "\n".join(lines), "wrap call after alias substitution")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("plan", "apply"))
    parser.add_argument("stage", choices=("constructors", "internal", "finish", "formatting"))
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    if args.mode == "apply":
        plan = json.loads(args.manifest.read_text())
        require(plan["head"] == head and plan["stage"] == args.stage, "wrong plan")
        for path, sha in plan["inputs"].items():
            require(digest(Path(path).read_bytes()) == sha, "changed input: " + path)
        updates = []
        for path, edits in plan["edits"].items():
            doc = Document(Path(path), Path(path).read_text(), {})
            doc.edits = edits
            require(all(doc.text[e["start"]:e["end"]] == e["old"] for e in edits), "stale edits")
            updates.append((doc.path, doc.result()))
        for path, text in updates:
            path.write_text(text)
        print("Applied", args.stage, len(updates), "files")
        return
    module = repo / "sys/vfs/vmmfs"
    paths = sorted(module.glob("vmmfs*.c"))
    if args.stage == "finish":
        paths.append(module / "vmmfs_parent.h")
    inputs = {str(p): digest(p.read_bytes()) for p in paths + list(module.glob("*.h"))
              + [Path(__file__).resolve(), module / "compile_commands.json"]}
    client = Client(module)
    documents, functions = [], []
    try:
        for path in paths:
            text = path.read_text()
            doc = Document(path, text, client.tree(path, text))
            for function in doc.tree.get("children", []):
                if function.get("kind") != "Function" or not any(
                        n.get("kind") == "Compound" for n in function.get("children", [])):
                    continue
                before = len(doc.edits)
                if args.stage == "constructors" and function.get("detail") in TARGETS:
                    constructor(doc, function)
                elif args.stage == "internal":
                    internal(doc, function)
                elif args.stage == "finish":
                    finish(doc, function)
                elif args.stage == "formatting":
                    formatting(doc, function)
                if function.get("detail") in TARGETS:
                    unused_locals(doc, function)
                if len(doc.edits) != before:
                    functions.append(function["detail"])
            doc.result()
            documents.append(doc)
    finally:
        client.close()
    plan = {"head": head, "stage": args.stage, "inputs": inputs, "functions": functions,
            "edits": {str(d.path): d.edits for d in documents if d.edits}}
    args.manifest.write_text(json.dumps(plan, indent=2) + "\n")
    args.manifest.with_suffix(".diff").write_text("".join("".join(difflib.unified_diff(
        d.text.splitlines(True), d.result().splitlines(True), fromfile=str(d.path), tofile=str(d.path)))
        for d in documents if d.edits))
    print(json.dumps({"functions": functions, "files": len(plan["edits"]),
                      "edits": sum(len(d.edits) for d in documents)}, indent=2))


if __name__ == "__main__":
    main()
