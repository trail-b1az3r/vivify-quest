#!/usr/bin/env python3
"""Generate a real translated VS/GS/FS pipeline for the device smoke test.

The geometry stage must emit a triangle, read the logical eye, and write its
layer output without selecting a nonexistent layer of a non-layered FBO.
The center pixel must be black for eye 0 and white for eye 1 (blue background).
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dxbc'))
import mkdxbc as m


def fixtures():
    def sig(entries, kind):
        return m.signature_chunk(entries, kind)

    position = {'name': 'POSITION', 'register': 0}
    svpos = {'name': 'SV_Position', 'register': 0, 'sv': 1}
    layer = {'name': 'SV_RenderTargetArrayIndex', 'register': 1, 'sv': 4,
             'component_type': 1, 'mask': 1, 'rw_mask': 1}
    color = {'name': 'COLOR', 'register': 2}
    vertex = m.container([
        sig([position], b'ISGN'), sig([svpos], b'OSGN'),
        m.shex_chunk([m.insn(54, m.dest(2, 0), m.src(1, 0)) + m.insn(62)], stage=1)])
    code = m.insn(93, controls=3)  # triangle input
    code += m.insn(94, extra=[3])  # max output vertex count
    code += m.insn(92, controls=5)  # triangle strip output
    for vertex_index in range(3):
        code += m.insn(54, m.dest(2, 0), m._operand(1, [vertex_index, 0]))
        code += m.insn(43, m.dest(2, 2), m._operand(1, [0, 1], swizzle=(0, 0, 0, 0)))
        code += m.insn(54, m.dest(2, 2, 8), m.imm_float(1.0))
        code += m.insn(54, m.dest(2, 1, 1), m._operand(1, [0, 1], swizzle=(0, 0, 0, 0)))
        code += m.insn(19)  # EmitVertex
    code += m.insn(9) + m.insn(62)  # EndPrimitive, return
    geometry = m.container([sig([svpos, layer], b'ISGN'), sig([svpos, layer, color], b'OSGN'),
                            m.shex_chunk([code], stage=2)])
    fragment = m.container([
        sig([color], b'ISGN'), sig([{'name': 'SV_Target', 'register': 0, 'sv': 64}], b'OSGN'),
        m.shex_chunk([m.insn(54, m.dest(2, 0), m.src(1, 2)) + m.insn(62)], stage=0)])
    # A separate post-effect fragment reads two resources compiled as arrays
    # on Windows. The per-eye blit mode must lower both to ordinary textures
    # and preserve the logical eye in blue, like a stereo depth screen effect.
    code = m.insn(104, extra=[2])
    code += m.insn(54, m.dest(0, 0), m.imm_float(.5, .5, 0., 0.))
    code += m.insn(86, m.dest(0, 0, 4), m.src(1, 1, (0, 0, 0, 0)))
    code += m.insn(69, m.dest(0, 1), m.src(0, 0), m.src(7, 0), m.src(6, 0))
    # Through the Screen decodes depth from RG; GLES depth must supply zero G.
    code += m.insn(15, m.dest(2, 0, 1), m.src(0, 1), m.imm_float(1., 1. / 255., 0., 0.))
    code += m.insn(69, m.dest(0, 1), m.src(0, 0), m.src(7, 1), m.src(6, 1))
    code += m.insn(54, m.dest(2, 0, 2), m.src(0, 1, (0, 0, 0, 0)))
    code += m.insn(86, m.dest(2, 0, 4), m.src(1, 1, (0, 0, 0, 0)))
    code += m.insn(54, m.dest(2, 0, 8), m.imm_float(1.)) + m.insn(62)
    depth = m.container([
        m.rdef_chunk(bindings=[{'name': '_CameraDepthTexture', 'type': 2, 'dimension': 5, 'bind_point': 0},
                              {'name': '_MainTex', 'type': 2, 'dimension': 5, 'bind_point': 1}]),
        sig([layer], b'ISGN'), sig([{'name': 'SV_Target', 'register': 0, 'sv': 64}], b'OSGN'),
        m.shex_chunk([code], stage=0)])
    return {'vert': vertex, 'geom': geometry, 'frag': fragment, 'depth.frag': depth}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('translator', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    validator = shutil.which('glslangValidator')
    native = subprocess.run([str(args.translator.resolve()), 'native-empty-program'], capture_output=True, text=True, check=True).stdout
    native_paths = []
    for name, suffix in (('VERTEX', 'vert'), ('FRAGMENT', 'frag')):
        text = native.split('#ifdef ' + name + '\n', 1)[1].split('#endif\n', 1)[0]
        dest = args.output / ('native-perspective.' + suffix)
        dest.write_text(text); native_paths.append(str(dest))
    if validator:
        subprocess.run([validator, '-l', *native_paths], check=True)
    for eye in ('left', 'right'):
        paths = []
        translated = {}
        for stage, bytecode in fixtures().items():
            source = args.output / ('probe.' + stage + '.dxbc')
            source.write_bytes(bytecode)
            proc = subprocess.run([str(args.translator.resolve()), 'glsl-eye-' + eye + ('-blit' if stage == 'depth.frag' else ''), str(source)],
                                  capture_output=True, text=True, check=True)
            metadata, separator, glsl = proc.stdout.partition('---\n')
            if not separator or 'ok=1\n' not in metadata:
                raise RuntimeError(proc.stdout)
            assert all(s not in glsl for s in ('gl_ViewID_OVR', 'gl_Layer', 'num_views'))
            translated[stage] = glsl
        # Standalone stage translation cannot know the other stages' required
        # versions. Match the bundle converter's LinkStages promotion; GLES
        # drivers reject mixed versions even when glslang accepts the link.
        version = max(int(re.match(r'#version (\d+) es\n', code)[1]) for code in translated.values())
        for stage, glsl in translated.items():
            glsl = re.sub(r'^#version \d+ es', f'#version {version} es', glsl, count=1)
            dest = args.output / (eye + '.' + stage)
            dest.write_text(glsl)
            if stage != 'depth.frag': paths.append(str(dest))
        if validator:
            subprocess.run([validator, '-l', *paths], check=True)
            subprocess.run([validator, '-l', *paths[:2], str(args.output / (eye + '.depth.frag'))], check=True)
    print('Generated left/right geometry and depth/blit pipelines' + ('; GLSL link checks passed.' if validator
                                                     else '; GLSL validator unavailable.'))


if __name__ == '__main__':
    main()
