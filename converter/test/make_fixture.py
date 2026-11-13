"""Writes a small PC (Direct3D 11) asset bundle with one translatable shader,
for converter/test/capi_test.cpp:  python3 make_fixture.py <output path>

Built with the same fixture builders as tools/bundleconvert/run_tests.py.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "bundleconvert"))
sys.path.insert(0, os.path.join(ROOT, "tools", "dxbc"))

import mkdxbc as dx  # noqa: E402
import mkshader  # noqa: E402
from mkbundle import build  # noqa: E402

DX11_VERTEX_SM50 = 16
ADD, MUL, RET, DCL_TEMPS, DCL_INPUT, DCL_OUTPUT_SIV = 0, 56, 62, 104, 95, 103


def vertex_program():
    isgn = dx.signature_chunk([{"name": "POSITION", "index": 0, "register": 0}], b"ISGN")
    osgn = dx.signature_chunk(
        [{"name": "SV_POSITION", "index": 0, "register": 0, "sv": 1, "rw_mask": 0}], b"OSGN")
    rdef = dx.rdef_chunk(
        constant_buffers=[{"name": "$Globals", "size": 64, "variables": [
            {"name": "unity_MatrixVP", "offset": 0, "size": 64, "class": 3, "type": 3,
             "rows": 4, "columns": 4, "elements": 0}]}],
        bindings=[{"name": "$Globals", "type": 0, "dimension": 0, "bind_point": 0}])
    code = []
    code += dx.insn(DCL_INPUT, dx.dest(dx.OPERAND_INPUT, 0))
    code += dx.insn(DCL_OUTPUT_SIV, dx.dest(dx.OPERAND_OUTPUT, 0), extra=[1])
    code += dx.insn(DCL_TEMPS, extra=[1])
    code += dx.insn(MUL, dx.dest(dx.OPERAND_TEMP, 0),
                    dx.src(dx.OPERAND_INPUT, 0, (0, 0, 0, 0)), dx.src_cb(0, 0))
    code += dx.insn(ADD, dx.dest(dx.OPERAND_OUTPUT, 0), dx.src(dx.OPERAND_TEMP, 0), dx.src_cb(0, 1))
    code += dx.insn(RET)
    return dx.unity_program([rdef, isgn, osgn, dx.shex_chunk([code], stage=1)])


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make_fixture.py <output path>")
    program = [[mkshader.program_blob([mkshader.sub_program(DX11_VERTEX_SM50, vertex_program())])]]
    shaders = [("Custom/VivifyConvertTest", [4], program)]
    sf = mkshader.serialized_file_with_shaders(shaders, sf_version=21, target=19)
    build(sys.argv[1], sf_bytes=[sf], with_resource=False)


if __name__ == "__main__":
    main()
