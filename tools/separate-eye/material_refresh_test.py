#!/usr/bin/env python3
"""Exercise actual renderer material methods with documented API stand-ins.

No Unity player, headset connection or generated code is executed. The optional
baseline verifies that these behavior checks fail against the pre-fix source.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--baseline', help='git revision expected to fail the new regression checks')
parser.add_argument('--baseline-kind', choices=('material', 'native', 'native-cache'), default='material')
args = parser.parse_args()


def method(text, marker):
    assert text.count(marker) == 1, marker
    start = text.index(marker)
    end = text.index('\n}\n', start) + 3
    result = text[start:end]
    assert result.count('{') == result.count('}'), marker
    return result


def run(directory, revision=None):
    def source(path):
        if revision:
            return subprocess.check_output(['git', 'show', f'{revision}:{path}'], cwd=repo, text=True)
        return (repo / path).read_text()
    (directory / 'ActualCopyMaterial.inc').write_text(method(source('src/VivifySeparateEye.cpp'),
        'UnityEngine::Material* CopyMaterial('))
    (directory / 'ActualBlitMaterialFor.inc').write_text(method(source('src/VivifyPostProcessing.cpp'),
        'UnityEngine::Material* Runtime::BlitMaterialFor('))
    (directory / 'ActualCanUseNativeMaterial.inc').write_text(method(source('src/VivifySeparateEye.cpp'),
        'bool CanUseNativeMaterial('))
    (directory / 'ActualNativePolicy.hpp').write_text(source('include/VivifySeparateEyePolicy.hpp'))
    executable = directory / 'test'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I', str(directory),
                    str(repo / 'tools/separate-eye/material_refresh_test.cpp'), '-o', str(executable)], check=True)
    return subprocess.run([str(executable)], text=True, capture_output=True)


with tempfile.TemporaryDirectory(prefix='vivify-material-refresh-') as temporary:
    directory = Path(temporary)
    fixed = run(directory)
    print(fixed.stdout, end='')
    print(fixed.stderr, end='')
    fixed.check_returncode()
    if args.baseline:
        baseline = run(directory, args.baseline)
        assert baseline.returncode == 1, baseline.stdout + baseline.stderr
        labels = ('same-cycle shader change invalidates native permission',
                  'same-cycle removed shader invalidates native permission') if args.baseline_kind == 'native-cache' else (
                  'supported upstream geometry uses native scene path',
                  'declined or disabled tessellation retains upstream native fallback') if args.baseline_kind == 'native' else (
                  'companion follows live source shader', 'same-cycle blit refreshes all material values',
                  'changed donor invalidates scene snapshot')
        for label in labels:
            assert f'FAIL {label}\n' in baseline.stderr, baseline.stderr
        assert 'Sanitizer' not in baseline.stderr
        print('Pre-fix baseline reproduced the selected material/native-route regression:')
        print(baseline.stdout, end='')
        print(baseline.stderr, end='')
