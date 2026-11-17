#!/usr/bin/env python3
"""Convert donor modes and optionally the native route, then link GLSL pipelines.

This validates static shader/serialization behavior. It does not run Unity or
establish visual parity. Source bundles remain caller-owned and aren't fixtures.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bundleconvert'))
from mkbundle import read_converted


def stages(code):
    result, stage, nesting, lines = {}, None, 0, []
    for line in code.splitlines():
        if stage is None:
            if line in ('#ifdef VERTEX', '#ifdef FRAGMENT', '#ifdef GEOMETRY'):
                stage, nesting, lines = line.split()[1], 1, []
            continue
        if line.lstrip().startswith(('#if ', '#ifdef ', '#ifndef ')):
            nesting += 1
        if line.lstrip().startswith('#endif'):
            nesting -= 1
            if nesting == 0:
                result[stage] = '\n'.join(lines) + '\n'
                stage = None
                continue
        lines.append(line)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('converter', type=Path)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    native = parser.add_mutually_exclusive_group()
    native.add_argument('--include-native', action='store_true')
    native.add_argument('--native-only', action='store_true')
    args = parser.parse_args()
    validator = shutil.which('glslangValidator')
    if not validator:
        parser.error('glslangValidator is required')
    converter = str(args.converter.resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    modes = {'eye-left': '__VivifyEye0__/', 'eye-right': '__VivifyEye1__/',
             'mono': '__VivifyMono__/', 'blit-left': '__VivifyBlitEye0__/', 'blit-right': '__VivifyBlitEye1__/', 'native-perspective': '__VivifyNative'}
    if args.native_only:
        modes = {'native': None}
    elif args.include_native:
        modes = {'native': None, **modes}
    results, cabinets, names = {}, [], set()
    for mode, prefix in modes.items():
        bundle = args.output / (mode + '.vivify')
        flag = '--shaders' if mode == 'native' else '--shaders-' + mode + '-isolated'
        proc = subprocess.run([converter, flag, str(args.source), str(bundle)],
                              env=dict(os.environ, VIVIFY_SKIP_UNUSED='1'), capture_output=True, text=True)
        (args.output / (mode + '-conversion.log')).write_text(proc.stdout + proc.stderr)
        proc.check_returncode()
        exposed = re.search(r'^renderDependencyEntries=(\d+)$', proc.stdout, re.M)
        exposed_count = int(exposed.group(1)) if exposed else 0
        if not re.search(r'variantsRefused=0\b', proc.stdout) or not re.search(r'\brefused=0\b', proc.stdout):
            raise RuntimeError(mode + ': conversion was incomplete')
        _, _, _, nodes, data = read_converted(bundle)
        cabinets.append({n[3] for n in nodes if 'CAB-' in n[3]})
        unique, shaders, grabs = {}, set(), set()
        for index, (off, size, _, name) in enumerate(nodes):
            if name.endswith('.resS'):
                continue
            sf = args.output / (mode + '-' + str(index) + '.sf')
            sf.write_bytes(data[off:off + size])
            inspected = subprocess.run([converter, '--inspect', str(sf)], capture_output=True, text=True, check=True).stdout
            for line in inspected.splitlines():
                if line.startswith('shader='):
                    shaders.add(line[7:].split(' platforms=')[0])
                if line.startswith('grabTexture='):
                    grabs.add(line[len('grabTexture='):])
                if not line.startswith('entry '):
                    continue
                _, sep, code = line.partition(' code=')
                code = code.replace('\\n', '\n')
                if sep and code.startswith('#ifdef VERTEX'):
                    unique[hashlib.sha256(code.encode()).hexdigest()] = code
        if not shaders or (prefix is not None and any(not s.startswith(prefix) for s in shaders)) or names.intersection(shaders):
            raise RuntimeError(mode + ': shader names were not isolated')
        names.update(shaders)
        errors, geometry, layer_geometry, depth_pipelines = [], 0, 0, 0
        for key, code in unique.items():
            if mode not in ('native', 'native-perspective') and any(s in code for s in ('gl_ViewID_OVR', 'num_views', 'gl_Layer')):
                errors.append({'pipeline': key, 'error': 'unexpected layered rendering'})
            for texture in (() if mode in ('native', 'native-perspective') else ('_CameraDepthTexture', '_CameraDepthNormalsTexture', '_GrabTexture', *grabs)):
                if re.search(r'sampler2D(?:MS)?Array\s+' + re.escape(texture) + r'\s*;', code):
                    errors.append({'pipeline': key, 'error': texture + ' still expects an array'})
            if mode in ('eye-left', 'eye-right', 'blit-left', 'blit-right') and ('uniform UnityStereoGlobals' in code or 'vivify_cb_UnityStereoGlobals' in code):
                errors.append({'pipeline': key, 'error': 'explicit eyes still depend on the engine stereo block'})
            if mode.startswith('blit') and re.search(r'sampler2DArray\s+_MainTex\s*;', code):
                errors.append({'pipeline': key, 'error': 'blit input still expects an array'})
            if 'sampler2D _CameraDepthTexture;' in code:
                depth_pipelines += 1
            sections, files = stages(code), []
            versions = [re.findall(r'^#version (\d+) es$', section, re.M) for section in sections.values()]
            if not versions or any(len(v) != 1 for v in versions) or len({v[0] for v in versions if len(v) == 1}) != 1:
                errors.append({'pipeline': key, 'error': 'GLES stages require a single matching language version'})
            feeder = sections.get('GEOMETRY', sections.get('VERTEX', ''))
            varying = r'^((?:(?:flat|centroid|smooth|sample) )*)%s vec4 ((?:vs|gs)_\w+);$'
            outputs = {name: qualifier for qualifier, name in re.findall(varying % 'out', feeder, re.M)}
            for qualifier, name in re.findall(varying % 'in', sections.get('FRAGMENT', ''), re.M):
                if outputs.get(name) != qualifier:
                    errors.append({'pipeline': key, 'error': name + ' interpolation mismatch'})
            geometry += 'GEOMETRY' in sections
            layer_geometry += 'gl_Layer' in sections.get('GEOMETRY', '')
            for stage, suffix in [('VERTEX', 'vert'), ('GEOMETRY', 'geom'), ('FRAGMENT', 'frag')]:
                if stage in sections:
                    path = args.output / (mode + '-' + key[:12] + '.' + suffix)
                    path.write_text(sections[stage])
                    files.append(str(path))
            proc = subprocess.run([validator, '-l', *files], capture_output=True, text=True)
            if proc.returncode:
                errors.append({'pipeline': key, 'error': (proc.stdout + proc.stderr)[-2000:]})
        results[mode] = {'shaders': len(shaders), 'unique_pipelines': len(unique), 'geometry_pipelines': geometry, 'layer_geometry_pipelines': layer_geometry,
                         'exposed_render_dependencies': exposed_count,
                         'depth_sampling_pipelines': depth_pipelines, 'grab_textures': sorted(grabs),
                         'native_perspective_shaders': sorted(s.removeprefix('__VivifyNativePerspective__/') for s in shaders if s.startswith('__VivifyNativePerspective__/')), 'errors': errors}
    for i, cabinet in enumerate(cabinets):
        if any(cabinet.intersection(other) for other in cabinets[:i]):
            raise RuntimeError('donors share internal cabinet names')
    summary = {'source_sha256': hashlib.sha256(args.source.read_bytes()).hexdigest(),
               'device_tested': False, 'modes': results}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({mode: {k: len(v) if k == 'errors' else v for k, v in result.items()}
                      for mode, result in results.items()}, indent=2))
    return any(r['errors'] for r in results.values())


if __name__ == '__main__':
    sys.exit(main())
