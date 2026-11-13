"""Offline prototype tests; no Unity bundle rewriting or device control."""
import os, random, shutil, struct, subprocess, sys, tempfile
from pathlib import Path
import mkdxbc as m

binary = str(Path(sys.argv[1]).resolve())
validator = shutil.which('glslangValidator')
assert validator, 'GLSL reference compiler required for the tessellation prototype'
checks = 0
def check(ok, why):
    global checks
    assert ok, why
    checks += 1

def fixture(offset=32, partition=1, points=3, extra_domain=(), reflection=True, swapped=False, untyped_fork=False, stereo=False, inside_offset=None):
    sig = [dict(name='SV_Position', register=0, sv=1), dict(name='NORMAL', register=1, mask=7)]
    if stereo: sig.append(dict(name='SV_InstanceID', register=2, mask=1, component_type=1))
    patch = [dict(name='SV_TessFactor', register=i, index=i, sv=13, mask=1) for i in range(3)]
    patch += [dict(name='SV_InsideTessFactor', register=3, sv=14, mask=1)]
    cb = m.src_cb(0, offset//16, (offset//4%4,)*4)
    inside_offset = offset if inside_offset is None else inside_offset
    inside_cb = m.src_cb(0, inside_offset//16, (inside_offset//4%4,)*4)
    dynamic = m.dest(m.OPERAND_OUTPUT, 0, 1)
    dynamic[0] |= 2 << 22  # relative index, without an immediate word
    dynamic = [dynamic[0]] + m.src(m.OPERAND_TEMP, 0, (0,)*4)
    hs = [m.insn(113), m.insn(147, controls=points), m.insn(148, controls=points), m.insn(149, controls=2),
          m.insn(150, controls=partition), m.insn(151, controls=3), m.insn(106),
          m.insn(89, m.dest_cb(0, 3)), m.insn(115), m.insn(153, extra=[3]),
          m.insn(95, [23 << 12] if untyped_fork else m.dest(23, None, 1))]
    hs += [m.insn(103, m.dest(2, i, 1), extra=[17+i]) for i in range(3)]
    hs += [m.insn(104, extra=[1]), m.insn(91, m.dest(2, 0, 1), extra=[3]),
           m.insn(54, m.dest(0, 0, 1), m.src(23, None, (0,)*4)), m.insn(54, dynamic, cb), m.insn(62),
           m.insn(115), m.insn(103, m.dest(2, 3, 1), extra=[20]), m.insn(54, m.dest(2, 3, 1), inside_cb), m.insn(62)]
    domain_point = lambda c: m.src(28, None, (c,)*4)
    cp = lambda vertex, reg: m._operand(25, [vertex, reg])
    ds = [m.insn(147, controls=points), m.insn(149, controls=2), m.insn(106), m.insn(95, m.dest(28, None, 7))]
    ds += [m.insn(95, m._operand(25, [3, s['register']], mask=s.get('mask',15))) for s in sig]
    ds += [m.insn(103 if s.get('sv') else 101, m.dest(2, s['register'], s.get('mask',15)),
                   extra=[s['sv']] if s.get('sv') else []) for s in sig]
    ds += [m.insn(104, extra=[1])]
    for s in sig:
        reg, mask = s['register'], s.get('mask',15)
        if s.get('component_type')==1:
            ds += [m.insn(54, m.dest(2,reg,mask), cp(0,reg))]
            continue
        ds += [m.insn(56, m.dest(0, 0, mask), domain_point(1), cp(1, reg)),
               m.insn(50, m.dest(0, 0, mask), cp(0, reg), domain_point(0), m.src(0, 0)),
               m.insn(50, m.dest(2, reg, mask), cp(2, reg), domain_point(2), m.src(0, 0))]
    if swapped:
        ds += [m.insn(54, m.dest(0, 0, 3), m.src(0, 0, (1,0,2,3))),
               m.insn(54, m.dest(2, 1, 3), m.src(0, 0))]
    ds += list(extra_domain) + [m.insn(62)]
    chunks = [m.signature_chunk(sig), m.signature_chunk(sig,b'OSGN'), m.signature_chunk(patch,b'PCSG')]
    rdef = [m.rdef_chunk([dict(name='$Globals', size=48, variables=[dict(name='_TessellationUniform', offset=offset, size=4,
                          rows=1, columns=1, **{'class':0,'type':3})])],
                        [dict(name='$Globals', type=0, bind_point=0)])] if reflection else []
    return m.container(chunks + rdef + [m.shex_chunk(hs,stage=3)]), m.container(chunks + [m.shex_chunk(ds,stage=4)])

with tempfile.TemporaryDirectory(prefix='vivify-tess-') as directory:
    directory = Path(directory)
    def run(h, d, args=()):
        (directory/'h.dxbc').write_bytes(h); (directory/'d.dxbc').write_bytes(d)
        prefix = directory/'pair'
        for ext in ['tesc','tese']: (directory/f'pair.{ext}').unlink(missing_ok=True)
        p = subprocess.run([binary, 'tess-pair', str(directory/'h.dxbc'), str(directory/'d.dxbc'), str(prefix), *args], capture_output=True, text=True)
        check(p.returncode == 0, p.stderr)
        fields = dict(line.split('=',1) for line in p.stdout.splitlines() if '=' in line)
        if fields.get('ok') == '1':
            for ext in ['tesc','tese']:
                proc = subprocess.run([validator,str(directory/f'pair.{ext}')],capture_output=True,text=True)
                check(proc.returncode == 0, proc.stdout+proc.stderr)
        else:
            check(not (directory/'pair.tesc').exists() and not (directory/'pair.tese').exists(), 'failure emitted partial stage files')
        return fields
    for offset in [32,36,40,44]:
        f=run(*fixture(offset=offset)); check(f.get('ok')=='1' and f.get('factor')=='_TessellationUniform', f)
    check(run(*fixture(untyped_fork=True)).get('ok')=='1','real compiler untyped fork index declaration')
    run(*fixture())
    (directory/'pair.vert').write_text('#version 320 es\nprecision highp float;\nin vec4 in_POSITION0;\nout vec4 vs_NORMAL0;\nvoid main(){gl_Position=in_POSITION0;vs_NORMAL0=vec4(0,1,0,0);}\n')
    (directory/'pair.geom').write_text('#version 320 es\nprecision highp float;\nlayout(triangles) in;\nlayout(triangle_strip,max_vertices=3) out;\nin vec4 vs_NORMAL0[];\nout vec4 gs_NORMAL0;\nvoid main(){for(int i=0;i<3;i++){gl_Position=gl_in[i].gl_Position;gs_NORMAL0=vs_NORMAL0[i];EmitVertex();}EndPrimitive();}\n')
    (directory/'pair.frag').write_text('#version 320 es\nprecision highp float;\nin vec4 gs_NORMAL0;\nout vec4 color;\nvoid main(){color=gs_NORMAL0;}\n')
    linked=subprocess.run([validator,'-l',*[str(directory/f'pair.{e}') for e in ['vert','tesc','tese','geom','frag']]],capture_output=True,text=True)
    check(linked.returncode==0,linked.stdout+linked.stderr)
    check(run(*fixture(stereo=True)).get('ok')=='1','stereo instance payload survives both tessellation interfaces')
    (directory/'pair.vert').write_text('#version 320 es\nprecision highp float;\nin vec4 in_POSITION0;\nout vec4 vs_NORMAL0;\nout vec4 vs_SV_InstanceID0;\nvoid main(){gl_Position=in_POSITION0;vs_NORMAL0=vec4(0,1,0,0);vs_SV_InstanceID0=vec4(intBitsToFloat(1));}\n')
    (directory/'pair.geom').write_text('#version 320 es\nprecision highp float;\nlayout(triangles) in;\nlayout(triangle_strip,max_vertices=3) out;\nin vec4 vs_NORMAL0[];\nin vec4 vs_SV_InstanceID0[];\nout vec4 gs_NORMAL0;\nvoid main(){for(int i=0;i<3;i++){gl_Position=gl_in[i].gl_Position;gs_NORMAL0=vs_NORMAL0[i]+vec4(float(floatBitsToInt(vs_SV_InstanceID0[i].x)));EmitVertex();}EndPrimitive();}\n')
    linked=subprocess.run([validator,'-l',*[str(directory/f'pair.{e}') for e in ['vert','tesc','tese','geom','frag']]],capture_output=True,text=True)
    check(linked.returncode==0,linked.stdout+linked.stderr)
    for kwargs in [dict(points=4),dict(partition=2),dict(partition=3),dict(extra_domain=[m.insn(19)]),
                   dict(inside_offset=36), dict(extra_domain=[m.insn(62)]),
                   dict(extra_domain=[m.insn(54,m.dest(2,1,7),m.src(0,1))]),
                   dict(extra_domain=[m.insn(75,m.dest(2,1,7),m.src(0,0))]),
                   dict(extra_domain=[m.insn(54,m.dest(2,1,7),m.src(0,0),saturate=True)])]:
        check(run(*fixture(**kwargs)).get('ok')=='0','unsupported topology/domain must fail closed')
    h,d=fixture(reflection=False)
    check(run(h,d).get('ok')=='0','stripped RDEF cannot guess the factor binding')
    check(run(h,d,['0','32','_TessellationUniform']).get('ok')=='1','explicit actual reflection permits stripped shader')
    check(run(h,d,['0','36','_TessellationUniform']).get('ok')=='0','wrong reflected address must be rejected')
    check(run(h,d,['0','32','bad;injected']).get('ok')=='0','reflection cannot inject GLSL')
    check(run(d,h).get('ok')=='0','swapped stages must fail closed')
    check(run(*fixture(swapped=True)).get('ok')=='1','simultaneous register component writes supported')
    evaluation=(directory/'pair.tese').read_text()
    # Every generated SSA value uses earlier values, even for r0.xy <- r0.yx.
    defined=set()
    import re
    for line in evaluation.splitlines():
        if 'float tess_value' not in line: continue
        dest,expression=line.split('=',1)
        check(set(re.findall(r'tess_value\d+',expression)) <= defined,'read precedes register component write')
        defined.update(re.findall(r'tess_value\d+',dest))
    random_source=random.Random(0x7e55)
    h,d=fixture()
    for trial in range(250):
        parts=[bytearray(h),bytearray(d)]
        part=parts[trial%2]
        for _ in range(random_source.randrange(1,6)):
            part[random_source.randrange(len(part))]=random_source.randrange(256)
        run(*parts)
print(f'{checks} tessellation prototype checks passed (including five-stage GLSL link and 250 malformed pairs)')
