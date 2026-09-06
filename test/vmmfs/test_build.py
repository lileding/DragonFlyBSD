#!/usr/bin/env python3
"""All private layouts must participate in ordinary kmod dependencies."""
from pathlib import Path
import subprocess
import shlex
import re
import tempfile
import unittest

MODULE = Path(__file__).resolve().parents[2] / "sys/vfs/vmmfs"

class BuildDependencies(unittest.TestCase):

    def test_clean_build_has_no_installed_header_fallback(self):
        for prepared in (False, True):
            for staged in (False, True):
                with self.subTest(prepared=prepared, staged=staged):
                    with tempfile.TemporaryDirectory(prefix="vmmfs-includes-") as directory:
                        build = Path(directory)
                        if prepared:
                            (build / "dragonfly").symlink_to(MODULE.parents[1])
                        destination = str(build / "install") if staged else ""
                        result = subprocess.run(
                            ["make", "-f", str(MODULE / "Makefile"),
                             "SYSDIR=" + str(MODULE.parents[1]),
                             "DESTDIR=" + destination, "-V", "${CFLAGS}"],
                            cwd=build, capture_output=True, text=True, check=True)
                        flags = shlex.split(result.stdout)
                        self.assertIn("-nostdinc", flags)
                        self.assertIn("-Idragonfly", flags)
                        self.assertIn("-Idragonfly/../include", flags)
                        self.assertNotIn("-I" + destination + "/usr/include", flags,
                                         "clean builds must not fall back to installed headers")

    def test_private_headers_are_self_contained(self):
        result = subprocess.run(
            ["make", "-V", "${CC}", "-V", "${CFLAGS}"], cwd=MODULE,
            capture_output=True, text=True, check=True)
        compiler, flags = result.stdout.splitlines()
        command = shlex.split(compiler) + shlex.split(flags)
        for header in sorted(MODULE.glob("vmmfs*.h")):
            with self.subTest(header=header.name):
                probe = '#include <sys/param.h>\n#include "' + header.name + '"\n'
                compiled = subprocess.run(
                    command + ["-x", "c", "-fsyntax-only", "-"],
                    input=probe, cwd=MODULE, capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)

    def test_private_headers_are_kmod_inputs(self):
        result = subprocess.run(["make", "-V", "SRCS"], cwd=MODULE,
                                capture_output=True, text=True, check=True)
        inputs = {Path(name).name for name in result.stdout.split()}
        headers = {path.name for path in MODULE.glob("*.h")}
        self.assertTrue(headers)
        self.assertEqual(headers - inputs, set(),
                         "kmod objects can retain obsolete struct layouts")


    def test_private_header_change_rebuilds_every_object(self):
        inputs = subprocess.run(["make", "-V", "SRCS"], cwd=MODULE,
                                capture_output=True, text=True, check=True)
        sources = {Path(name).name for name in inputs.stdout.split()
                   if name.endswith(".c")}
        with tempfile.TemporaryDirectory(prefix="vmmfs-depend-") as directory:
            makefile = Path(directory) / "Makefile"
            for header in sorted(MODULE.glob("vmmfs*.h")):
                with self.subTest(header=header.name):
                    makefile.write_text(
                        f'.include "{MODULE / "Makefile"}"\n'
                        f'.PHONY: {header.name}\n{header.name}:\n\t@true\n')
                    result = subprocess.run(
                        ["make", "-n", "-f", str(makefile), "all"], cwd=MODULE,
                        capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    rebuilt = set(re.findall(r"-c\s+(vmmfs\w*\.c)",
                                             result.stdout))
                    self.assertEqual(sources - rebuilt, set(),
                                     f"{header.name} leaves stale objects")

    def test_built_objects_use_current_private_layouts(self):
        result = subprocess.run(["make", "-V", "SRCS"], cwd=MODULE,
                                capture_output=True, text=True, check=True)
        sources = [MODULE / name for name in result.stdout.split()
                   if name.endswith(".c")]
        if not (MODULE / "vmmfs.ko").exists():
            self.skipTest("module has not been built")
        newest_header = max(path.stat().st_mtime_ns for path in MODULE.glob("*.h"))
        for source in sources:
            object_file = source.with_suffix(".o")
            self.assertTrue(object_file.exists(), str(object_file))
            self.assertGreaterEqual(object_file.stat().st_mtime_ns, newest_header,
                                    str(object_file) + " predates a private header")

if __name__ == "__main__":

    unittest.main(verbosity=2)
