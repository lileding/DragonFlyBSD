#!/usr/bin/env python3
"""Print VMM C call trees using clangd AST and definition LSP requests."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import time
from typing import Any


class LspError(RuntimeError):
    pass


def is_vmm_symbol(name: str) -> bool:
    return name.startswith("vmm_")


class Clangd:
    def __init__(self, root: Path) -> None:
        command = os.environ.get("CLANGD", "clangd")
        self.process = subprocess.Popen(
            [
                command,
                f"--compile-commands-dir={root}",
                "--background-index",
                "--clang-tidy=false",
                "--log=error",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if self.process.stdin is None or self.process.stdout is None:
            raise LspError("cannot create clangd stdio pipes")
        self.stdin = self.process.stdin
        self.stdout = self.process.stdout
        self.next_id = 1
        self.open_documents: set[str] = set()

    def close(self) -> None:
        if self.process.poll() is not None:
            return
        try:
            self.request("shutdown", None)
            self.notify("exit", None)
            self.process.wait(timeout=2)
        except (LspError, subprocess.TimeoutExpired):
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()

    def initialize(self, root: Path) -> None:
        root_uri = root.as_uri()
        result = self.request(
            "initialize",
            {
                "processId": None,
                "rootUri": root_uri,
                "workspaceFolders": [{"uri": root_uri, "name": root.name}],
                "capabilities": {
                    "workspace": {
                        "symbol": {"dynamicRegistration": False},
                    },
                },
            },
        )
        if not isinstance(result, dict):
            raise LspError("clangd did not return initialize capabilities")
        self.notify("initialized", {})

    def notify(self, method: str, params: Any) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method: str, params: Any) -> Any:
        request_id = self.next_id
        self.next_id += 1
        self._send(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": method,
                "params": params,
            }
        )
        while True:
            message = self._read()
            if message.get("id") != request_id:
                continue
            if "error" in message:
                error = message["error"]
                raise LspError(f"{method}: {error.get('message', error)}")
            return message.get("result")

    def wait_until_open(self, uri: str) -> None:
        deadline = time.monotonic() + 10
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise LspError(f"clangd did not open {uri_to_display(uri)}")
            ready, _, _ = select.select([self.stdout], [], [], remaining)
            if not ready:
                continue
            message = self._read()
            if message.get("method") != "textDocument/publishDiagnostics":
                continue
            parameters = message.get("params")
            if isinstance(parameters, dict) and parameters.get("uri") == uri:
                return

    def _send(self, message: dict[str, Any]) -> None:
        body = json.dumps(message, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        self.stdin.write(header)
        self.stdin.write(body)
        self.stdin.flush()

    def _read(self) -> dict[str, Any]:
        headers: dict[bytes, bytes] = {}
        while True:
            line = self.stdout.readline()
            if not line:
                raise LspError("clangd closed its output stream")
            if line in (b"\n", b"\r\n"):
                break
            key, separator, value = line.partition(b":")
            if not separator:
                raise LspError(f"malformed clangd header: {line!r}")
            headers[key.lower()] = value.strip()
        try:
            length = int(headers[b"content-length"])
        except (KeyError, ValueError) as error:
            raise LspError("clangd response has no valid Content-Length") from error
        body = self.stdout.read(length)
        if len(body) != length:
            raise LspError("truncated clangd response")
        return json.loads(body)


def source_files(root: Path) -> list[Path]:
    database = root / "compile_commands.json"
    try:
        commands = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise LspError(f"cannot read {database}: {error}") from error
    result: set[Path] = set()
    for command in commands:
        filename = command.get("file")
        if not isinstance(filename, str):
            continue
        path = Path(filename).resolve()
        if path.suffix != ".c":
            continue
        try:
            path.relative_to(root)
        except ValueError:
            continue
        result.add(path)
    # make-generated databases in this tree may omit object sources after an
    # incremental build.  clangd can infer their command from a listed peer.
    result.update(path.resolve() for path in root.rglob("vmm*.c"))
    if not result:
        raise LspError("no VMM C sources found in this directory")
    return sorted(result)


def open_source(client: Clangd, path: Path) -> str:
    uri = path.as_uri()
    if uri in client.open_documents:
        return uri
    client.notify(
        "textDocument/didOpen",
        {
            "textDocument": {
                "uri": uri,
                "languageId": "c",
                "version": 1,
                "text": path.read_text(encoding="utf-8"),
            }
        },
    )
    client.wait_until_open(uri)
    client.open_documents.add(uri)
    return uri


def walk_symbols(symbols: list[dict[str, Any]]) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for symbol in symbols:
        result.append(symbol)
        children = symbol.get("children")
        if isinstance(children, list):
            result.extend(walk_symbols(children))
    return result


def symbol_position(symbol: dict[str, Any]) -> dict[str, int]:
    selection = symbol.get("selectionRange")
    if isinstance(selection, dict):
        position = selection.get("start")
        if isinstance(position, dict):
            return position
    location = symbol.get("location")
    if isinstance(location, dict):
        range_ = location.get("range")
        if isinstance(range_, dict):
            position = range_.get("start")
            if isinstance(position, dict):
                return position
    raise LspError(f"clangd symbol has no source location: {symbol.get('name')}")


def symbol_is_definition(symbol: dict[str, Any]) -> bool:
    location = symbol.get("location")
    if not isinstance(location, dict):
        return True
    uri = location.get("uri")
    range_ = location.get("range")
    if not isinstance(uri, str) or not isinstance(range_, dict):
        return True
    start = range_.get("start")
    end = range_.get("end")
    if not isinstance(start, dict) or not isinstance(end, dict):
        return True
    try:
        lines = Path(uri.removeprefix("file://")).read_text(
            encoding="utf-8").splitlines()
    except OSError:
        return start.get("line") != end.get("line")
    start_line = start.get("line")
    end_line = end.get("line")
    if not isinstance(start_line, int) or not isinstance(end_line, int):
        return True
    return "{" in "\n".join(lines[start_line:end_line + 1])


def find_function(client: Clangd, files: list[Path], name: str,
                  filename: Path | None) -> tuple[str, dict[str, Any]]:
    matches: list[tuple[str, dict[str, Any]]] = []
    available: set[str] = set()
    for path in files:
        if filename is not None and path != filename:
            continue
        uri = open_source(client, path)
        symbols = client.request("textDocument/documentSymbol", {
            "textDocument": {"uri": uri},
        })
        if not isinstance(symbols, list):
            continue
        for symbol in walk_symbols(symbols):
            symbol_name = symbol.get("name")
            if not isinstance(symbol_name, str):
                continue
            available.add(symbol_name)
            if (symbol_name == name or symbol_name.startswith(f"{name}(")) and \
               symbol_is_definition(symbol):
                matches.append((uri, symbol))
    if not matches:
        workspace_symbols = client.request("workspace/symbol", {"query": name})
        if isinstance(workspace_symbols, list):
            for symbol in workspace_symbols:
                symbol_name = symbol.get("name")
                location = symbol.get("location")
                if not isinstance(symbol_name, str) or not isinstance(location, dict):
                    continue
                if symbol_name != name and not symbol_name.startswith(f"{name}("):
                    continue
                uri = location.get("uri")
                selection = location.get("range")
                if not isinstance(uri, str) or not isinstance(selection, dict):
                    continue
                path = Path(uri.removeprefix("file://")).resolve()
                if filename is not None and path != filename:
                    continue
                matches.append((uri, {
                    "name": symbol_name,
                    "selectionRange": selection,
                }))
    if not matches:
        candidates = sorted(candidate for candidate in available
                            if name in candidate)[:12]
        suffix = f"; candidates: {', '.join(candidates)}" if candidates else ""
        raise LspError(f"function not found: {name}{suffix}")
    if len(matches) != 1:
        locations = ", ".join(
            f"{uri_to_display(uri)}:{symbol_position(symbol)['line'] + 1}"
            for uri, symbol in matches
        )
        raise LspError(f"function name is ambiguous: {name}: {locations}; use --file")
    return matches[0]


def all_functions(client: Clangd, files: list[Path]) -> list[dict[str, Any]]:
    """Return every function definition directly implemented by VMMFS."""
    result: dict[tuple[str, int, int, str], dict[str, Any]] = {}
    for path in files:
        uri = open_source(client, path)
        symbols = client.request("textDocument/documentSymbol", {
            "textDocument": {"uri": uri},
        })
        if not isinstance(symbols, list):
            continue
        for symbol in walk_symbols(symbols):
            name = symbol.get("name")
            if (not isinstance(name, str) or not is_vmm_symbol(name) or
                    symbol.get("kind") not in (6, 12)):
                continue
            if not symbol_is_definition(symbol):
                continue
            item = {
                "name": name.removesuffix("()"),
                "uri": uri,
                "range": symbol_range(symbol),
            }
            result[item_key(item)] = item
    return sorted(result.values(), key=item_key)


def function_is_static(item: dict[str, Any]) -> bool:
    """Read only the declaration prefix located by clangd."""
    path = Path(item["uri"].removeprefix("file://"))
    start = item["range"]["start"]["line"]
    try:
        declaration = "\n".join(path.read_text(encoding="utf-8").splitlines()[start:start + 3])
    except OSError as error:
        raise LspError(f"cannot read {path}: {error}") from error
    return declaration.lstrip().startswith("static")


def ast_function_references(tree: dict[str, Any]) -> set[str]:
    """Return C function names referenced by an AST subtree."""
    result: set[str] = set()
    pending = [tree]
    while pending:
        node = pending.pop()
        children = node.get("children")
        if isinstance(children, list):
            pending.extend(children)
        if node.get("kind") != "DeclRef":
            continue
        name = node.get("detail")
        arcana = node.get("arcana")
        if (isinstance(name, str) and isinstance(arcana, str) and
                " Function " in arcana):
            result.add(name)
    return result


def ops_functions(client: Clangd, files: list[Path]) -> set[str]:
    """Find callbacks installed in VMM operations and backend tables."""
    result: set[str] = set()
    for path in files:
        uri = open_source(client, path)
        symbols = client.request("textDocument/documentSymbol", {
            "textDocument": {"uri": uri},
        })
        if not isinstance(symbols, list):
            continue
        for symbol in walk_symbols(symbols):
            name = symbol.get("name")
            if (not isinstance(name, str) or
                    not (name.endswith("_ops") or name.endswith("_backend")) or
                    symbol.get("kind") != 13):
                continue
            tree = client.request("textDocument/ast", {
                "textDocument": {"uri": uri},
                "range": symbol_range(symbol),
            })
            if isinstance(tree, dict):
                result.update(ast_function_references(tree))
    return result


def worker_functions(client: Clangd, files: list[Path]) -> set[str]:
    """Find VMM callbacks registered as asynchronous worker entry points."""
    registrations = {
        "callout_reset",
        "callout_reset_bycpu",
        "callout_reset_sbt",
        "callout_reset_sbt_on",
        "kthread_create",
        "kthread_create1",
        "lwkt_create",
        "taskqueue_enqueue",
    }
    result: set[str] = set()
    for path in files:
        uri = open_source(client, path)
        tree = client.request("textDocument/ast", {
            "textDocument": {"uri": uri},
        })
        if not isinstance(tree, dict):
            continue
        pending = [tree]
        while pending:
            node = pending.pop()
            children = node.get("children")
            if isinstance(children, list):
                pending.extend(children)
            if node.get("kind") != "Call":
                continue
            reference = call_reference(node)
            if reference is None or reference["name"] not in registrations:
                continue
            result.update(ast_function_references(node))
    return result - registrations


def root_functions(client: Clangd, files: list[Path]) -> list[dict[str, Any]]:
    """Keep public functions plus static operations and worker callbacks."""
    callbacks = ops_functions(client, files) | worker_functions(client, files)
    return [item for item in all_functions(client, files)
            if (not item["name"].endswith("_log") and
                (not function_is_static(item) or item["name"] in callbacks))]


def uri_to_display(uri: str) -> str:
    try:
        return Path(uri.removeprefix("file://")).name
    except ValueError:
        return uri


def symbol_range(symbol: dict[str, Any]) -> dict[str, dict[str, int]]:
    location = symbol.get("location")
    if isinstance(location, dict):
        range_ = location.get("range")
        if isinstance(range_, dict):
            return range_
    selection = symbol.get("selectionRange")
    if isinstance(selection, dict):
        return selection
    position = symbol_position(symbol)
    return {"start": position, "end": position}


def ast_calls(client: Clangd, item: dict[str, Any]) -> list[dict[str, Any]]:
    path = Path(item["uri"].removeprefix("file://"))
    if not path.is_file():
        return []
    open_source(client, path)
    tree = client.request("textDocument/ast", {
        "textDocument": {"uri": item["uri"]},
        "range": item["range"],
    })
    if not isinstance(tree, dict):
        return []

    calls: list[dict[str, Any]] = []
    pending = [tree]
    while pending:
        node = pending.pop()
        children = node.get("children")
        if isinstance(children, list):
            pending.extend(reversed(children))
        if node.get("kind") != "Call":
            continue
        reference = call_reference(node)
        if reference is not None:
            calls.append(reference)
    return calls


def call_reference(node: dict[str, Any]) -> dict[str, Any] | None:
    pending = list(reversed(node.get("children", [])))
    while pending:
        child = pending.pop()
        children = child.get("children")
        if isinstance(children, list):
            pending.extend(reversed(children))
        if child.get("kind") != "DeclRef":
            continue
        arcana = child.get("arcana")
        range_ = child.get("range")
        name = child.get("detail")
        if (not isinstance(arcana, str) or " Function " not in arcana or
                not isinstance(range_, dict) or not isinstance(name, str)):
            continue
        return {"name": name, "range": range_}
    return None


def definition(client: Clangd, source: dict[str, Any], reference: dict[str, Any]) \
        -> dict[str, Any] | None:
    result = client.request("textDocument/definition", {
        "textDocument": {"uri": source["uri"]},
        "position": reference["range"]["start"],
    })
    if not isinstance(result, list) or not result:
        return None
    location = result[0]
    if "targetUri" in location:
        uri = location.get("targetUri")
        range_ = location.get("targetRange")
    else:
        uri = location.get("uri")
        range_ = location.get("range")
    if not isinstance(uri, str) or not isinstance(range_, dict):
        return None
    return {"name": reference["name"], "uri": uri, "range": range_}


def item_key(item: dict[str, Any]) -> tuple[str, int, int, str]:
    start = item["range"]["start"]
    return (item["uri"], start["line"], start["character"], item["name"])


def function_key(item: dict[str, Any]) -> tuple[str, str]:
    """Identify a function across documentSymbol and definition responses."""
    return (item["uri"], item["name"])


def path_is_internal(root: Path, path: Path) -> bool:
    try:
        path.resolve().relative_to(root)
    except ValueError:
        return False
    return True


def object_name(root: Path, item: dict[str, Any]) -> str | None:
    """Derive the VMM object namespace from its implementation file."""
    if not item["name"].startswith("vmm_"):
        return None
    path = Path(item["uri"].removeprefix("file://")).resolve()
    if not path_is_internal(root, path):
        return None
    if path.stem == "vmm":
        return "vmm"
    return path.stem.removeprefix("vmm_")


def item_display(root: Path, item: dict[str, Any]) -> str:
    start = item["range"]["start"]
    name = item["name"]
    object_ = object_name(root, item)
    if object_ is not None:
        prefix = f"vmm_{object_}_"
        if name.startswith(prefix):
            name = f"{object_}::{name.removeprefix(prefix)}"
        elif object_ == "vmm" and name.startswith("vmm_"):
            name = f"vmm::{name.removeprefix('vmm_')}"
        else:
            name = f"{object_}::{name.removeprefix('vmm_')}"
    return f"{name} ({uri_to_display(item['uri'])}:{start['line'] + 1})"


def function_calls(client: Clangd, item: dict[str, Any]) -> list[dict[str, Any]]:
    resolved: dict[tuple[str, int, int, str], dict[str, Any]] = {}
    for reference in ast_calls(client, item):
        target = definition(client, item, reference)
        if (target is not None and is_vmm_symbol(target["name"]) and
                not target["name"].endswith("_log")):
            resolved[item_key(target)] = target
    return sorted(resolved.values(), key=item_key)


def print_tree(client: Clangd, root: Path, item: dict[str, Any], depth: int | None,
               include_external: bool,
               roots: set[tuple[str, str]] | None = None) -> None:
    active: set[tuple[str, int, int, str]] = set()

    def walk(current: dict[str, Any], level: int, prefix: str,
             last: bool) -> None:
        key = item_key(current)
        if level == 0:
            print(item_display(root, current))
        else:
            print(f"{prefix}{'`-- ' if last else '|-- '}{item_display(root, current)}")
        if level != 0 and roots is not None and function_key(current) in roots:
            return
        if depth is not None and level >= depth:
            return
        active.add(key)
        children = function_calls(client, current)
        child_prefix = prefix if level == 0 else prefix + ("    " if last else "|   ")
        for index, child in enumerate(children):
            child_last = index == len(children) - 1
            branch = "`-- " if child_last else "|-- "
            child_path = Path(child["uri"].removeprefix("file://")).resolve()
            if not path_is_internal(root, child_path) and not include_external:
                print(f"{child_prefix}{branch}{item_display(root, child)} [external]")
                continue
            if item_key(child) in active:
                print(f"{child_prefix}{branch}{item_display(root, child)}")
                print(f"{child_prefix}{'    ' if child_last else '|   '}`-- ... recursion")
                continue
            walk(child, level + 1, child_prefix, child_last)
        active.remove(key)

    walk(item, 0, "", True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Print a clangd-derived C call tree for this VMM directory."
    )
    parser.add_argument("function", nargs="?",
                        help="exact C function definition name; omit to print all")
    parser.add_argument("--depth", type=int,
                        help="maximum edges to print (default: unlimited)")
    parser.add_argument("--file", type=Path,
                        help="source file for an otherwise ambiguous name")
    parser.add_argument("--all", action="store_true",
                        help="recurse into functions outside this directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.depth is not None and args.depth < 0:
        print("depth must be non-negative", file=sys.stderr)
        return 2
    if args.function is not None and not is_vmm_symbol(args.function):
        print("function must start with vmm_", file=sys.stderr)
        return 2
    if args.function is not None and args.function.endswith("_log"):
        print("logging functions are filtered from this call graph", file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parent
    selected_file = args.file.resolve() if args.file is not None else None
    client = Clangd(root)
    try:
        client.initialize(root)
        files = source_files(root)
        if args.function is not None:
            uri, symbol = find_function(client, files, args.function, selected_file)
            items = [{
                "name": args.function,
                "uri": uri,
                "range": symbol_range(symbol),
            }]
        else:
            if selected_file is not None:
                files = [selected_file]
            items = root_functions(client, files)
        if args.function is not None:
            print_tree(client, root, items[0], args.depth, args.all)
        else:
            roots = {function_key(item) for item in items}
            objects: dict[str, list[dict[str, Any]]] = {}
            for item in items:
                object_ = object_name(root, item)
                if object_ is None:
                    continue
                objects.setdefault(object_, []).append(item)
            for group_index, (object_, group) in enumerate(sorted(objects.items())):
                if group_index != 0:
                    print()
                print(f"== {object_} ==")
                for index, item in enumerate(group):
                    if index != 0:
                        print()
                    print_tree(client, root, item, args.depth, args.all, roots)
    except (LspError, OSError) as error:
        print(f"lsp_callchain: {error}", file=sys.stderr)
        return 1
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
