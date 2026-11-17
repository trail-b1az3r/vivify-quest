#!/usr/bin/env python3
"""Run actual capture allocation/cleanup methods with state/lifetime API stand-ins.

No Unity execution, camera render, device connection or graphics emulation.
The optional baseline must reproduce the old cleanup failures.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--baseline')
parser.add_argument('--baseline-kind', choices=('teardown', 'resize'), default='teardown')
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
    components = source('src/VivifyComponents.cpp')
    processing = source('src/VivifyPostProcessing.cpp')
    release = 'void SecondaryCameraController::ReleaseGlobalBindings()'
    # This new method is absent and unused in the pre-fix cleanup paths. The
    # empty declaration satisfies the common stand-in type without altering
    # either old method's behavior.
    bindings = method(components, release) if release in components else release + ' {}\n'
    (directory / 'ActualReleaseBindings.inc').write_text(bindings)
    (directory / 'ActualEnsureEyeTexture.inc').write_text(method(components, 'UnityEngine::RenderTexture* SecondaryCameraController::EnsureEyeTexture('))
    (directory / 'ActualControllerDestroy.inc').write_text(method(components, 'void SecondaryCameraController::OnDestroy()'))
    (directory / 'ActualReleaseDeclared.inc').write_text(method(processing, 'void Runtime::ReleaseDeclaredTextureData('))
    (directory / 'ActualReleaseCamera.inc').write_text(method(processing, 'void Runtime::ReleaseSecondaryCameraData('))
    # The types in the fixture are declared before the real shared header is
    # included. Resolve just its two SDK include names through these shims.
    sdk = directory / 'UnityEngine'
    sdk.mkdir(exist_ok=True)
    for name in ('Shader', 'Texture'):
        (sdk / (name + '.hpp')).write_text('#pragma once\n')
    executable = directory / 'test'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-O1', '-g',
                    '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I', str(directory), '-I', str(repo / 'include'),
                    str(repo / 'tools/separate-eye/texture_ownership_test.cpp'), '-o', str(executable)], check=True)
    return subprocess.run([str(executable)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='vivify-texture-ownership-') as temporary:
    directory = Path(temporary)
    fixed = run(directory)
    print(fixed.stdout, end='')
    print(fixed.stderr, end='')
    fixed.check_returncode()
    if args.baseline:
        baseline = run(directory, args.baseline)
        assert baseline.returncode == 1, baseline.stdout + baseline.stderr
        labels = ('camera resize unbinds its old image before release',
                  'failed camera allocation leaves no released global') if args.baseline_kind == 'resize' else (
                  'declared cleanup preserves another owner', 'external camera destruction clears owned color and depth',
                  'runtime camera cleanup preserves replacement color and depth')
        for label in labels:
            assert label in baseline.stderr, baseline.stderr
        assert 'Sanitizer' not in baseline.stderr
        print('Pre-fix production methods reproduce the ownership failures:')
        print(baseline.stdout, end='')
        print(baseline.stderr, end='')
