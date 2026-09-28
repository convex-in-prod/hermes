# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import pathlib
import re
import subprocess
import sys

compiler, output, cc, config, vm_lib, console_lib = sys.argv[1:]
output = pathlib.Path(output).resolve()
output.mkdir(parents=True, exist_ok=True)
root = pathlib.Path(__file__).resolve().parents[3]
fixture = root / "test/shermes/scoped-normalization-reuse.js"
scan_fixture = root / "test/shermes/scoped-string-scan-reuse.js"
includes = ["-I" + str(root / directory) for directory in
            ["include", "API", "API/jsi", "public"]] + ["-I" + config]


def run(arguments):
    subprocess.run([str(argument) for argument in arguments], check=True, timeout=120)


objects = []
for source, unit in [(scan_fixture, "scoped_scan"), (fixture, "scoped_normalization")]:
    run([compiler, "-O", "-verify-ir", "-fscoped-normalization-reuse",
         "-fstatic-object-prototype", "-emit-c", "-exported-unit=" + unit,
         "-o", output / (unit + ".c"), source])
    obj = output / (unit + ".o")
    run([cc, "-O2", "-DNDEBUG", "-fno-strict-aliasing", "-fno-strict-overflow",
         *includes, "-c", output / (unit + ".c"), "-o", obj])
    objects.append(obj)
run([cc, "-std=c++17", "-O2", "-DNDEBUG", "-DSTATIC_HERMES", *includes,
     root / "test/shermes/Inputs/scoped-normalization-main.cpp",
     *objects, "-L" + vm_lib, "-L" + console_lib,
     "-Wl,-rpath," + vm_lib, "-Wl,-rpath," + console_lib,
     "-lshermes_console", "-lhermesvm", "-lstdc++", "-lm", "-o", output / "fixture"])
result = subprocess.run([output / "fixture"], check=False, timeout=120,
                        capture_output=True, text=True)
assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
expected = re.findall(r"^// CHECK: (.*)$",
                      scan_fixture.read_text() + fixture.read_text(), re.MULTILINE)
assert result.stdout.splitlines() == expected, result.stdout
assert not result.stderr, result.stderr
