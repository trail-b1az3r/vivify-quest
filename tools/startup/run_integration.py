#!/usr/bin/env python3
"""Compile the generated SDK relocator core, never its hook installer."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
sdk = repo / "extern/includes/beatsaber-hook/shared/inline-hook"
source = sdk / "And64InlineHook.cpp"
if len(sys.argv) > 1:
    source = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="vivify-relocator-") as temporary:
    directory = Path(temporary)
    output = directory / "And64InlineHook.cpp"
    script = directory / "generate.cmake"
    script.write_text('include("' + str(repo / "cmake/FixArm64Relocator.cmake") + '")\n'
                      'vivify_fix_arm64_relocator("' + str(source) + '" "' + str(output) + '")\n')
    subprocess.run(["cmake", "-P", str(script)], check=True)
    generated = output.read_text()
    (directory / "And64HookInstaller.inc").write_text(generated)
    boundary = 'extern "C" {'
    assert generated.count(boundary) == 1
    # Keep all actual fix-up functions, exclude only hook installation wrappers.
    (directory / "And64RelocatorCore.inc").write_text(generated.split(boundary)[0] + '\n#endif\n')
    executable = directory / "test"
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-g",
                    "-D__aarch64__", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", str(repo / "include"), "-I", str(sdk), "-I", str(directory),
                    "-I", str(repo / "tools/startup/shim"),
                    str(repo / "tools/startup/relocator_integration_test.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++20", "-O1", "-g",
                    "-D__aarch64__", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-I", str(repo / "include"), "-I", str(sdk), "-I", str(directory),
                    "-I", str(repo / "tools/startup/shim"),
                    str(repo / "tools/startup/hook_install_test.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    # A changed SDK must not be silently fed through the version-specific fix.
    modified = directory / "changed.cpp"
    modified.write_text(source.read_text() + "\n// changed SDK\n")
    script.write_text(script.read_text().replace(str(source), str(modified)))
    result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
    assert result.returncode != 0 and "Unreviewed And64InlineHook.cpp" in result.stderr
    print("Changed-SDK refusal passed")
