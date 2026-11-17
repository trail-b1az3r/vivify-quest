import os, re, shutil, sys, subprocess, itertools, struct, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkbundle import build, read_converted, target_of

HERE = os.path.dirname(os.path.abspath(__file__))
def _build_default_binary() -> str:
    """Compile the converter from the current sources.

    Defaulting to a prebuilt /tmp/conv meant a local run could pass against a
    binary built before the change under test -- which is exactly how a link
    error against VivifySerializedFile.cpp reached CI green-looking. Set
    VIVIFY_CONV to skip this and test a binary you built yourself.
    """
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = os.path.join(tempfile.mkdtemp(prefix="vivify-conv-"), "conv")
    cmd = [
        os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-g",
        "-fsanitize=address,undefined", "-I", os.path.join(root, "include"),
        "-o", out,
        os.path.join(root, "tools", "bundleconvert", "main.cpp"),
        os.path.join(root, "src", "VivifyBundleConvert.cpp"),
        os.path.join(root, "src", "VivifySerializedFile.cpp"),
        os.path.join(root, "src", "VivifyDxbc.cpp"),
        os.path.join(root, "src", "VivifyTriangleTessellation.cpp"),
    ]
    subprocess.run(cmd, check=True)
    return out


CONV = os.environ.get("VIVIFY_CONV") or _build_default_binary()
TMP  = os.path.join(HERE, "work")
os.makedirs(TMP, exist_ok=True)

ANDROID = 13
fails = 0
cases = 0

def case(name, **kw):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "src.vivify")
    dst = os.path.join(TMP, "dst.vivify")
    if os.path.exists(dst): os.remove(dst)
    expect = kw.pop("expect", "converted")
    orig, nodes = build(src, **kw)
    proc = subprocess.run([CONV, src, dst], capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    status = ""
    for line in proc.stdout.splitlines():
        if line.startswith("status="): status = line[len("status="):]
    if status != expect:
        print(f"FAIL {name}: expected status '{expect}', got '{status}'\n{out}")
        fails += 1
        return
    if expect != "converted":
        print(f"ok   {name} ({status})")
        return
    try:
        version, uver, urev, cnodes, cdata = read_converted(dst)
    except Exception as e:
        print(f"FAIL {name}: converted bundle did not parse: {e}\n{out}")
        fails += 1
        return
    problems = []
    if version != kw.get("version", 6): problems.append("archive version changed")
    if [n[3] for n in cnodes] != [n[3] for n in nodes]: problems.append("node paths changed")
    if [(n[0], n[1], n[2]) for n in cnodes] != [(n[0], n[1], n[2]) for n in nodes]:
        problems.append("node offsets/sizes/flags changed")
    if len(cdata) != len(orig): problems.append(f"payload length {len(cdata)} != {len(orig)}")
    else:
        sfv = kw.get("sf_version", 17)
        patched = 0
        for (o, s, f, nm) in nodes:
            if nm.endswith(".resS"):
                if cdata[o:o+s] != orig[o:o+s]: problems.append(f"resource node {nm} was modified")
                continue
            t = target_of(cdata, o, s, sfv)
            if t != ANDROID: problems.append(f"{nm} target={t}, expected Android")
            patched += 1
            # everything except the 4 target bytes must be byte-identical
            head_len = 48 if sfv >= 22 else 20
            p = o + head_len
            e = cdata.index(b"\0", p); p = e + 1
            if cdata[o:p] != orig[o:p] or cdata[p+4:o+s] != orig[p+4:o+s]:
                problems.append(f"{nm} changed outside the target-platform field")
        if patched == 0: problems.append("no serialized files patched")
    if problems:
        print(f"FAIL {name}: " + "; ".join(problems))
        fails += 1
    else:
        print(f"ok   {name}")

# archive-level compression x block-level compression
for comp in (0, 1, 2, 3):
    for bcomp in (0, 1, 2, 3):
        case(f"comp={comp} blockcomp={bcomp}", compression=comp, block_compression=bcomp)

# archive header variants
case("version 7 (16-byte aligned header)", version=7)
case("blocks-info at end of file", info_at_end=True)
case("version 7 + info at end", version=7, info_at_end=True)

# SerializedFile header variants
case("SerializedFile v22 (large-file header)", sf_version=22)
case("SerializedFile v21", sf_version=21)
case("SerializedFile v22 + lzma", sf_version=22, compression=1, block_compression=1)

# platform variants
case("source StandaloneWindows (5)", target=5)
case("source StandaloneOSX (2)", target=2)
case("already Android", target=13, expect="already an Android bundle")

# shape variants
case("single serialized file, no resource", n_files=1, with_resource=False)
case("many files / many blocks", n_files=5, payload=60000, block_size=0x2000)
case("large payload, lzma", n_files=3, payload=300000, compression=1, block_compression=1, block_size=0x20000)
case("large payload, lz4", n_files=3, payload=300000, compression=2, block_compression=2, block_size=0x20000)

# negative cases
bad = os.path.join(TMP, "bad.vivify")
open(bad, "wb").write(b"NotAUnityArchive\0garbage" * 10)
p = subprocess.run([CONV, bad, os.path.join(TMP, "bad.out")], capture_output=True, text=True)
cases += 1
if "not a UnityFS asset bundle" in p.stdout:
    print("ok   rejects non-bundle input")
else:
    print("FAIL rejects non-bundle input:\n" + p.stdout); fails += 1

src = os.path.join(TMP, "trunc_src.vivify")
build(src, compression=2, block_compression=2)
raw = open(src, "rb").read()
open(bad, "wb").write(raw[:len(raw)//2])
p = subprocess.run([CONV, bad, os.path.join(TMP, "bad.out")], capture_output=True, text=True)
cases += 1
if p.returncode != 0 and "status=converted" not in p.stdout:
    print("ok   rejects truncated bundle")
else:
    print("FAIL rejects truncated bundle:\n" + p.stdout); fails += 1

cases += 1
if not os.path.exists(os.path.join(TMP, "bad.out")):
    print("ok   no output file left behind on failure")
else:
    print("FAIL stale output file left behind"); fails += 1

# --- the step-4 rewrite path (conv --repack) ---------------------------------
#
# RepackBundle unpacks a bundle, rebuilds every SerializedFile inside it through
# the rewriter, and repacks it. With no shader edits the payload must come back
# exactly as it went in: a resized object is the whole point of the rewriter,
# and if the no-op case does not round-trip then nothing built on it can be
# trusted with a real bundle.

def repack_case(name, **kw):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "rp_src.vivify")
    dst = os.path.join(TMP, "rp_dst.vivify")
    if os.path.exists(dst):
        os.remove(dst)
    orig, nodes = build(src, **kw)
    proc = subprocess.run([CONV, "--repack", src, dst], capture_output=True, text=True)
    status = ""
    for line in proc.stdout.splitlines():
        if line.startswith("status="):
            status = line[len("status="):]
    if status != "converted":
        print(f"FAIL {name}: repack status '{status}'\n{proc.stdout}{proc.stderr}")
        fails += 1
        return
    try:
        version, uver, urev, cnodes, cdata = read_converted(dst)
    except Exception as e:
        print(f"FAIL {name}: repacked bundle did not parse: {e}")
        fails += 1
        return
    problems = []
    if [n[3] for n in cnodes] != [n[3] for n in nodes]:
        problems.append("node paths changed")
    if [(n[0], n[1], n[2]) for n in cnodes] != [(n[0], n[1], n[2]) for n in nodes]:
        problems.append(f"node offsets/sizes changed: {[(n[0], n[1]) for n in cnodes]} != "
                        f"{[(n[0], n[1]) for n in nodes]}")
    if cdata != orig:
        problems.append(f"payload changed ({len(cdata)} vs {len(orig)} bytes)")
    if problems:
        print(f"FAIL {name}: " + "; ".join(problems))
        fails += 1
    else:
        print(f"ok   {name}")


repack_case("repack round-trips the payload byte for byte")
repack_case("repack round-trips SerializedFile v22", sf_version=22)
repack_case("repack round-trips several files and blocks",
            n_files=5, payload=60000, block_size=0x2000)
repack_case("repack round-trips a bundle with no resource node",
            n_files=1, with_resource=False)
repack_case("repack round-trips an lz4-compressed source",
            compression=2, block_compression=2)
repack_case("repack round-trips an already-Android bundle", target=ANDROID)

cases += 1
p = subprocess.run([CONV, "--repack", bad, os.path.join(TMP, "rp_bad.out")],
                   capture_output=True, text=True)
if p.returncode != 0 and not os.path.exists(os.path.join(TMP, "rp_bad.out")):
    print("ok   repack rejects a non-bundle without leaving output")
else:
    print("FAIL repack accepted a non-bundle:\n" + p.stdout); fails += 1

# --- the whole conversion (conv --shaders) -----------------------------------
#
# A bundle carrying a real DXBC vertex program goes in; a bundle whose shader
# says GLES3 and whose program is GLSL text comes out. The strongest check that
# does not need a second parser written in Python is to run the *output* back
# through the same conversion: a converted bundle already targets Android and
# its shaders already run here, so the second pass must find nothing to do.

sys.path.insert(0, os.path.join(os.path.dirname(HERE), "dxbc"))
import mkdxbc as dx  # noqa: E402
import mkshader  # noqa: E402
# Opcode numbers the geometry fixture needs (D3D10_SB_OPCODE_TYPE).
DCL_GS_INPUT_PRIMITIVE_B, DCL_MAXOUT_B, DCL_GS_OUTPUT_TOPOLOGY_B, EMIT_B = 93, 94, 92, 19
RET = 62

DX11_VERTEX_SM50 = 16
DX11_PIXEL_SM50 = 18
GLES3_PLATFORM = 9

MUL, MAD, ADD, RET, DCL_TEMPS, DCL_INPUT, DCL_OUTPUT, DCL_OUTPUT_SIV = 56, 50, 0, 62, 104, 95, 101, 103
MOV = 54
SAMPLE, DCL_SAMPLER, DCL_RESOURCE, DCL_INPUT_PS, MSAD = 69, 90, 88, 98, 213


def dxbc_vertex():
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
    code += dx.insn(ADD, dx.dest(dx.OPERAND_OUTPUT, 0), dx.src(dx.OPERAND_TEMP, 0),
                    dx.src_cb(0, 1))
    code += dx.insn(RET)
    # unity_program, not container: a D3D11 sub-program in a real bundle carries
    # Unity's binding header in front of the DXBC, and a fixture without one
    # tests a shape that never occurs.
    return dx.unity_program([rdef, isgn, osgn, dx.shex_chunk([code], stage=1)])


def dxbc_untranslatable():
    """Same framing, but using an instruction outside the translated subset.

    msad is a sum-of-absolute-differences instruction with no GLSL ES form at
    all, so it stays outside the subset however far the translator grows."""
    isgn = dx.signature_chunk([{"name": "POSITION", "index": 0, "register": 0}], b"ISGN")
    osgn = dx.signature_chunk(
        [{"name": "SV_POSITION", "index": 0, "register": 0, "sv": 1, "rw_mask": 0}], b"OSGN")
    code = []
    code += dx.insn(DCL_TEMPS, extra=[1])
    code += dx.insn(MSAD, dx.dest(dx.OPERAND_TEMP, 0), dx.src(dx.OPERAND_TEMP, 0),
                    dx.src(dx.OPERAND_TEMP, 0), dx.src(dx.OPERAND_TEMP, 0))
    code += dx.insn(RET)
    return dx.unity_program([isgn, osgn, dx.shex_chunk([code], stage=1)])


def shader_bundle(path, shaders, sf_version=21, target=19, textures=None):
    sf = mkshader.serialized_file_with_shaders(shaders, sf_version=sf_version, target=target,
                                               textures=textures)
    return build(path, sf_bytes=[sf], with_resource=False)


def run_shaders(src, dst, mode="--shaders"):
    if os.path.exists(dst):
        os.remove(dst)
    proc = subprocess.run([CONV, mode, src, dst], capture_output=True, text=True)
    fields = {}
    refusals = []
    for line in proc.stdout.splitlines():
        if line.startswith("refusal="):
            refusals.append(line[len("refusal="):])
            continue
        # status and message are prose and hold spaces, so they are read as the
        # whole line; the counters share one line and are split on spaces.
        if line.startswith("status=") or line.startswith("message="):
            key, _, value = line.partition("=")
            fields[key] = value
            continue
        for part in line.split(" "):
            key, _, value = part.partition("=")
            if key and value:
                fields.setdefault(key, value)
    return proc, fields, refusals


def shader_case(name, check, shaders, **kw):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "sh_src.vivify")
    dst = os.path.join(TMP, "sh_dst.vivify")
    mode = kw.pop("mode", "--shaders")
    shader_bundle(src, shaders, **kw)
    proc, fields, refusals = run_shaders(src, dst, mode)
    problem = check(proc, fields, refusals, dst)
    if problem:
        print(f"FAIL {name}: {problem}\n{proc.stdout}{proc.stderr}")
        fails += 1
    else:
        print(f"ok   {name}")


# One platform group holding one sub-blob holding one program.
one_program = [[mkshader.program_blob([mkshader.sub_program(DX11_VERTEX_SM50, dxbc_vertex())])]]


def expect_translated(proc, fields, refusals, dst):
    if fields.get("status") != "converted":
        return f"status '{fields.get('status')}'"
    if fields.get("translated") != "1" or fields.get("programs") != "1":
        return f"translated={fields.get('translated')} programs={fields.get('programs')}"
    try:
        read_converted(dst)
    except Exception as e:
        return f"converted bundle did not parse: {e}"
    return None


shader_case("a DirectX shader is translated and the bundle still parses",
            expect_translated, [("Custom/Test", [4], one_program)])
shader_case("translation works on SerializedFile v22", expect_translated,
            [("Custom/Test", [4], one_program)], sf_version=22)


def expect_idempotent(proc, fields, refusals, dst):
    problem = expect_translated(proc, fields, refusals, dst)
    if problem:
        return problem
    # Second pass over the converted bundle: nothing left to do.
    again = os.path.join(TMP, "sh_dst2.vivify")
    proc2, fields2, _ = run_shaders(dst, again)
    if fields2.get("status") != "already an Android bundle":
        return f"second pass status '{fields2.get('status')}' ({proc2.stdout})"
    if fields2.get("leftAlone") != "1":
        return f"second pass leftAlone={fields2.get('leftAlone')}"
    if os.path.exists(again):
        return "second pass wrote a file it had nothing to change in"
    return None


shader_case("a converted bundle converts to nothing on a second pass",
            expect_idempotent, [("Custom/Test", [4], one_program)])


def expect_refused(proc, fields, refusals, dst):
    if fields.get("status") != "already an Android bundle":
        return f"status '{fields.get('status')}' (nothing translated, nothing to retarget)"
    if fields.get("refused") != "1":
        return f"refused={fields.get('refused')}"
    if not refusals or "msad" not in refusals[0]:
        return f"refusal did not name the instruction: {refusals}"
    return None


untranslatable = [[mkshader.program_blob(
    [mkshader.sub_program(DX11_VERTEX_SM50, dxbc_untranslatable())])]]
shader_case("an untranslatable shader is refused by name, not half-converted",
            expect_refused, [("Custom/Hard", [4], untranslatable)], target=ANDROID)


def expect_mixed(proc, fields, refusals, dst):
    if fields.get("status") != "converted":
        return f"status '{fields.get('status')}'"
    if fields.get("translated") != "1" or fields.get("refused") != "1":
        return f"translated={fields.get('translated')} refused={fields.get('refused')}"
    try:
        read_converted(dst)
    except Exception as e:
        return f"converted bundle did not parse: {e}"
    return None


shader_case("a bundle with one good and one bad shader keeps both and stays loadable",
            expect_mixed,
            [("Custom/Good", [4], one_program), ("Custom/Bad", [4], untranslatable)])

# --- making a bundle's block-compressed textures decodable on device -------
#
# The mod decodes BC textures to RGBA32 at level load, and it can only do that
# while the texture still has a CPU copy -- which Unity keeps only when
# m_IsReadable is set. Conversion sets it. A texture that never gets the flag
# loses its pixels at load, and decoding what is left produces solid black,
# which is what every converted level looked like from 0.8.0 to 0.9.12.
def texture_case(name, specs, check, **kw):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "tex_src.vivify")
    dst = os.path.join(TMP, "tex_dst.vivify")
    shader_bundle(src, [("Custom/Test", [4], one_program)], textures=specs, **kw)
    proc, fields, refusals = run_shaders(src, dst)
    problem = check(proc, fields, refusals, dst)
    if problem:
        print(f"FAIL {name}: {problem}\n{proc.stdout}{proc.stderr}")
        fails += 1
    else:
        print(f"ok   {name}")


def expect_textures(seen, readable, streamed=0):
    def check(proc, fields, refusals, dst):
        if fields.get("status") != "converted":
            return f"status '{fields.get('status')}'"
        got = (fields.get("texSeen"), fields.get("texReadable"), fields.get("texStreamed"))
        want = (str(seen), str(readable), str(streamed))
        if got != want:
            return f"seen/readable/streamed {got}, wanted {want}"
        try:
            read_converted(dst)
        except Exception as e:
            return f"converted bundle did not parse: {e}"
        return None
    return check


texture_case("a BC1 texture is marked readable",
             [dict(name="brick", texture_format=10, image_data=bytes(8))],
             expect_textures(1, 1))
texture_case("a BC7 texture is marked readable",
             [dict(name="sky", texture_format=25, image_data=bytes(16))],
             expect_textures(1, 1))
texture_case("an RGBA32 texture is left alone -- the GPU can already sample it",
             [dict(name="plain", texture_format=4, image_data=bytes(64))],
             expect_textures(0, 0))
texture_case("a texture that is already readable is counted but not rewritten",
             [dict(name="brick", texture_format=10, is_readable=True, image_data=bytes(8))],
             expect_textures(1, 0))
texture_case("a streamed texture is marked readable and reported as streamed",
             [dict(name="streamed", texture_format=12, stream_size=4096,
                   stream_path="archive:/x/x.resS")],
             expect_textures(1, 1, streamed=1))
texture_case("several textures are all found, whatever their formats",
             [dict(name="a", texture_format=10, image_data=bytes(8)),
              dict(name="b", texture_format=4, image_data=bytes(64)),
              dict(name="c", texture_format=12, image_data=bytes(16)),
              dict(name="d", texture_format=27, is_readable=True, image_data=bytes(16))],
             expect_textures(3, 2))
texture_case("the texture pass works on SerializedFile v22",
             [dict(name="brick", texture_format=10, image_data=bytes(8))],
             expect_textures(1, 1), sf_version=22)


# The flag has to survive the rebuild that resizes the shader beside it: the
# byte is written into the data before RewriteSerializedFile relays the file
# around a longer shader body, and a relay that dropped it would leave a bundle
# that converts "successfully" and still renders black.
cases += 1
src = os.path.join(TMP, "tex_src.vivify")
dst = os.path.join(TMP, "tex_dst.vivify")
shader_bundle(src, [("Custom/Test", [4], one_program)],
              textures=[dict(name="brick", texture_format=10, image_data=bytes(8))])
proc, fields, refusals = run_shaders(src, dst)
if fields.get("translated") == "1" and fields.get("texReadable") == "1" and b"brick" in open(dst, "rb").read():
    print("ok   a texture keeps its flag through the shader rebuild beside it")
else:
    print("FAIL the texture pass and the shader rewrite do not survive each other:\n" + proc.stdout)
    fails += 1

# Reading the flag back out of the converted bundle, rather than trusting the
# counter that says it was written: converting the output again finds the
# texture and has nothing left to do to it.
cases += 1
again = os.path.join(TMP, "tex_dst2.vivify")
proc2, fields2, _ = run_shaders(dst, again)
if fields2.get("texSeen") == "1" and fields2.get("texReadable") == "0":
    print("ok   the flag is really in the converted bundle, not just in the counter")
else:
    print("FAIL the converted bundle's texture is not readable:\n" + proc2.stdout)
    fails += 1


# --- conversion through m_ParsedForm (Unity 2021.3.16's real Shader layout) --
#
# These shaders are serialized through Unity's own type tree for the class, so
# m_ParsedForm is complete: passes, per-stage player sub-program lists,
# parameter blobs, keyword names. That is what conversion now edits -- the
# program types Unity selects by, and which store entry each variant uses.
import mkshader2021  # noqa: E402

AND_OP, ITOF, DCL_INPUT_SGV, DCL_INPUT_PS_SGV = 1, 43, 96, 99
SV_RT_ARRAY_INDEX, SV_INSTANCE_ID = 4, 8
GLES3_TYPE = 4


def _vs(spi, texcoord=True, rdef=True):
    inputs = [{"name": "POSITION", "index": 0, "register": 0}]
    outputs = [{"name": "SV_POSITION", "index": 0, "register": 0, "sv": 1, "rw_mask": 0}]
    if texcoord:
        outputs.append({"name": "TEXCOORD", "index": 0, "register": 1, "rw_mask": 0})
    if spi:
        inputs.append({"name": "SV_InstanceID", "index": 0, "register": 1, "sv": SV_INSTANCE_ID,
                       "mask": 0x1, "rw_mask": 0x1, "component_type": 1})
        outputs.append({"name": "SV_RenderTargetArrayIndex", "index": 0, "register": 2,
                        "sv": SV_RT_ARRAY_INDEX, "mask": 0x1, "rw_mask": 0xE, "component_type": 1})
    matrix = "unity_StereoMatrixVP" if spi else "unity_MatrixVP"
    # Not called rdef: that name is the parameter, and shadowing it made every
    # "stripped" fixture carry RDEF after all, so the tests meant to prove the
    # converter works without it never exercised that.
    rdef_chunk = dx.rdef_chunk(
        constant_buffers=[{"name": "$Globals", "size": 128 if spi else 64, "variables": [
            {"name": matrix, "offset": 0, "size": 128 if spi else 64, "class": 3, "type": 3,
             "rows": 4, "columns": 4, "elements": 2 if spi else 0}]}],
        bindings=[{"name": "$Globals", "type": 0, "dimension": 0, "bind_point": 0}])
    code = []
    code += dx.insn(DCL_INPUT, dx.dest(dx.OPERAND_INPUT, 0))
    if spi:
        code += dx.insn(DCL_INPUT_SGV, dx.dest(dx.OPERAND_INPUT, 1, 0x1), extra=[SV_INSTANCE_ID])
    code += dx.insn(DCL_OUTPUT_SIV, dx.dest(dx.OPERAND_OUTPUT, 0), extra=[1])
    if texcoord:
        code += dx.insn(DCL_OUTPUT, dx.dest(dx.OPERAND_OUTPUT, 1))
    if spi:
        code += dx.insn(DCL_OUTPUT_SIV, dx.dest(dx.OPERAND_OUTPUT, 2, 0x1), extra=[SV_RT_ARRAY_INDEX])
    code += dx.insn(DCL_TEMPS, extra=[1])
    code += dx.insn(MUL, dx.dest(dx.OPERAND_TEMP, 0), dx.src(dx.OPERAND_INPUT, 0, (0, 0, 0, 0)),
                    dx.src_cb(0, 0))
    code += dx.insn(ADD, dx.dest(dx.OPERAND_OUTPUT, 0), dx.src(dx.OPERAND_TEMP, 0), dx.src_cb(0, 1))
    if texcoord:
        code += dx.insn(ADD, dx.dest(dx.OPERAND_OUTPUT, 1), dx.src(dx.OPERAND_INPUT, 0), dx.src_cb(0, 1))
    if spi:
        code += dx.insn(AND_OP, dx.dest(dx.OPERAND_OUTPUT, 2, 0x1),
                        dx.src(dx.OPERAND_INPUT, 1, (0, 0, 0, 0)), dx.imm_int(1, 1, 1, 1))
    code += dx.insn(RET)
    chunks = [dx.signature_chunk(inputs, b"ISGN"), dx.signature_chunk(outputs, b"OSGN"),
              dx.shex_chunk([code], stage=1)]
    # Unity strips RDEF from the DXBC in a built bundle; rdef=False is that.
    return dx.unity_program(([rdef_chunk] if rdef else []) + chunks)


def _ps(spi, flat=True):
    inputs = [{"name": "TEXCOORD", "index": 0, "register": 0}]
    if spi:
        inputs.append({"name": "SV_RenderTargetArrayIndex", "index": 0, "register": 1,
                       "sv": SV_RT_ARRAY_INDEX, "mask": 0x1, "rw_mask": 0x1, "component_type": 1})
    outputs = [{"name": "SV_Target", "index": 0, "register": 0, "rw_mask": 0}]
    code = []
    code += dx.insn(DCL_INPUT_PS, dx.dest(dx.OPERAND_INPUT, 0), controls=1 if flat else 2)
    if spi:
        code += dx.insn(DCL_INPUT_PS_SGV, dx.dest(dx.OPERAND_INPUT, 1, 0x1), controls=1,
                        extra=[SV_RT_ARRAY_INDEX])
    code += dx.insn(DCL_OUTPUT, dx.dest(dx.OPERAND_OUTPUT, 0))
    if spi:
        code += dx.insn(ITOF, dx.dest(dx.OPERAND_OUTPUT, 0), dx.src(dx.OPERAND_INPUT, 1, (0, 0, 0, 0)))
    else:
        code += dx.insn(ADD, dx.dest(dx.OPERAND_OUTPUT, 0), dx.src(dx.OPERAND_INPUT, 0),
                        dx.src(dx.OPERAND_INPUT, 0))
    code += dx.insn(RET)
    return dx.unity_program([dx.signature_chunk(inputs, b"ISGN"), dx.signature_chunk(outputs, b"OSGN"),
                             dx.shex_chunk([code], stage=0)])


def pc_shader_2021(name, vertex_programs, fragment_programs, keyword_names=("STEREO_INSTANCING_ON",),
                   share_vertex=False, vertex_params=None, fragment_params_blobs=None, pass_names=None,
                   entry_keywords=None, dependencies=(), geometry_programs=(), grab_texture="", properties=()):
    """A PC (Direct3D 11) shader in 2021.3.16's layout. *_programs: list of
    (dxbc, keyword index list). Every program gets a parameter blob after it,
    as 2021.3.10+ stores them, and fragments live in a second segment. With
    share_vertex, every vertex variant points at the first vertex entry, the
    way Unity dedups identical programs. With entry_keywords (names, indexed
    like keyword_names), each program entry also carries its keyword names, as
    a 2019 bundle's do."""
    entries, vertex_refs, vertex_param_list, fragment_refs, fragment_params = [], [], [], [], []
    for code, keywords in vertex_programs:
        blob = 0 if (share_vertex and vertex_refs) else len(entries)
        vertex_refs.append(mkshader2021.player_sub_program(blob, DX11_VERTEX_SM50, keywords))
        names = [entry_keywords[k] for k in keywords] if entry_keywords else ()
        entries.append((mkshader.sub_program(DX11_VERTEX_SM50, code, keywords=names), 0))
        vertex_param_list.append(len(entries))
        blob = (vertex_params[len(vertex_param_list) - 1] if vertex_params
                else b"\x00\x00\x00\x00PARAMS-VS" + bytes([len(entries)]))
        entries.append((blob, 0))
    for code, keywords in fragment_programs:
        fragment_refs.append(mkshader2021.player_sub_program(len(entries), DX11_PIXEL_SM50, keywords))
        names = [entry_keywords[k] for k in keywords] if entry_keywords else ()
        entries.append((mkshader.sub_program(DX11_PIXEL_SM50, code, keywords=names), 1))
        fragment_params.append(len(entries))
        blob = (fragment_params_blobs[len(fragment_params) - 1] if fragment_params_blobs
                else b"\x00\x00\x00\x00PARAMS-PS" + bytes([len(entries)]))
        entries.append((blob, 1))
    geometry_refs, geometry_params = [], []
    for code, keywords in geometry_programs:
        geometry_refs.append(mkshader2021.player_sub_program(len(entries), 20, keywords))  # DX11 geometry SM5
        entries.append((mkshader.sub_program(20, code), 1))
        geometry_params.append(len(entries))
        entries.append((b"\x00\x00\x00\x00PARAMS-GS" + bytes([len(entries)]), 1))
    stages = {
        mkshader2021.VERTEX: {"player": [vertex_refs], "params": [vertex_param_list]},
        mkshader2021.FRAGMENT: {"player": [fragment_refs], "params": [fragment_params]},
    }
    if geometry_refs:
        stages[mkshader2021.GEOMETRY] = {"player": [geometry_refs], "params": [geometry_params]}
    store = mkshader.build_program_store([mkshader.segmented_chunks(entries)])
    return mkshader2021.shader_body(name, [4], store, [stages], keyword_names, pass_names=pass_names,
                                    dependencies=dependencies, grab_texture=grab_texture, properties=properties), entries


def inspect_converted(dst):
    _, _, _, nodes, data = read_converted(dst)
    off, size, _, _ = nodes[0]
    sf_path = os.path.join(TMP, "inspect.sf")
    with open(sf_path, "wb") as handle:
        handle.write(data[off:off + size])
    proc = subprocess.run([CONV, "--inspect", sf_path], capture_output=True, text=True)
    refs, entries, platforms = [], {}, None
    for line in proc.stdout.splitlines():
        if line.startswith("shader="):
            platforms = line.split("platforms=")[1]
        elif line.startswith("ref "):
            refs.append(dict(part.split("=", 1) for part in line[4:].split(" ")))
        elif line.startswith("entry "):
            head, _, code = line[6:].partition(" code=")
            entry = dict(part.split("=", 1) for part in head.split(" "))
            entry["code"] = code
            entries[int(entry["blob"])] = entry
    return platforms, refs, entries


# A Unity 2019 program: the parameter tables come after the code, behind four-
# byte alignment. DXBC is always a whole number of dwords, so the source never
# has padding there; the translated GLSL is almost never a multiple of four.
# Version 6 of the on-device conversion copied the tables straight after the
# GLSL, Unity read them from one to three bytes early, and the game crashed as
# soon as a converted 2019 level was selected.
TABLES_2019 = struct.pack("<ii", 0, 0) + struct.pack("<i", 1) + struct.pack("<i", 0) + bytes(8) + \
    bytes(range(1, 17))
program_2019 = [[mkshader.program_blob([mkshader.sub_program(
    DX11_VERTEX_SM50, dxbc_vertex(), blob_version=201806140, trailing=TABLES_2019)])]]


def expect_2019_tables_aligned(proc, fields, refusals, dst):
    problem = expect_translated(proc, fields, refusals, dst)
    if problem:
        return problem
    _, _, entries = inspect_converted(dst)
    gles = [e for e in entries.values() if e["code"].startswith("#")]
    if not gles:
        return "no GLSL entry in the converted bundle"
    for entry in gles:
        if entry["trailing"] != TABLES_2019.hex():
            return (f"parameter tables after {len(entry['code'].replace(chr(92) + 'n', 'x'))}-byte code "
                    f"read back as {entry['trailing']}, wrote {TABLES_2019.hex()}")
    return None


shader_case("a 2019 program's parameter tables stay four-byte aligned after its GLSL",
            expect_2019_tables_aligned, [("Custom/Test2019", [4], program_2019)])


def split_stages(code):
    """The per-stage sections of a Unity GLES program ("#ifdef VERTEX" ...
    "#endif"), matching #if/#endif nesting: a stage can hold conditionals of
    its own (UNITY_RUNTIME_INSTANCING_ARRAY_SIZE's #ifndef), so the first
    #endif is not necessarily the stage's."""
    sections, stage, depth, lines = {}, None, 0, []
    for line in code.split("\n"):
        stripped = line.strip()
        if stage is None:
            m = re.match(r"#ifdef (VERTEX|FRAGMENT|GEOMETRY)$", stripped)
            if m:
                stage, depth, lines = m.group(1), 0, []
            continue
        if stripped.startswith("#if"):
            depth += 1
        elif stripped.startswith("#endif"):
            if depth == 0:
                sections[stage] = "\n".join(lines) + "\n"
                stage = None
                continue
            depth -= 1
        lines.append(line)
    return sections


GLSLANG = shutil.which("glslangValidator")


def glslang_link_problem(dst):
    """Compiles and links every linked program in a converted bundle with the
    Khronos reference front-end, when it is installed: a program that passes
    the string checks but that a GLSL ES compiler rejects would still draw
    nothing on the headset."""
    if GLSLANG is None or not os.path.exists(dst):
        return None
    _, _, entries = inspect_converted(dst)
    for blob, entry in entries.items():
        code = entry["code"].replace("\\n", "\n")
        if not code.startswith("#ifdef VERTEX"):
            continue
        sections = split_stages(code)
        files = []
        for stage, ext in (("VERTEX", "vert"), ("FRAGMENT", "frag"), ("GEOMETRY", "geom")):
            if stage in sections:
                path = os.path.join(TMP, f"link_{blob}.{ext}")
                with open(path, "w") as handle:
                    handle.write(sections[stage])
                files.append(path)
        proc = subprocess.run([GLSLANG, "-l"] + files, capture_output=True, text=True, errors="replace")
        if proc.returncode != 0:
            return f"glslang will not link entry {blob}: {(proc.stdout + proc.stderr).strip()[:800]}"
    return None


def pc_shader_case(name, body, check, mode="--shaders"):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "pc_src.vivify")
    dst = os.path.join(TMP, "pc_dst.vivify")
    sf = mkshader.serialized_file_with_shaders([body], sf_version=22, shader_tree=mkshader2021.RealTypeTree())
    build(src, sf_bytes=[sf], with_resource=False)
    proc, fields, refusals = run_shaders(src, dst, mode)
    try:
        problem = check(proc, fields, refusals, dst) or glslang_link_problem(dst)
    except Exception as e:  # noqa: BLE001 - a crash in a check is a failure, with its reason
        problem = f"check raised {type(e).__name__}: {e}"
    if problem:
        print(f"FAIL {name}: {problem}\n{proc.stdout}{proc.stderr}")
        fails += 1
    else:
        print(f"ok   {name}")


stereo_body, stereo_entries = pc_shader_2021(
    "Swifter/Stereo",
    [(_vs(spi=False), []), (_vs(spi=True), [0])],
    [(_ps(spi=False), []), (_ps(spi=True), [0])])


def expect_linked(proc, fields, refusals, dst):
    if fields.get("status") != "converted" or fields.get("linked") != "1":
        return f"status '{fields.get('status')}' linked={fields.get('linked')} refusals={refusals}"
    platforms, refs, entries = inspect_converted(dst)
    if platforms != "9":
        return f"platforms {platforms}, wanted GLES3 (9)"
    if len(refs) != 4 or any(r["type"] != str(GLES3_TYPE) for r in refs):
        return f"every variant should now say GLES3: {refs}"
    by_stage = {(r["stage"], r["keywords"]): r for r in refs}
    plain_vs = entries[int(by_stage[("0", "")]["blob"])]["code"]
    for needle in ("#ifdef VERTEX\\n#version 300 es\\n#extension GL_OVR_multiview2 : require\\n"
                   "layout(num_views = 2) in;", "#ifdef FRAGMENT\\n", "gl_InstanceID * 2 + int(gl_ViewID_OVR)"):
        if needle not in plain_vs:
            return f"the plain vertex variant's program lacks '{needle}': {plain_vs}"
    if "hlslcc_mtx4x4unity_StereoMatrixVP" not in plain_vs:
        return "the plain variant was not handed its SPI twin's per-eye code"
    if "flat out vec4 vs_TEXCOORD0;" not in plain_vs:
        return f"the fragment's flat qualifier did not reach the vertex output: {plain_vs}"
    spi_vs = entries[int(by_stage[("0", "STEREO_INSTANCING_ON")]["blob"])]["code"]
    if "int(gl_ViewID_OVR)" not in spi_vs.split("#ifdef FRAGMENT")[1]:
        return "the SPI variant's fragment does not read the eye from gl_ViewID_OVR"
    plain_fs = plain_vs.split("#ifdef FRAGMENT")[1]
    if "int(gl_ViewID_OVR)" not in plain_fs:
        return "the plain fragment variant was not handed its SPI twin's eye index"
    for index, (payload, _) in enumerate(stereo_entries):
        if payload.startswith(b"\x00\x00\x00\x00PARAMS"):
            entry = entries.get(index)
            if entry is None or entry["raw"] != "1" or int(entry["rawSize"]) != len(payload):
                return f"parameter blob {index} did not survive: {entry}"
    frag = entries[int(by_stage[("1", "")]["blob"])]["code"]
    if not frag.startswith("#ifdef VERTEX\\n") or "#ifdef FRAGMENT\\n" not in frag:
        return f"the fragment variant's entry does not hold a linked program: {frag}"
    if fields.get("stereoRemapped") != "2":
        return f"stereoRemapped={fields.get('stereoRemapped')}"
    return None


pc_shader_case("a PC shader is linked into multiview GLES programs Unity will select",
               stereo_body, expect_linked)
# Kept for fuzz.py, which mutates it to exercise the m_ParsedForm walker and the
# program-list patching on hostile input.
with open(os.path.join(TMP, "pc_src.vivify"), "rb") as _seed, \
        open(os.path.join(TMP, "fuzz_seed_2021.vivify"), "wb") as _copy:
    _copy.write(_seed.read())


def expect_idempotent_linked(proc, fields, refusals, dst):
    problem = expect_linked(proc, fields, refusals, dst)
    if problem:
        return problem
    again = os.path.join(TMP, "pc_dst2.vivify")
    _, fields2, _ = run_shaders(dst, again)
    if fields2.get("status") != "already an Android bundle" or fields2.get("leftAlone") != "1":
        return f"second pass: {fields2}"
    return None


pc_shader_case("a linked shader is left alone by a second pass", stereo_body, expect_idempotent_linked)


# With separate stereo variants, the plain variant gets single-view code --
# GL refuses a multiview program in the single-view framebuffers blits, render
# textures and CustomRenderTextures draw into -- and the stereo variant keeps
# multiview code under the keyword Unity turns on for two-eye rendering.
def expect_stereo_split(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    if fields.get("stereoSplit") != "2" or fields.get("stereoRemapped") != "0":
        return f"stereoSplit={fields.get('stereoSplit')} stereoRemapped={fields.get('stereoRemapped')}"
    _, refs, entries = inspect_converted(dst)
    vertex = {r["keywords"]: r for r in refs if r["stage"] == "0"}
    if set(vertex) != {"", "STEREO_MULTIVIEW_ON"}:
        return f"vertex variants are {sorted(vertex)}; STEREO_INSTANCING_ON was not renamed"
    plain = entries[int(vertex[""]["blob"])]["code"]
    stereo = entries[int(vertex["STEREO_MULTIVIEW_ON"]["blob"])]["code"]
    if "num_views" in plain or "GL_OVR_multiview" in plain:
        return "the plain variant is still a multiview program"
    if "layout(num_views = 2) in;" not in stereo or "gl_InstanceID * 2 + int(gl_ViewID_OVR)" not in stereo:
        return "the stereo variant is not the multiview translation of the SPI program"
    if "hlslcc_mtx4x4unity_MatrixVP" not in plain:
        return "the plain variant is not the plain program's own code"
    return None


pc_shader_case("with separate stereo variants, plain gets single-view code and stereo becomes STEREO_MULTIVIEW_ON",
               stereo_body, expect_stereo_split, mode="--shaders-split")

# A 2019 bundle keeps its keyword names in the program entries, and Unity
# picks its variants through the pass's m_NameIndices, which renaming the
# entries' names does not reach. 0.14.0-0.14.4 split such bundles anyway: the
# stereo variants stayed under a keyword the Quest never enables, the eye
# cameras drew the single-view plain programs, and notes and most visuals of
# downloaded PC maps were invisible. A 2019 bundle keeps the plain variants on
# their stereo twin's multiview code instead, even in split mode.
sps_body, _ = pc_shader_2021(
    "Swifter/Stereo2019",
    [(_vs(spi=False), []), (_vs(spi=True), [0])],
    [(_ps(spi=False), []), (_ps(spi=True), [0])],
    keyword_names=(), entry_keywords=("UNITY_SINGLE_PASS_STEREO",))


def expect_2019_not_split(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    if fields.get("stereoSplit") != "0":
        return f"stereoSplit={fields.get('stereoSplit')}: a 2019 bundle was split"
    if fields.get("stereoRemapped") != "2":
        return f"stereoRemapped={fields.get('stereoRemapped')}: the plain variants did not get their twin's code"
    _, refs, entries = inspect_converted(dst)
    for ref in refs:
        code = entries[int(ref["blob"])]["code"]
        if ref["stage"] == "0" and "layout(num_views = 2) in;" not in code:
            return f"vertex variant {ref['keywords']!r} is not a multiview program"
    for entry in entries.values():
        if "STEREO_MULTIVIEW_ON" in entry.get("keywords", ""):
            return "a 2019 entry's keyword was renamed"
    return None


pc_shader_case("a 2019 bundle is not split: plain variants get their stereo twin's multiview code",
               sps_body, expect_2019_not_split, mode="--shaders-split")

# 0.14.8: a 2019 bundle built for single-pass instanced stereo can be split
# after all, by renaming STEREO_INSTANCING_ON where a 2019 bundle picks its
# variants: each pass's m_NameIndices key. Its plain variants then get
# single-view code, which replay and recording cameras need.
spi2019_body, _ = pc_shader_2021(
    "Swifter/Stereo2019SPI",
    [(_vs(spi=False), []), (_vs(spi=True), [0])],
    [(_ps(spi=False), []), (_ps(spi=True), [0])],
    keyword_names=(), entry_keywords=("STEREO_INSTANCING_ON",),
    pass_names={"STEREO_INSTANCING_ON": 7, "_Color": 8})


def expect_2019_split(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    if fields.get("stereoSplit") != "2" or fields.get("stereoRemapped") != "0":
        return f"stereoSplit={fields.get('stereoSplit')} stereoRemapped={fields.get('stereoRemapped')}"
    _, _, _, nodes, data = read_converted(dst)
    off, size, _, _ = nodes[0]
    body = data[off:off + size]
    renamed = struct.pack("<i", 19) + b"STEREO_MULTIVIEW_ON\x00" + struct.pack("<i", 7)
    if renamed not in body:
        return "m_NameIndices' STEREO_INSTANCING_ON was not renamed in place"
    if struct.pack("<i", 20) + b"STEREO_INSTANCING_ON" + struct.pack("<i", 7) in body:
        return "the old m_NameIndices key is still there"
    if struct.pack("<i", 6) + b"_Color" + b"\x00\x00" + struct.pack("<i", 8) not in body:
        return "the next m_NameIndices entry moved"
    _, refs, entries = inspect_converted(dst)
    vertex = [r for r in refs if r["stage"] == "0"]
    codes = [entries[int(r["blob"])]["code"] for r in vertex]
    multiview = [("num_views" in c) for c in codes]
    if sorted(multiview) != [False, True]:
        return f"expected one single-view and one multiview vertex program, got {multiview}"
    return None


pc_shader_case("a 2019 SPI bundle split renames m_NameIndices in place: single-view plain, multiview stereo",
               spi2019_body, expect_2019_split, mode="--shaders-split2019")
# Without the 2019 option, the same bundle keeps multiview code in its plain variants.
pc_shader_case("a 2019 SPI bundle is not split unless asked",
               spi2019_body, lambda proc, fields, refusals, dst: None if fields.get("stereoSplit") == "0"
               and fields.get("stereoRemapped") == "2" else f"{fields}", mode="--shaders-split")

# Replay render mode: every program single-view, no keyword renamed and no
# plain variant pointed at its stereo twin, for single-screen cameras.
def expect_single_view_only(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    if fields.get("stereoSplit") != "0" or fields.get("stereoRemapped") != "0":
        return f"stereoSplit={fields.get('stereoSplit')} stereoRemapped={fields.get('stereoRemapped')}"
    _, refs, entries = inspect_converted(dst)
    vertex = {r["keywords"]: r for r in refs if r["stage"] == "0"}
    if set(vertex) != {"", "STEREO_INSTANCING_ON"}:
        return f"vertex variants are {sorted(vertex)}; a keyword was renamed"
    for keywords, ref in vertex.items():
        code = entries[int(ref["blob"])]["code"]
        if "num_views" in code or "GL_OVR_multiview" in code:
            return f"variant {keywords!r} is a multiview program"
    if "hlslcc_mtx4x4unity_MatrixVP" not in entries[int(vertex[""]["blob"])]["code"]:
        return "the plain variant is not the plain program's own code"
    return None


pc_shader_case("replay render mode: every variant single-view, its own code, keywords untouched",
               stereo_body, expect_single_view_only, mode="--shaders-single")

# A geometry stage that cannot run here (multiview forbids one; this one does
# not even translate) no longer leaves the variant on DirectX -- a grey
# stand-in or nothing on the headset. Its vertex and fragment programs link
# without it, and the geometry entry stays DirectX so Unity skips the stage.
gs_body, _ = pc_shader_2021(
    "Swifter/TriangleExplosion", [(_vs(spi=False), [])], [(_ps(spi=False), [])], keyword_names=(),
    geometry_programs=[(b"not a geometry program", [])])


def expect_geometry_dropped(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    if fields.get("geometryDropped") != "1":
        return f"geometryDropped={fields.get('geometryDropped')}"
    _, refs, entries = inspect_converted(dst)
    by_stage = {r["stage"]: r for r in refs}
    if by_stage["2"]["type"] != "20":
        return f"the geometry entry was relabelled (type {by_stage['2']['type']}); Unity would try to run it"
    code = entries[int(by_stage["0"]["blob"])]["code"]
    if "GEOMETRY" in code or "gl_Position" not in code:
        return "the vertex+fragment program is not a plain linked one that writes gl_Position"
    return None


# The mod keeps this off (0.14.13); the option still works when asked for.
os.environ["VIVIFY_DROP_GEOMETRY"] = "1"
pc_shader_case("an untranslatable geometry stage is dropped when asked; vertex and fragment still link",
               gs_body, expect_geometry_dropped)
del os.environ["VIVIFY_DROP_GEOMETRY"]


def expect_geometry_kept(proc, fields, refusals, dst):
    if fields.get("geometryDropped") != "0" or fields.get("variantsRefused") == "0":
        return f"geometryDropped={fields.get('geometryDropped')} variantsRefused={fields.get('variantsRefused')}"
    return None


pc_shader_case("by default an untranslatable geometry stage leaves its variant as it was",
               gs_body, expect_geometry_kept)

# A geometry stage that passes a semantic straight through (0.14.27): the
# output is renamed and the fragment stage reads the renamed name, so the
# variant links and keeps its geometry. Before, it was refused and left on
# DirectX -- grey or missing models on the headset (42 Flux, Extra Sensory II).
def _geometry_passthrough_dxbc():
    isgn = dx.signature_chunk([{"name": "TEXCOORD", "index": 0, "register": 0, "mask": 0x3, "rw_mask": 0x3}], b"ISGN")
    osgn = dx.signature_chunk([{"name": "TEXCOORD", "index": 0, "register": 0, "mask": 0x3, "rw_mask": 0x3}], b"OSGN")
    code = dx.insn(DCL_GS_INPUT_PRIMITIVE_B, controls=3)
    code += dx.insn(DCL_MAXOUT_B, extra=[3])
    code += dx.insn(DCL_GS_OUTPUT_TOPOLOGY_B, controls=5)
    code += dx.insn(MOV, dx.dest(dx.OPERAND_OUTPUT, 0), dx._operand(dx.OPERAND_INPUT, [1, 0], swizzle=(0, 1, 2, 3)))
    code += dx.insn(EMIT_B)
    code += dx.insn(RET)
    return dx.unity_program([isgn, osgn, dx.shex_chunk([code], stage=2)])


gs_pass_body, _ = pc_shader_2021(
    "Swifter/GeometryPassthrough", [(_vs(spi=False), [])], [(_ps(spi=False), [])], keyword_names=(),
    geometry_programs=[(_geometry_passthrough_dxbc(), [])])


def expect_passthrough_linked(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    if fields.get("geometryDropped") != "0":
        return f"geometryDropped={fields.get('geometryDropped')}: the geometry stage was not kept"
    _, refs, entries = inspect_converted(dst)
    by_stage = {r["stage"]: r for r in refs}
    if "2" not in by_stage:
        return "no geometry entry in the converted shader"
    if by_stage["2"]["type"] == "20":
        return "the geometry entry was left on DirectX; it should have been translated"
    code = entries[int(by_stage["0"]["blob"])]["code"]
    if "flat in vec4 gs_TEXCOORD0;" not in code or "flat out vec4 gs_TEXCOORD0;" not in code:
        return "geometry pass-through interpolation does not match the flat fragment input (Adreno rejects this)"
    return None


pc_shader_case("a geometry stage that passes a varying through links with the fragment stage",
               gs_pass_body, expect_passthrough_linked)

# Upstream 0.15.9 flattens even translatable geometry for its ordinary
# multiview renderer. Explicit-eye donors must still retain the full stage.
os.environ["VIVIFY_DROP_ALL_GEOMETRY"] = "1"
pc_shader_case("upstream flat geometry option drops a translatable stage",
               gs_pass_body, expect_geometry_dropped)
del os.environ["VIVIFY_DROP_ALL_GEOMETRY"]
for donor_mode in ("--shaders-eye-left-isolated", "--shaders-eye-right-isolated", "--shaders-mono-isolated"):
    pc_shader_case("full geometry survives in donor " + donor_mode,
                   gs_pass_body, expect_passthrough_linked, mode=donor_mode)

# The blit companion: single-view programs in a bundle whose internal files
# are renamed, so it can be loaded beside the main conversion.
def expect_companion(proc, fields, refusals, dst):
    problem = expect_single_view_only(proc, fields, refusals, dst)
    if problem:
        return problem
    # Compared against a plain single-view conversion of the same source.
    plain = os.path.join(TMP, "pc_plain.vivify")
    run_shaders(os.path.join(TMP, "pc_src.vivify"), plain, "--shaders-single")
    a = [n[3] for n in read_converted(plain)[3]]
    b = [n[3] for n in read_converted(dst)[3]]
    if len(a) != len(b) or any(x == y for x, y in zip(a, b) if "CAB-" in x):
        return f"internal files not renamed: {a} -> {b}"
    if any(len(x) != len(y) for x, y in zip(a, b)):
        return f"a renamed path changed length: {a} -> {b}"
    return None


pc_shader_case("the blit companion is single-view with its internal files renamed",
               stereo_body, expect_companion, mode="--shaders-companion")

def expect_eye_companion(eye):
    def validate(proc, fields, refusals, dst):
        if fields.get("refused") != "0" or fields.get("variantsRefused") != "0":
            return f"eye conversion refused programs: {fields} {refusals}"
        _, refs, entries = inspect_converted(dst)
        code = "\n".join(e["code"] for e in entries.values())
        if "gl_ViewID_OVR" in code or "num_views" in code or "gl_Layer" in code:
            return "eye companion still requires multiview or a layered target"
        if f"gl_InstanceID * 2 + {eye}" not in code:
            return "eye companion lost SPI instance/eye selection"
        plain_vertices = [r for r in refs if r["stage"] == "0" and not r["keywords"]]
        if not plain_vertices or any(f"gl_InstanceID * 2 + {eye}" not in
                                    entries[int(r["blob"])]["code"] for r in plain_vertices):
            return "the selected plain variant did not inherit its stereo twin's eye logic"
        # All three companions must be loadable beside the original and each
        # other, even when the archive's first hash digit wraps around f.
        paths = [[n[3] for n in read_converted(dst)[3]]]
        for mode in ("--shaders-companion", "--shaders-eye-left" if eye else "--shaders-eye-right"):
            other = os.path.join(TMP, mode + ".vivify")
            run_shaders(os.path.join(TMP, "pc_src.vivify"), other, mode)
            paths.append([n[3] for n in read_converted(other)[3]])
        for names in zip(*paths):
            if "CAB-" in names[0] and len(set(names)) != len(names):
                return f"companions have colliding archive names: {names}"
            if len(set(map(len, names))) != 1:
                return f"renaming changed a path length: {names}"
        return None
    return validate

for eye, side in enumerate(("left", "right")):
    pc_shader_case(f"{side} eye companion preserves its eye without multiview or layer routing",
                   spi2019_body, expect_eye_companion(eye), mode="--shaders-eye-" + side)
    shader_case(f"{side} eye refuses legacy shaders with no parsed variant metadata",
                lambda proc, fields, refusals, dst: None if fields.get("translated") == "0" and
                fields.get("refused") == "1" and any("requires parsed shader variants" in r for r in refusals)
                else f"{fields} {refusals}", [("Legacy/Test", [4], one_program)], mode="--shaders-eye-" + side)

os.environ["VIVIFY_SKIP_UNUSED"] = "1"
pc_shader_case("right eye survives stereo-variant pruning in the shader Unity actually selects",
               spi2019_body, expect_eye_companion(1), mode="--shaders-eye-right")
del os.environ["VIVIFY_SKIP_UNUSED"]

def expect_isolated_eye(eye):
    def check(proc, fields, refusals, dst):
        problem = expect_eye_companion(eye)(proc, fields, refusals, dst)
        if problem: return problem
        _, _, _, nodes, data = read_converted(dst)
        off, size, _, _ = nodes[0]
        path = os.path.join(TMP, "isolated.sf")
        with open(path, "wb") as handle: handle.write(data[off:off + size])
        inspected = subprocess.run([CONV, "--inspect", path], capture_output=True, text=True, check=True)
        names = [line[7:].split(" platforms=")[0] for line in inspected.stdout.splitlines() if line.startswith("shader=")]
        if not names or any(not n.startswith(f"__VivifyEye{eye}__/") for n in names):
            return f"donor shader names were not isolated: {names}"
        return None
    return check

for eye, side in enumerate(("left", "right")):
    pc_shader_case(f"{side} eye shader names are isolated without breaking serialized program offsets",
                   spi2019_body, expect_isolated_eye(eye), mode="--shaders-eye-" + side + "-isolated")

anonymous_body, _ = pc_shader_2021(
    "", [(_vs(spi=False), []), (_vs(spi=True), [0])],
    [(_ps(spi=False), []), (_ps(spi=True), [0])],
    keyword_names=(), entry_keywords=("STEREO_INSTANCING_ON",),
    pass_names={"STEREO_INSTANCING_ON": 7, "_Color": 8})
pc_shader_case("an anonymous shader receives an isolated name and retains valid program offsets",
               anonymous_body, expect_isolated_eye(1), mode="--shaders-eye-right-isolated")

# All normal/mono/eye/blit donors must coexist, with matching root and parsed
# names and intact program offsets. Blits retain SPI eyes rather than using a
# plain mono program that can constant-fold eye-dependent effects away.
def expect_new_donor(prefix, eye=None):
    def validate(proc, fields, refusals, dst):
        problem = expect_eye_companion(eye)(proc, fields, refusals, dst) if eye is not None else (None if fields.get("refused") == "0" and fields.get("variantsRefused") == "0" else str((fields, refusals)))
        if problem: return problem
        _, _, _, nodes, data = read_converted(dst)
        for off, size, _, path in nodes:
            if path.endswith(".resS"): continue
            body = data[off:off + size]
            if body.count(prefix.encode()) < 2: return "root/parsed shader names are not both isolated"
        paths = [[n[3] for n in read_converted(dst)[3]]]
        own = "--shaders-mono-isolated" if eye is None else "--shaders-blit-" + ("left" if eye == 0 else "right") + "-isolated"
        for mode in ("--shaders-companion", "--shaders-eye-left-isolated", "--shaders-eye-right-isolated", "--shaders-mono-isolated", "--shaders-blit-left-isolated", "--shaders-blit-right-isolated", "--shaders-native-perspective-isolated"):
            if mode == own: continue
            other = os.path.join(TMP, mode + ".vivify")
            run_shaders(os.path.join(TMP, "pc_src.vivify"), other, mode)
            paths.append([n[3] for n in read_converted(other)[3]])
        if any(len(set(names)) != len(names) for names in zip(*paths)):
            return "a donor shares an internal archive name with another mode"
        if eye is None:
            _, _, entries = inspect_converted(dst)
            if any("gl_InstanceID * 2" in e["code"] for e in entries.values()): return "authored mono camera uses SPI eye remapping"
        return None
    return validate
pc_shader_case("isolated mono camera shaders coexist with every eye and blit donor",
               spi2019_body, expect_new_donor("__VivifyMono__/"), mode="--shaders-mono-isolated")
for eye, side in enumerate(("left", "right")):
    pc_shader_case(f"{side} blit donor preserves eye logic and has its own shader/archive names",
                   spi2019_body, expect_new_donor(f"__VivifyBlitEye{eye}__/", eye), mode=f"--shaders-blit-{side}-isolated")

# A named GrabPass is a camera capture, while an arbitrary texture array in the
# very same program remains a real array. Exercise this through serialization.
from pathlib import Path
sys.path.insert(0, str(Path(HERE).parent / "dxbc"))
import mkdxbc as dx
resources = ["_CameraDepthTexture", "_CapturedScene", "_MainTex", "_Atlas"]
bindings = [{"name": name, "type": 2, "dimension": 5, "bind_point": i} for i, name in enumerate(resources)]
body = []
for i in range(len(resources)):
    body += dx.insn(69, dx.dest(2, 0), dx.imm_float(.25, .5, 1., 0.), dx.src(7, i), dx.src(6, i))
body += dx.insn(62)
ps_array = dx.container([dx.rdef_chunk(bindings=bindings),
    dx.signature_chunk([{"name": "SV_Target", "register": 0, "sv": 64}], b"OSGN"), dx.shex_chunk([body], stage=0)])
camera_body, _ = pc_shader_2021("Test/CameraArray", [(_vs(spi=False), []), (_vs(spi=True), [0])],
                               [(ps_array, []), (ps_array, [0])], grab_texture="_CapturedScene")
def expect_camera_textures(blit):
    def validate(proc, fields, refusals, dst):
        if fields.get("refused") != "0" or fields.get("variantsRefused") != "0": return str((fields, refusals))
        _, _, entries = inspect_converted(dst)
        code = "\n".join(e["code"] for e in entries.values())
        for name in resources:
            shape = "2D" if name in ("_CameraDepthTexture", "_CapturedScene") or (blit and name == "_MainTex") else "2DArray"
            if f"sampler{shape} {name};" not in code: return f"wrong shape for {name}: expected {shape}"
        return None
    return validate
for blit, mode in ((False, "--shaders-eye-right-isolated"), (True, "--shaders-blit-right-isolated")):
    pc_shader_case(f"camera textures lower to 2D without changing authored array assets (blit={blit})",
                   camera_body, expect_camera_textures(blit), mode=mode)

# Variants a Quest never selects are left untranslated when asked (the mod
# asks for converted maps): here the stereo copy of an unsplit 2019 shader.
# The plain variant still gets its stereo twin's multiview code.
def expect_unused_skipped(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("skipped") != "2":
        return f"linked={fields.get('linked')} skipped={fields.get('skipped')} {refusals}"
    _, refs, entries = inspect_converted(dst)
    gles = [r for r in refs if r["type"] in ("3", "4")]
    if len(gles) != 2:
        return f"{len(gles)} GLES refs; expected just the plain vertex and fragment"
    vertex = [r for r in gles if r["stage"] == "0"][0]
    if "num_views" not in entries[int(vertex["blob"])]["code"]:
        return "the plain vertex variant lost its twin's multiview code"
    return None


os.environ["VIVIFY_SKIP_UNUSED"] = "1"
pc_shader_case("variants a Quest never selects are left untranslated; the plain one keeps multiview code",
               spi2019_body, expect_unused_skipped)
del os.environ["VIVIFY_SKIP_UNUSED"]

mono_body, _ = pc_shader_2021("Custom/Mono", [(_vs(spi=False, texcoord=False), [])],
                              [(_ps(spi=False, flat=False), [])], keyword_names=())


def expect_missing_varying(proc, fields, refusals, dst):
    if fields.get("linked") != "1":
        return f"linked={fields.get('linked')} refusals={refusals}"
    _, refs, entries = inspect_converted(dst)
    code = entries[int(refs[0]["blob"])]["code"]
    vertex = code.split("#ifdef FRAGMENT")[0]
    if "out vec4 vs_TEXCOORD0;" not in vertex:
        return f"a varying the fragment reads was not declared on the vertex side: {vertex}"
    if "stereoInstanced" in code or "gl_InstanceID * 2" in code:
        return "a mono shader was given the SPI instance remap"
    return None


pc_shader_case("a varying only the fragment declares is declared on the vertex side too",
               mono_body, expect_missing_varying)

# Unity dedups identical programs, so two keyword variants of the vertex stage
# can share one store entry while their fragments differ. Linked, they are two
# different programs, and the entry has to be split rather than overwritten.
shared_vs = _vs(spi=False)
split_body, split_entries = pc_shader_2021(
    "Custom/Fog", [(shared_vs, []), (shared_vs, [0])],
    [(_ps(spi=False, flat=False), []), (_ps(spi=False, flat=True), [0])], keyword_names=("FOG",),
    share_vertex=True)


def expect_split(proc, fields, refusals, dst):
    if fields.get("linked") != "1":
        return f"linked={fields.get('linked')} refusals={refusals}"
    _, refs, entries = inspect_converted(dst)
    vertex = {r["keywords"]: r for r in refs if r["stage"] == "0"}
    a, b = entries[int(vertex[""]["blob"])]["code"], entries[int(vertex["FOG"]["blob"])]["code"]
    if a == b:
        return "variants that link different fragments ended up with the same program"
    if "flat out vec4 vs_TEXCOORD0;" not in b or "flat out vec4 vs_TEXCOORD0;" in a:
        return "each variant should carry its own fragment's interpolation"
    return None


pc_shader_case("vertex variants that link different fragments get separate programs",
               split_body, expect_split)

# What a real built bundle looks like: Unity strips RDEF out of every DXBC
# program, and the names of its constant buffers, uniforms and textures live
# only in m_ParsedForm -- here, 2021.3.10+ parameter blobs plus the pass's
# m_NameIndices. Every real PC shader failed on this before ("reads constant
# buffer b0, which its reflection data does not describe").
names = {"$Globals": 0, "unity_StereoMatrixVP": 1, "unity_MatrixVP": 2}
mono_params = mkshader2021.parameter_blob(constant_buffers=[(0, 64, [], [(2, 0, 4, 0)])], bindings=[(0, 0)])
spi_params = mkshader2021.parameter_blob(constant_buffers=[(0, 128, [], [(1, 0, 4, 2)])], bindings=[(0, 0)])
empty_params = mkshader2021.parameter_blob()
stripped_body, _ = pc_shader_2021(
    "Swifter/Stripped",
    [(_vs(spi=False, rdef=False), []), (_vs(spi=True, rdef=False), [0])],
    [(_ps(spi=False), []), (_ps(spi=True), [0])],
    vertex_params=[mono_params, spi_params], fragment_params_blobs=[empty_params, empty_params],
    pass_names=names)


def expect_stripped(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    _, refs, entries = inspect_converted(dst)
    vertex = {r["keywords"]: r for r in refs if r["stage"] == "0"}
    code = entries[int(vertex[""]["blob"])]["code"]
    if "uniform vec4 hlslcc_mtx4x4unity_StereoMatrixVP[8];" not in code:
        return f"the parameter blob's matrix did not become the uniform: {code}"
    return None


pc_shader_case("a shader whose DXBC has no RDEF is translated from m_ParsedForm's parameters",
               stripped_body, expect_stripped)

# Dialtone (2021.3.16) writes its parameter blobs in Unity's inline layout --
# names as strings, behind a format version -- rather than through the type
# tree. Read only the type-tree way, every blob failed to parse, the stereo
# variants lost the UnityStereoGlobals buffer they read, and all three of its
# shaders were refused ("reads constant buffer b4, which its reflection data
# does not describe").
inline_mono = mkshader2021.inline_parameter_blob(
    constant_buffers=[("UnityPerFrame", 64, [], [("unity_MatrixVP", 0, 4, 0)])], bindings=[("UnityPerFrame", 0)])
inline_spi = mkshader2021.inline_parameter_blob(
    constant_buffers=[("UnityStereoGlobals", 128, [], [("unity_StereoMatrixVP", 0, 4, 2)])],
    bindings=[("UnityStereoGlobals", 0)])
inline_empty = mkshader2021.inline_parameter_blob()
inline_body, _ = pc_shader_2021(
    "Dialtone/Inline",
    [(_vs(spi=False, rdef=False), []), (_vs(spi=True, rdef=False), [0])],
    [(_ps(spi=False), []), (_ps(spi=True), [0])],
    vertex_params=[inline_mono, inline_spi], fragment_params_blobs=[inline_empty, inline_empty])

def expect_stereo_globals_block(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    _, refs, entries = inspect_converted(dst)
    vertex = {r["keywords"]: r for r in refs if r["stage"] == "0"}
    code = entries[int(vertex[""]["blob"])]["code"]
    # Multiview delivers the per-eye matrices in this block; loose uniforms
    # of the same names are not kept current, and read stale matrices that
    # put everything in the wrong place.
    if "layout(std140) uniform UnityStereoGlobals { vec4 vivify_cb_UnityStereoGlobals[68]; };" not in code:
        return f"UnityStereoGlobals is not the uniform block multiview fills: {code}"
    if "uniform vec4 hlslcc_mtx4x4unity_StereoMatrixVP" in code:
        return "the stereo matrices are still read as loose uniforms"
    return None


pc_shader_case("a parameter blob in Unity's inline layout is read for its names",
               inline_body, expect_stereo_globals_block)

# GPU instancing: Unity compiles a per-instance array at a placeholder length
# of 2, and on DirectX a read past it still reaches the real buffer. Declared
# in GLSL at that length, every instance after the second read garbage --
# which scrambled chords and chains in Burning Sands, since Beat Saber draws
# notes that appear together as one instanced batch. The array has to be sized
# by UNITY_RUNTIME_INSTANCING_ARRAY_SIZE, which the engine defines on load.
def _instanced_vs():
    inputs = [{"name": "POSITION", "index": 0, "register": 0},
              {"name": "SV_InstanceID", "index": 0, "register": 1, "sv": SV_INSTANCE_ID,
               "mask": 0x1, "rw_mask": 0x1, "component_type": 1}]
    outputs = [{"name": "SV_POSITION", "index": 0, "register": 0, "sv": 1, "rw_mask": 0},
               {"name": "TEXCOORD", "index": 0, "register": 1, "rw_mask": 0}]
    code = []
    code += dx.insn(DCL_INPUT, dx.dest(dx.OPERAND_INPUT, 0))
    code += dx.insn(DCL_INPUT_SGV, dx.dest(dx.OPERAND_INPUT, 1, 0x1), extra=[SV_INSTANCE_ID])
    code += dx.insn(DCL_OUTPUT_SIV, dx.dest(dx.OPERAND_OUTPUT, 0), extra=[1])
    code += dx.insn(DCL_OUTPUT, dx.dest(dx.OPERAND_OUTPUT, 1))
    code += dx.insn(DCL_TEMPS, extra=[1])
    code += dx.insn(MOV, dx.dest(dx.OPERAND_TEMP, 0, 0x1), dx.src(dx.OPERAND_INPUT, 1, (0, 0, 0, 0)))
    code += dx.insn(ADD, dx.dest(dx.OPERAND_OUTPUT, 0), dx.src(dx.OPERAND_INPUT, 0),
                    dx.src_cb_relative(0, 0, 0))
    code += dx.insn(ADD, dx.dest(dx.OPERAND_OUTPUT, 1), dx.src(dx.OPERAND_INPUT, 0), dx.src(dx.OPERAND_INPUT, 0))
    code += dx.insn(RET)
    return dx.unity_program([dx.signature_chunk(inputs, b"ISGN"), dx.signature_chunk(outputs, b"OSGN"),
                             dx.shex_chunk([code], stage=1)])


instanced_params = mkshader2021.inline_parameter_blob(
    constant_buffers=[("UnityInstancing_Props", 32, [], [])], bindings=[("UnityInstancing_Props", 0)],
    struct_arrays={"UnityInstancing_Props": (0, 2, 16)})
instanced_body, _ = pc_shader_2021(
    "Notes/Instanced", [(_instanced_vs(), [])], [(_ps(spi=False), [])], keyword_names=(),
    vertex_params=[instanced_params], fragment_params_blobs=[mkshader2021.inline_parameter_blob()])


def expect_runtime_instancing_size(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    _, refs, entries = inspect_converted(dst)
    code = entries[int(next(r for r in refs if r["stage"] == "0")["blob"])]["code"]
    if "vec4 vivify_cb_UnityInstancing_Props[1 * UNITY_RUNTIME_INSTANCING_ARRAY_SIZE]" not in code:
        return f"the per-instance array is not sized at run time: {code}"
    if "#ifndef UNITY_RUNTIME_INSTANCING_ARRAY_SIZE\\n#define UNITY_RUNTIME_INSTANCING_ARRAY_SIZE 2" not in code:
        return "no fallback definition of UNITY_RUNTIME_INSTANCING_ARRAY_SIZE"
    return None


pc_shader_case("an instanced array is sized by UNITY_RUNTIME_INSTANCING_ARRAY_SIZE, not the placeholder 2",
               instanced_body, expect_runtime_instancing_size)

# 0.15.4: a single-pass instanced vertex stage that reads the eye from the
# instance ID without writing the layer itself (a geometry stage does that).
# The translator rebuilds SPI's instance numbering for it only when told the
# program is SPI code, and the converter never told it: both views took eye 0.
spi_eye_body, _ = pc_shader_2021(
    "Grass/SpiEyeFromInstance",
    [(_instanced_vs(), []), (_instanced_vs(), [0])],
    [(_ps(spi=False), []), (_ps(spi=False), [0])],
    vertex_params=[instanced_params, instanced_params],
    fragment_params_blobs=[mkshader2021.inline_parameter_blob(), mkshader2021.inline_parameter_blob()])


def expect_spi_instance_remap(proc, fields, refusals, dst):
    if fields.get("linked") != "1" or fields.get("variantsRefused") != "0":
        return f"linked={fields.get('linked')} variantsRefused={fields.get('variantsRefused')} {refusals}"
    _, refs, entries = inspect_converted(dst)
    for ref in refs:
        if ref["stage"] != "0":
            continue
        code = entries[int(ref["blob"])]["code"]
        if "gl_InstanceID * 2 + int(gl_ViewID_OVR)" not in code:
            return f"vertex variant '{ref['keywords']}' takes every view as eye 0: {code}"
    return None


pc_shader_case("an SPI vertex stage that leaves the layer to a geometry stage still takes its eye from the view",
               spi_eye_body, expect_spi_instance_remap)

broken_body, _ = pc_shader_2021("Custom/Broken", [(_vs(spi=False), [])],
                                [(dxbc_untranslatable(), [])], keyword_names=())


def expect_refused_linked(proc, fields, refusals, dst):
    if fields.get("refused") != "1" or fields.get("linked") != "0":
        return f"refused={fields.get('refused')} linked={fields.get('linked')}"
    if not refusals:
        return "no refusal reason was reported"
    if os.path.exists(dst):
        platforms, refs, _ = inspect_converted(dst)
        if platforms != "4" or any(r["type"] == str(GLES3_TYPE) for r in refs):
            return "a shader that could not link was relabelled anyway"
    return None


pc_shader_case("a shader whose fragment will not translate is left as it was",
               broken_body, expect_refused_linked)

cases += 1
p = subprocess.run([CONV, "--shaders", bad, os.path.join(TMP, "sh_bad.out")],
                   capture_output=True, text=True)
if p.returncode != 0 and not os.path.exists(os.path.join(TMP, "sh_bad.out")):
    print("ok   shader conversion rejects a non-bundle without leaving output")
else:
    print("FAIL shader conversion accepted a non-bundle:\n" + p.stdout); fails += 1

# A Quest bundle can ship a shader with nothing in it: the mapper's Unity failed
# to compile it for Android (Hold My Hand's raymarched kaleidoscope). --merge
# puts the translated PC build of that shader into the Quest bundle, under the
# Quest shader's own path ID, so the materials that use it get a working one.
def merge_case(quest_dependencies, expect_reference, expect_note, donor_mode="--shaders"):
    global fails, cases
    cases += 1
    name = "Custom/PoofShaders/Kaleidoscope"
    donor_src = os.path.join(TMP, "merge_pc.vivify")
    donor = os.path.join(TMP, "merge_pc_converted.vivify")
    quest = os.path.join(TMP, "merge_quest.vivify")
    merged = os.path.join(TMP, "merge_out.vivify")
    # The PC shader names a dependency in its own file: it must not survive
    # into the Quest bundle, where path 4242 is something else or nothing.
    pc_body, _ = pc_shader_2021(name, [(_vs(spi=False), []), (_vs(spi=True), [0])],
                                [(_ps(spi=False), []), (_ps(spi=True), [0])], dependencies=[(0, 4242)])
    pc_sf = mkshader.serialized_file_with_shaders([pc_body], sf_version=22,
                                                  shader_tree=mkshader2021.RealTypeTree())
    build(donor_src, sf_bytes=[pc_sf], with_resource=False)
    proc, fields, refusals = run_shaders(donor_src, donor, donor_mode)
    if fields.get("linked") != "1":
        print(f"FAIL merge: the PC donor did not convert: {fields} {refusals}")
        fails += 1
        return
    # The Quest bundle: the same shader with an empty store for GLES3, and a
    # dependency of its own.
    empty_store = mkshader.build_program_store([[b"\x00\x00\x00\x00"]])
    quest_body = mkshader2021.shader_body(name, [9], empty_store, [], dependencies=quest_dependencies)
    quest_sf = mkshader.serialized_file_with_shaders([quest_body], sf_version=22, target=13,
                                                     shader_tree=mkshader2021.RealTypeTree())
    build(quest, sf_bytes=[quest_sf], with_resource=False)
    if os.path.exists(merged):
        os.remove(merged)
    proc = subprocess.run([CONV, "--merge", quest, donor, merged, name, "Custom/NotThere"],
                          capture_output=True, text=True)
    out = proc.stdout
    problem = None
    if proc.returncode != 0 or "merged=1" not in out:
        problem = "merge did not succeed"
    elif "skipped=Custom/NotThere: not in the Quest bundle" not in out:
        problem = "the missing shader was not reported"
    else:
        platforms, refs, entries = inspect_converted(merged)
        if platforms is None or "9" not in platforms.split(","):
            problem = f"merged shader platforms are {platforms}"
        elif not all("num_views" in entries[int(r["blob"])]["code"] for r in refs if r["stage"] == "0"):
            problem = "a merged vertex variant is not a multiview program; the headset would draw nothing"
    if problem is None:
        # The dependency in the donor body is cleared: the Quest file's
        # m_Dependencies entry reads back as a null reference.
        _, _, _, nodes, data = read_converted(merged)
        off, size, _, _ = nodes[0]
        body = data[off:off + size]
        if struct.pack("<iq", 0, 4242) in body:
            problem = "the PC shader's dependency was carried into the Quest bundle"
        elif struct.pack("<ii", 1, *expect_reference[:1]) + struct.pack("<q", expect_reference[1]) not in body:
            problem = f"the dependency list does not read as one reference to {expect_reference}"
        elif expect_note != ("note=" in out):
            problem = "a cleared reference was not noted" if expect_note else "an unexpected note"
    if problem:
        print(f"FAIL merge: {problem}\n{out}{proc.stderr}")
        fails += 1
    else:
        print("ok   an empty Quest shader is replaced by its translated PC build"
              + (", references cleared" if expect_note else ", keeping its own references"))


# The Quest shader's own reference replaces the PC one (same count).
merge_case([(1, 7)], (1, 7), False)
# Counts differ: the reference is cleared, and the log says so.
merge_case([], (0, 0), True)


# A donor converted with the stereo split has single-view plain variants,
# which drew nothing when merged (0.14.8). It is refused; the mod converts
# its donors unsplit.
def merge_split_donor_refused():
    global fails, cases
    cases += 1
    name = "Custom/PoofShaders/Kaleidoscope"
    donor = os.path.join(TMP, "merge_pc_split.vivify")
    run_shaders(os.path.join(TMP, "merge_pc.vivify"), donor, "--shaders-split")
    merged = os.path.join(TMP, "merge_split_out.vivify")
    if os.path.exists(merged):
        os.remove(merged)
    proc = subprocess.run([CONV, "--merge", os.path.join(TMP, "merge_quest.vivify"), donor, merged, name],
                          capture_output=True, text=True)
    if proc.returncode == 0 or "merged=0" not in proc.stdout or "stereo split" not in proc.stdout \
            or os.path.exists(merged):
        print(f"FAIL merge: a split donor was not refused\n{proc.stdout}{proc.stderr}")
        fails += 1
    else:
        print("ok   a donor converted with the stereo split is refused")


merge_split_donor_refused()

if GLSLANG is None:
    print("note: glslangValidator not installed; linked programs were not compiled")
def scan_depth_case(name, body, expect_depth, convert_first=False):
    """--scan reports whether any program names _CameraDepthTexture, which
    decides whether the runtime renders the main camera's depth pre-pass."""
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "scan_src.vivify")
    dst = os.path.join(TMP, "scan_dst.vivify")
    sf = mkshader.serialized_file_with_shaders([body], sf_version=22, shader_tree=mkshader2021.RealTypeTree())
    build(src, sf_bytes=[sf], with_resource=False)
    target = src
    if convert_first:
        run_shaders(src, dst)
        target = dst
    proc = subprocess.run([CONV, "--scan", target], capture_output=True, text=True)
    got = dict(line.split("=", 1) for line in proc.stdout.splitlines() if "=" in line and
               not line.startswith("depthShader="))
    problem = None
    if proc.returncode != 0 or got.get("parsed") != "1":
        problem = f"scan failed (exit {proc.returncode})"
    elif got.get("samplesCameraDepth") != ("1" if expect_depth else "0"):
        problem = f"samplesCameraDepth={got.get('samplesCameraDepth')}, expected {int(expect_depth)}"
    elif expect_depth and "depthShader=Swifter/DepthReader" not in proc.stdout:
        problem = "the depth-reading shader was not named"
    if problem:
        print(f"FAIL {name}: {problem}\n{proc.stdout}{proc.stderr}")
        fails += 1
    else:
        print(f"ok   {name}")


depth_body, _ = pc_shader_2021(
    "Swifter/DepthReader",
    [(_vs(spi=False), [])],
    [(_ps(spi=False), [])],
    keyword_names=(),
    fragment_params_blobs=[b"\x00\x00\x00\x00PARAMS _CameraDepthTexture sampler"])
scan_depth_case("a shader naming _CameraDepthTexture is reported as reading scene depth", depth_body, True)
scan_depth_case("it still is after conversion", depth_body, True, convert_first=True)
scan_depth_case("a shader that names no depth texture is not", stereo_body, False)

# Metadata-based native scene reuse must classify all geometry stages,
# including unused store entries and anonymous shaders, and reject legacy metadata.
def scan_stages_case(label, bodies, expected, complete=True, tree=None, tessellation=()):
    global fails, cases
    cases += 1
    path = os.path.join(TMP, "scan_stages.vivify")
    sf = mkshader.serialized_file_with_shaders(bodies, sf_version=22,
                    shader_tree=tree if tree is not None else mkshader2021.RealTypeTree())
    build(path, sf_bytes=[sf], with_resource=False)
    proc = subprocess.run([CONV, "--scan", path], capture_output=True, text=True)
    names = [line.split("=", 1)[1] for line in proc.stdout.splitlines() if line.startswith("separateEyeShader=")]
    tessellation_names = [line.split("=", 1)[1] for line in proc.stdout.splitlines() if line.startswith("tessellationShader=")]
    confidence = "sceneStageInfoComplete=" + str(int(complete))
    if proc.returncode or set(names) != set(expected) or set(tessellation_names) != set(tessellation) or confidence not in proc.stdout:
        print(f"FAIL {label}: {proc.stdout} {proc.stderr}"); fails += 1
    else: print(f"ok   {label}")
scan_stages_case("native stage scan classifies a vertex/fragment shader", [stereo_body], [])
scan_stages_case("native stage scan identifies geometry shader passes", [gs_pass_body], ["Swifter/GeometryPassthrough"])
# Use the existing shader and replace both length-prefixed names without
# changing store/program offsets; changing bytes at equal length keeps it valid.
# A mixed shader file must retain the geometry classification of just one shader.
scan_stages_case("native stage scan handles mixed geometry and ordinary shaders", [stereo_body, gs_pass_body], ["Swifter/GeometryPassthrough"])
legacy_store = [[mkshader.program_blob([mkshader.sub_program(20, b"placeholder geometry program")])]]
legacy_body = mkshader.shader_object("Legacy/Geometry", [4], platform_blobs=legacy_store)
scan_stages_case("legacy metadata cannot authorize native reuse, and its stored geometry is detected", [legacy_body], ["Legacy/Geometry"], False, mkshader.shader_type_tree(22))

# The 0.15.9 native-Quest scan metadata must coexist with the conservative
# stage classification used by our renderer. Legacy pass metadata stays unknown.
def scan_native_case(label, programs, expected):
    global cases, fails
    cases += 1
    path = os.path.join(TMP, "native_scan.vivify")
    store = [[mkshader.program_blob([mkshader.sub_program(GLES3_TYPE, code) for code in programs])]]
    body = mkshader.shader_object("Test/Native", [GLES3_PLATFORM], platform_blobs=store)
    sf = mkshader.serialized_file_with_shaders([body], sf_version=22, shader_tree=mkshader.shader_type_tree(22))
    build(path, sf_bytes=[sf], with_resource=False)
    proc = subprocess.run([CONV, "--scan", path], capture_output=True, text=True)
    actual = {line for line in proc.stdout.splitlines() if line.startswith(("singleViewShader=", "stereoDropsTextureShader=", "intUniform="))}
    if proc.returncode or actual != set(expected) or "sceneStageInfoComplete=0" not in proc.stdout:
        print(f"FAIL {label}: {proc.stdout} {proc.stderr}"); fails += 1
    else: print(f"ok   {label}")

scan_native_case("native mono metadata preserves int uniforms without authorizing native scene reuse",
    [b"uniform highp int _Mode;\nuniform int unity_Internal;\nuniform int _Array[2];\nuniform float _Float;\n"],
    ["singleViewShader=Test/Native", "intUniform=Test/Native:_Mode"])
scan_native_case("native stereo texture omissions survive the merged stage scan",
    [b"uniform sampler2D _Overlay;\n", b"#extension GL_OVR_multiview2 : require\n"],
    ["stereoDropsTextureShader=Test/Native"])
scan_native_case("matching mono and stereo textures do not request redundant per-eye blits",
    [b"uniform sampler2D _Overlay;\n", b"#extension GL_OVR_multiview2 : require\nuniform sampler2D _Overlay;\n"], [])

# A successful GLES link of VS/FS must not hide unconverted hull/domain stages.
# The retained stage payloads are deliberately opaque: translation must not
# silently relabel or remove them while reporting their missing coverage.
partial_entries = [
    (mkshader.sub_program(DX11_VERTEX_SM50, _vs(spi=False)), 0),
    (mkshader2021.inline_parameter_blob(), 0),
    (mkshader.sub_program(DX11_PIXEL_SM50, _ps(spi=False)), 0),
    (mkshader2021.inline_parameter_blob(), 0),
    (mkshader.sub_program(21, b"original-hull-stage"), 0),
    (mkshader2021.inline_parameter_blob(), 0),
    (mkshader.sub_program(22, b"original-domain-stage"), 0),
    (mkshader2021.inline_parameter_blob(), 0),
]
partial_stages = {}
for stage, blob, program_type in [(mkshader2021.VERTEX,0,DX11_VERTEX_SM50),
                                 (mkshader2021.FRAGMENT,2,DX11_PIXEL_SM50),
                                 ("progHull",4,21), ("progDomain",6,22)]:
    partial_stages[stage] = {"player":[[mkshader2021.player_sub_program(blob,program_type)]],"params":[[blob+1]]}
partial_body = mkshader2021.shader_body("Test/PartialTessellation",[4],
                mkshader.build_program_store([mkshader.segmented_chunks(partial_entries)]),[partial_stages],())
scan_stages_case("tessellation stage presence is reported separately from geometry", [partial_body],
                 ["Test/PartialTessellation"], tessellation=["Test/PartialTessellation"])
legacy_tess_store = [[mkshader.program_blob([mkshader.sub_program(21,b"retained hull"),
                                            mkshader.sub_program(22,b"retained domain")])]]
legacy_tess_body = mkshader.shader_object("Legacy/Tessellation",[4],platform_blobs=legacy_tess_store)
scan_stages_case("legacy hull and domain stores cannot hide incomplete stage coverage", [legacy_tess_body],
                 ["Legacy/Tessellation"],False,mkshader.shader_type_tree(22),tessellation=["Legacy/Tessellation"])

def expect_partial_tessellation(proc, fields, refusals, dst):
    if fields.get("untranslatedTessellationShaders") != "1" or "retain untranslated tessellation" not in fields.get("message", ""):
        return "partial tessellation coverage was not reported: " + proc.stdout
    _, refs, entries = inspect_converted(dst)
    for stage, program_type, text in [("3","21","original-hull-stage"),("4","22","original-domain-stage")]:
        selected = [r for r in refs if r["stage"]==stage]
        if not selected or any(r["type"]!=program_type or entries[int(r["blob"])]["code"]!=text for r in selected):
            return "an untranslated tessellation stage was relabelled or modified"
    scanned = subprocess.run([CONV,"--scan",dst],capture_output=True,text=True)
    if scanned.returncode or "tessellationShader=Test/PartialTessellation" not in scanned.stdout:
        return "converted bundle lost its unsupported-stage classification: " + scanned.stdout
    return None

pc_shader_case("linked GLES programs retain and disclose unsupported tessellation stages",
               partial_body,expect_partial_tessellation)

# Shader specialization is a per-pass equivalence: every geometry variant
# must return without emitting on perspective views; ordinary passes are kept.
def _perspective_guard_geometry(valid=True):
    shape = {"name": "unity_OrthoParams", "offset": 0, "size": 16, "class": 1, "type": 3, "rows": 1, "columns": 4}
    reflection = dx.rdef_chunk(constant_buffers=[{"name": "UnityPerCamera", "size": 16, "variables": [shape]}],
        bindings=[{"name": "UnityPerCamera", "type": 0, "dimension": 0, "bind_point": 0}])
    code = dx.insn(DCL_GS_INPUT_PRIMITIVE_B, controls=3) + dx.insn(DCL_MAXOUT_B, extra=[3]) + dx.insn(DCL_GS_OUTPUT_TOPOLOGY_B, controls=5)
    code += dx.insn(104, extra=[1])
    code += dx.insn(57 if valid else 24, dx.dest(dx.OPERAND_TEMP, 0, 1), dx.src_cb(0, 0, (3,3,3,3)), dx.imm_float(1.0))
    code += dx.insn(31, dx.src(dx.OPERAND_TEMP, 0, (0,0,0,0)), controls=128) + dx.insn(RET) + dx.insn(21)
    code += dx.insn(MOV, dx.dest(dx.OPERAND_OUTPUT, 0), dx._operand(dx.OPERAND_INPUT, [0,0])) + dx.insn(EMIT_B) + dx.insn(RET)
    signature = dx.signature_chunk([{"name":"SV_Position","register":0,"sv":1}], b"OSGN")
    return dx.unity_program([reflection, dx.signature_chunk([{"name":"SV_Position","register":0,"sv":1}], b"ISGN"), signature, dx.shex_chunk([code], stage=2)])

def specialization_case(valid, ordinary_pass=False, mixed_guard=False):
    global fails, cases
    cases += 1
    body, entries = pc_shader_2021("Test/Perspective", [(_vs(spi=False),[])], [(_ps(spi=False),[])],
            keyword_names=(), geometry_programs=[(_perspective_guard_geometry(valid),[])])
    if mixed_guard:
        body, entries = pc_shader_2021("Test/Perspective", [(_vs(spi=False),[]),(_vs(spi=False),[0])],
             [(_ps(spi=False),[]),(_ps(spi=False),[0])], keyword_names=("ALTERNATE",),
             geometry_programs=[(_perspective_guard_geometry(True),[]),(_perspective_guard_geometry(False),[0])])
    if ordinary_pass:
        # Same vertex/fragment entries are deliberately shared by two passes:
        # their new linked sources differ, so conversion must clone store entries.
        stages = {mkshader2021.VERTEX:{"player":[[mkshader2021.player_sub_program(0,16)]],"params":[[1]]},
                  mkshader2021.FRAGMENT:{"player":[[mkshader2021.player_sub_program(2,18)]],"params":[[3]]}}
        with_geometry = dict(stages)
        with_geometry[mkshader2021.GEOMETRY] = {"player":[[mkshader2021.player_sub_program(4,20)]],"params":[[5]]}
        store = mkshader.build_program_store([mkshader.segmented_chunks(entries)])
        body = mkshader2021.shader_body("Test/Perspective",[4],store,[with_geometry,stages],())
    path = os.path.join(TMP,"specialize_src.vivify"); dest = os.path.join(TMP,"specialize_dst.vivify")
    sf = mkshader.serialized_file_with_shaders([body],sf_version=22,shader_tree=mkshader2021.RealTypeTree())
    build(path,sf_bytes=[sf],with_resource=False)
    proc, fields, refusals = run_shaders(path,dest,"--shaders-native-perspective-isolated")
    problem = None
    if fields.get("refused") != "0" or fields.get("variantsRefused") != "0":problem=str((fields,refusals))
    else:
        _, _, _, nodes, data = read_converted(dest); off,size,_,_=nodes[0]
        inspect_path = os.path.join(TMP,"specialize.sf");open(inspect_path,"wb").write(data[off:off+size])
        inspected = subprocess.run([CONV,"--inspect",inspect_path],capture_output=True,text=True,check=True).stdout
        proved = valid and not mixed_guard
        prefix = "__VivifyNativePerspective__/" if proved else "__VivifyNativeOther__/"
        if "shader="+prefix+"Test/Perspective" not in inspected:problem="wrong proof classification: "+inspected[:300]
        _, refs, programs = inspect_converted(dest)
        code = "\n".join(e["code"] for e in programs.values())
        if proved:
            if "layout(num_views = 2)" not in code or "gl_Position = vec4(2.0, 2.0, 2.0, 1.0)" not in code:problem="native empty pass missing"
            if any(r["type"] != "20" for r in refs if r["stage"]=="2"):problem="geometry was retained in the native program"
            if ordinary_pass:
                vertex_codes = [programs[int(r["blob"])]["code"] for r in refs if r["stage"]=="0"]
                if len(set(vertex_codes)) != 2 or not all("num_views" in c for c in vertex_codes):problem="ordinary pass was not preserved separately in native multiview"
        elif "#ifdef GEOMETRY" not in code:problem="unproved geometry was erased"
    if problem:print(f"FAIL specialization valid={valid} mixed={ordinary_pass}: {problem}");fails+=1
    else:print(f"ok   perspective specialization valid={valid} mixed={ordinary_pass}")
specialization_case(True)
specialization_case(False)
specialization_case(True,True)
specialization_case(True,mixed_guard=True)

def expect_explicit_stereo_uniforms(proc, fields, refusals, dst):
    if fields.get("refused") != "0" or fields.get("variantsRefused") != "0":return str((fields,refusals))
    _, refs, entries = inspect_converted(dst)
    code = "\n".join(e["code"] for e in entries.values() if e["type"] in ("3","4","5","6"))
    if "uniform UnityStereoGlobals" in code or "vivify_cb_UnityStereoGlobals" in code:return "explicit eyes still depend on XR's stereo UBO"
    if "uniform vec4 hlslcc_mtx4x4unity_StereoMatrixVP[8];" not in code:return "material-addressable stereo matrix array missing"
    return None
for side in ("left","right"):
    pc_shader_case(f"{side} explicit eye receives named matrices rather than the native XR block",
                   inline_body,expect_explicit_stereo_uniforms,mode=f"--shaders-eye-{side}-isolated")

def expect_stereo_binding_analysis(proc, fields, refusals, dst):
    problem = expect_explicit_stereo_uniforms(proc, fields, refusals, dst)
    if problem:
        return problem
    before = open(dst, "rb").read()
    scan = subprocess.run([CONV, "--scan", dst], capture_output=True, text=True)
    masks = [int(line.rsplit(":", 1)[1]) for line in scan.stdout.splitlines()
             if line.startswith("stereoBindings=")]
    if scan.returncode or masks != [1 << 2]:
        return "expected only the active VP matrix binding across all GLES variants: " + scan.stdout
    if open(dst, "rb").read() != before:
        return "read-only binding analysis changed shader bytes"
    return None

pc_shader_case("GLES binding analysis ignores retained DX11 entries and preserves the live stereo matrix",
               inline_body, expect_stereo_binding_analysis, mode="--shaders-eye-left-isolated")

# Immutable material feature specialization must be authorized AND proved.
# These are authored synthetic records, not private map assets.
_fixed_flags = {name: (1.0 if name == "_MatCapMaskEnable" else 0.0) for name in (
    "_DecalEnable", "_EmissionEnable", "_FurEnable", "_LimitterEnable", "_MatCapMaskEnable",
    "_OutLineEnable", "_ParallaxEnable", "_ReflectionEnable", "_RimLitEnable", "_SubTexEnable",
    "_ToonEnable", "_ToonGlossEnable")}

def _material_record(values=None, file_id=0, duplicate=False, color_shadow=None):
    t = mkshader.TypeTree().add(0, "Material", "Base", -1)
    t.add(1, "PPtr<Shader>", "m_Shader", 12).add(2, "int", "m_FileID", 4).add(2, "SInt64", "m_PathID", 8)
    t.add(1, "SavedProperties", "m_SavedProperties", -1).add(2, "map", "m_Floats", -1)
    t.add(3, "Array", "Array", -1, is_array=True).add(4, "int", "size", 4).add(4, "pair", "data", -1)
    t.add(5, "string", "first", -1, align=True).add(6, "Array", "Array", -1, is_array=True, align=True)
    t.add(7, "int", "size", 4).add(7, "char", "data", 1).add(5, "float", "second", 4)
    values = list((_fixed_flags if values is None else values).items())
    if duplicate: values.append(values[0])
    body = bytearray(struct.pack("<iqI", file_id, 1, len(values)))
    for name, value in values:
        text = name.encode(); body.extend(struct.pack("<I", len(text)) + text)
        mkshader._align4(body); body.extend(struct.pack("<f", value))
    if color_shadow:
        t.add(2, "map", "m_Colors", -1).add(3, "Array", "Array", -1, is_array=True)
        t.add(4, "int", "size", 4).add(4, "pair", "data", -1)
        t.add(5, "string", "first", -1, align=True).add(6, "Array", "Array", -1, is_array=True, align=True)
        t.add(7, "int", "size", 4).add(7, "char", "data", 1)
        t.add(5, "Color", "second", 16)
        for component in "rgba":t.add(6, "float", component, 4)
        name=color_shadow.encode();body.extend(struct.pack("<II", 1, len(name))+name)
        mkshader._align4(body);body.extend(struct.pack("<ffff",1,1,1,1))
    return (21, t, bytes(body))

def _clip_record(type_id=4, script_id=0, attribute=4, custom_type=0, event_count=0):
    t = mkshader.TypeTree().add(0, "AnimationClip", "Base", -1)
    for name in ("m_FloatCurves", "m_PPtrCurves", "m_Events"):
        t.add(1, "vector", name, -1).add(2, "Array", "Array", -1, is_array=True)
        t.add(3, "int", "size", 4).add(3, "int", "data", 4)
    t.add(1, "Bindings", "m_ClipBindingConstant", -1).add(2, "vector", "genericBindings", -1)
    t.add(3, "Array", "Array", -1, is_array=True).add(4, "int", "size", 4).add(4, "Binding", "data", -1)
    t.add(5, "int", "typeID", 4).add(5, "int", "isPPtrCurve", 4)
    t.add(5, "int", "attribute", 4).add(5, "int", "customType", 4).add(5, "PPtr", "script", 12)
    t.add(6, "int", "m_FileID", 4).add(6, "SInt64", "m_PathID", 8)
    t.add(2, "vector", "pptrCurveMapping", -1).add(3, "Array", "Array", -1, is_array=True)
    t.add(4, "int", "size", 4).add(4, "int", "data", 4)
    body = struct.pack("<III", 0, 0, event_count) + struct.pack("<i", 0) * event_count
    body += struct.pack("<IiiiiiqI", 1, type_id, 0, attribute, custom_type, 0, script_id, 0)
    return (74, t, body)

_fixed_body, _ = pc_shader_2021("Sunao Shader/Cutout", [(_vs(spi=False), [])], [(_ps(spi=False), [])], keyword_names=(), properties=[(name,2) for name in _fixed_flags])
def _fixed_case(name, records, expected, authorize=True, extra_class_id=None, other_file=False, shader_body=None):
    global cases, fails
    cases += 1
    src, dst = os.path.join(TMP, "fixed_src.vivify"), os.path.join(TMP, "fixed_dst.vivify")
    sf = mkshader.serialized_file_with_shaders([_fixed_body if shader_body is None else shader_body], sf_version=22,
        shader_tree=mkshader2021.RealTypeTree(), extra_records=records, extra_class_id=extra_class_id)
    files = [sf]
    if other_file: files.append(mkshader.serialized_file_with_shaders([], extra_class_id=114))
    build(src, sf_bytes=files, with_resource=False)
    env = dict(os.environ)
    if authorize: env["VIVIFY_IMMUTABLE_MATERIAL_FEATURES"] = "1"
    else: env.pop("VIVIFY_IMMUTABLE_MATERIAL_FEATURES", None)
    proc = subprocess.run([CONV, "--shaders", src, dst], capture_output=True, text=True, env=env)
    actual = re.search(r"^immutableMaterialShaders=(\d+)$", proc.stdout, re.M)
    good = proc.returncode == 0 and actual and int(actual[1]) == expected
    if good: print("ok   immutable material: " + name)
    else: print("FAIL immutable material: " + name + "\n" + proc.stdout + proc.stderr); fails += 1

_fixed_case("matching material and transform animation", [_material_record(), _clip_record()], 1)
_fixed_case("caller authorization required", [_material_record()], 0, authorize=False)
_fixed_case("no material evidence", [], 0)
_changed = dict(_fixed_flags, _EmissionEnable=1.0)
_fixed_case("changed material setting", [_material_record(_changed)], 0)
_fixed_case("every material must agree", [_material_record(), _material_record(_changed)], 0)
_missing = dict(_fixed_flags); _missing.pop("_ReflectionEnable")
_fixed_case("missing flag", [_material_record(_missing)], 0)
_fixed_case("duplicate property", [_material_record(duplicate=True)], 0)
_fixed_case("external shader reference", [_material_record(file_id=1)], 0)
_fixed_case("renderer animation", [_material_record(), _clip_record(type_id=23)], 0)
_fixed_case("scripted animation", [_material_record(), _clip_record(script_id=8)], 0)
_fixed_case("script component", [_material_record()], 0, extra_class_id=114)
_fixed_case("unknown component", [_material_record()], 0, extra_class_id=99999)
_fixed_case("unsafe second serialized file", [_material_record()], 0, other_file=True)
_bad_clip = _clip_record(); _bad_clip = (*_bad_clip[:2], _bad_clip[2][:-4])
_fixed_case("truncated clip", [_material_record(), _bad_clip], 0)
_bad_material = _material_record(); _bad_material = (*_bad_material[:2], _bad_material[2][:-4])
_fixed_case("truncated material", [_bad_material], 0)
_fixed_case("animation event callback", [_material_record(), _clip_record(event_count=1)], 0)
_fixed_case("unknown transform attribute", [_material_record(), _clip_record(attribute=900)], 0)
_fixed_case("unknown animation binding type", [_material_record(), _clip_record(custom_type=99)], 0)
_no_props, _ = pc_shader_2021("Sunao Shader/Cutout", [(_vs(spi=False), [])], [(_ps(spi=False), [])], keyword_names=())
_fixed_case("missing shader property definitions", [_material_record()], 0, shader_body=_no_props)
_wrong_props, _ = pc_shader_2021("Sunao Shader/Cutout", [(_vs(spi=False), [])], [(_ps(spi=False), [])], keyword_names=(), properties=[(name,5) for name in _fixed_flags])
_fixed_case("integer property layout is not a float profile", [_material_record()], 0, shader_body=_wrong_props)
_fixed_case("baked flag cannot have a second saved value type", [_material_record(color_shadow="_EmissionEnable")], 0)
_legacy_values = dict(_fixed_flags, _EmissionBlink=0.0)
_fixed_case("unrelated legacy saved properties remain intact", [_material_record(_legacy_values, color_shadow="_EmissionBlink")], 1)

def expect_both_isolated_names(name):
    def check(proc, fields, refusals, dst):
        if fields.get("refused") != "0":return str(refusals)
        _,_,_,_,data=read_converted(dst)
        text=("__VivifyMono__/"+name).encode("utf-8")
        if data.count(struct.pack("<I",len(text))+text)!=2:return "object and parsed-form shader names were not both isolated"
    return check
for authored_name in ("Test/DrosteObject\r\n", "Test/色のShader"):
    name_body,_=pc_shader_2021(authored_name,[(_vs(spi=False),[])],[(_ps(spi=False),[])],keyword_names=())
    pc_shader_case("typed shader name preserves and isolates "+repr(authored_name),name_body,expect_both_isolated_names(authored_name),mode="--shaders-mono-isolated")

def expect_bounded_isolated_name(proc,fields,refusals,dst):
    if fields.get("refused")!="1" or not any("isolated shader name is too long" in reason for reason in refusals):return "oversized isolated name was not refused"
long_name_body,_=pc_shader_2021("Test/"+"x"*65531,[(_vs(spi=False),[])],[(_ps(spi=False),[])],keyword_names=())
pc_shader_case("isolated shader names remain within the parser length bound",long_name_body,expect_bounded_isolated_name,mode="--shaders-mono-isolated")

# Animation-only materials/shaders are dependencies, rather than named bundle
# assets. Expose them without changing preload tables or authored references.
def _dependency_bundle_record(preload=(1, 2, 4), roots=None, malformed=False):
    roots = [("authored.prefab", 0, len(preload), 0, 4)] if roots is None else roots
    t = mkshader.TypeTree().add(0, "AssetBundle", "Base", -1)
    def text(level, name):
        t.add(level,"string",name,-1,align=True).add(level+1,"Array","Array",-1,is_array=True,align=True)
        t.add(level+2,"int","size",4).add(level+2,"char","data",1)
    def ptr(level, name):
        t.add(level,"PPtr<Object>",name,12).add(level+1,"int","m_FileID",4).add(level+1,"SInt64","m_PathID",8)
    text(1,"m_Name")
    t.add(1,"vector","m_PreloadTable",-1).add(2,"Array","Array",-1,is_array=True)
    t.add(3,"int","size",4);ptr(3,"data")
    t.add(1,"map","m_Container",-1).add(2,"Array","Array",-1,is_array=True)
    t.add(3,"int","size",4).add(3,"pair","data",-1);text(4,"first")
    t.add(4,"AssetInfo","second",-1).add(5,"int","preloadIndex",4).add(5,"int","preloadSize",4);ptr(5,"asset")
    t.add(1,"unsigned int","unchangedSentinel",4)
    body=bytearray()
    def string(value):
        b=value.encode();body.extend(struct.pack("<I",len(b))+b);mkshader._align4(body)
    string("dependency-fixture")
    body.extend(struct.pack("<I",len(preload)))
    for p in preload:body.extend(struct.pack("<iq",*(p if isinstance(p,tuple) else (0,p))))
    body.extend(struct.pack("<I",len(roots)))
    for key,index,count,file_id,path in roots:
        string(key);body.extend(struct.pack("<iiiq",index,count,file_id,path))
    body.extend(struct.pack("<I",0xaabbccdd))
    if malformed:body=body[:-1]
    return 142,t,bytes(body)

def dependency_case(label, *, enabled=True, sf_version=22, preload=(1,2,4), roots=None, malformed=False, rejected=False):
    global cases,fails
    cases+=1
    bundle=_dependency_bundle_record(preload,roots,malformed)
    dummy=(1,mkshader.TypeTree().add(0,"GameObject","Base",-1).add(1,"int","sentinel",4),struct.pack("<I",17))
    sf=mkshader.serialized_file_with_shaders([stereo_body],sf_version=sf_version,shader_tree=mkshader2021.RealTypeTree(),extra_records=[_material_record(),bundle,dummy])
    src=os.path.join(TMP,"dependency-src.vivify");dst=os.path.join(TMP,"dependency-dst.vivify")
    if os.path.exists(dst):os.remove(dst)
    build(src,sf_bytes=[sf],with_resource=False)
    env=dict(os.environ)
    if enabled:env["VIVIFY_EXPOSE_RENDER_DEPENDENCIES"]="1"
    else:env.pop("VIVIFY_EXPOSE_RENDER_DEPENDENCIES",None)
    p=subprocess.run([CONV,"--shaders",src,dst],env=env,text=True,capture_output=True)
    problem=None
    if rejected:
        if p.returncode == 0 or "render dependency lookup could not be safely extended" not in p.stdout or os.path.exists(dst):problem="unrecognized container was not refused: "+p.stdout
    elif p.returncode or "status=converted" not in p.stdout:problem=p.stdout+p.stderr
    else:
        _,_,_,nodes,data=read_converted(dst)
        original_roots=[("authored.prefab",0,len(preload),0,4)] if roots is None else list(roots)
        existing={path for _,_,_,file_id,path in original_roots if file_id==0}
        ranges={}
        for _,index,count,_,_ in original_roots:
            for pointer in preload[index:index+count]:
                file_id,path=(0,pointer) if isinstance(pointer,int) else pointer
                if file_id==0 and (path not in ranges or count<ranges[path][1]):ranges[path]=(index,count)
        expected_roots=original_roots+[("assets/__vivify_internal__/render/"+str(path),*ranges[path],0,path) for path in (1,2) if enabled and path in ranges]
        expected=_dependency_bundle_record(preload,expected_roots)[2]
        if data.count(expected)!=1:problem="bundle container/preload/trailer differs from the expected preserved record"
        if data.count(dummy[2])<1:problem="unrelated object was lost"
    if problem:print("FAIL "+label+": "+problem);fails+=1
    else:
        print("ok   "+label)
        if enabled and not rejected:shutil.copyfile(src,os.path.join(TMP,"fuzz_seed_dependencies.vivify"))

dependency_case("private lookup exposes animation-only material and translated shader")
dependency_case("dependency lookup leaves ordinary conversion unchanged when disabled",enabled=False)
dependency_case("dependency lookup supports serialized file version 21",sf_version=21)
dependency_case("dependency lookup preserves existing root assets",roots=[("shader.shader",0,1,0,1),("authored.prefab",2,1,0,4)])
dependency_case("dependency lookup preserves the smallest complete authored preload range",roots=[("scene",0,3,0,4),("material.mat",0,2,0,2)])
dependency_case("dependency lookup ignores unused preload records",roots=[("authored.prefab",2,1,0,4)])
dependency_case("dependency lookup excludes external preload references",preload=(1,(1,2),4))
dependency_case("dependency lookup excludes unreachable local objects",preload=(1,4))
dependency_case("dependency lookup refuses invalid preload ranges",roots=[("bad.prefab",8,1,0,4)],rejected=True)
dependency_case("dependency lookup preserves duplicate authored sub-asset keys",roots=[("same",0,1,0,1),("same",1,1,0,2)])
dependency_case("dependency lookup refuses reserved-key collisions",roots=[("assets/__vivify_internal__/authored",0,1,0,1)],rejected=True)
dependency_case("dependency lookup refuses truncated container record",malformed=True,rejected=True)

print(f"\n{cases - fails}/{cases} passed")
sys.exit(1 if fails else 0)
