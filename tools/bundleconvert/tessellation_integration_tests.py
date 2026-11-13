"""Opt-in converter validation against supplied map artifacts; no device or drawing.

Requires UnityPy only for independent serialized metadata mutation/inspection,
and glslangValidator for five-stage links. No Unity SDK/editor is required.
Usage: python tessellation_integration_tests.py CONV YOI LIFELIKE OUTPUT_DIR [SAVED_PIPELINES]
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys

import UnityPy
import lz4.block
from mkbundle import read_converted

conv, yoi, lifelike, destination = map(Path, sys.argv[1:5])
destination.mkdir(parents=True, exist_ok=True)
conv = conv.resolve()
checks = 0
original_hash = hashlib.sha256(yoi.read_bytes()).hexdigest()
env = dict(os.environ, VIVIFY_SKIP_UNUSED="1", VIVIFY_EXPOSE_RENDER_DEPENDENCIES="1")


def check(condition, why):
    global checks
    assert condition, why
    checks += 1


def convert(source, target, mode, success):
    target.unlink(missing_ok=True)
    process = subprocess.run([str(conv), mode, str(source), str(target)], env=env, text=True, capture_output=True)
    (destination / (target.stem + ".log")).write_text(process.stdout + process.stderr)
    fields = dict(re.findall(r"(?m)^([A-Za-z]+)=([^\n]*)", process.stdout))
    if success:
        check(process.returncode == 0 and target.exists(), process.stdout + process.stderr)
        check(fields.get("untranslatedTessellationShaders") == "0", fields)
        check(fields.get("translatedTessellationShaders") == "1", fields)
        check(fields.get("translatedTessellationShader") == "Roystan/Grass", "named output coverage missing")
        check("refused=0" in process.stdout and "variantsRefused=0" in process.stdout, process.stdout)
    else:
        check(process.returncode != 0, "unsupported donor was accepted: " + process.stdout)
        check(not target.exists(), "a partial rejected donor was emitted")
        check(fields.get("translatedTessellationShaders") == "0", fields)
    return process


def grass_tree(source):
    environment = UnityPy.load(str(source))
    for obj in environment.objects:
        if obj.type.name == "Shader":
            tree = obj.read_typetree()
            if tree["m_ParsedForm"]["m_Name"].endswith("Roystan/Grass"):
                return environment, obj, tree
    raise AssertionError("grass shader absent")


stages = {"VERTEX": "vert", "FRAGMENT": "frag", "GEOMETRY": "geom", "HULL": "tesc", "DOMAIN": "tese"}
# Unity 2019's inspector refs have no shader-level keyword table. Classify
# reachable variants independently from the ORIGINAL program-store strings,
# before the converter remaps plain entries onto their stereo twins.
_, _, original_tree = grass_tree(yoi)
subshaders = original_tree["m_ParsedForm"]["m_SubShaders"]
check(len(subshaders) == 1 and len(subshaders[0]["m_Passes"]) == 1, "test requires the verified one-pass Yoi fixture")
blob = bytes(original_tree["compressedBlob"])
chunks = [lz4.block.decompress(blob[o:o + c], uncompressed_size=d) for o, c, d in
          zip(original_tree["offsets"][0], original_tree["compressedLengths"][0], original_tree["decompressedLengths"][0])]
count = struct.unpack_from("<I", chunks[0])[0]
stride = 12 if struct.unpack_from("<I", chunks[0], 4)[0] >= 4 + 12 * count else 8
keyword_context = {}
for stage, key in enumerate(("progVertex", "progFragment", "progGeometry", "progHull", "progDomain")):
    for index, ref in enumerate(subshaders[0]["m_Passes"][0][key]["m_SubPrograms"]):
        entry = struct.unpack_from("<" + "I" * (stride // 4), chunks[0], 4 + ref["m_BlobIndex"] * stride)
        body = chunks[entry[2] if stride == 12 else 0][entry[0]:entry[0] + entry[1]]
        version = struct.unpack_from("<I", body)[0]
        at = 24 if version >= 201608170 else 20
        names = []
        for table in range(2 if 201806140 <= version < 202012090 else 1):
            number = struct.unpack_from("<I", body, at)[0]; at += 4
            for _ in range(number):
                size = struct.unpack_from("<I", body, at)[0]; at += 4
                names.append(body[at:at + size].decode()); at = (at + size + 3) & ~3
        keyword_context[str(stage), str(index)] = names
artifacts = []
for eye in ("left", "right"):
    target = destination / ("yoi-triangle-tess-" + eye + "-validation.vivify")
    convert(yoi, target, "--shaders-tess-eye-" + eye + "-isolated", True)
    _, _, _, nodes, data = read_converted(str(target))
    sources = set()
    refs = []
    for offset, size, flags, name in nodes:
        if not flags & 4:
            continue
        serialized = destination / (eye + "-serialized")
        serialized.write_bytes(data[offset:offset + size])
        inspect = subprocess.run([str(conv), "--inspect", str(serialized)], text=True, capture_output=True, check=True)
        (destination / (eye + "-inspect.log")).write_text(inspect.stdout)
        grass = False
        for line in inspect.stdout.splitlines():
            if line.startswith("shader="):
                grass = "Roystan/Grass" in line
            if grass and line.startswith("ref "):
                refs.append(dict(re.findall(r"(\w+)=([^ ]*)", line)))
            if grass and line.startswith("entry ") and "type=2 " in line and " code=" in line:
                code = line.split(" code=", 1)[1].replace("\\n", "\n")
                if "#ifdef HULL" in code:
                    sources.add(code)
    selected = [r for r in refs if not any(k in keyword_context[r["stage"], r["index"]] for k in
                ("STEREO_INSTANCING_ON", "UNITY_SINGLE_PASS_STEREO", "INSTANCING_ON", "DOTS_INSTANCING_ON", "PROCEDURAL_INSTANCING_ON"))]
    check(bool(selected), "no selected grass refs")
    check(all(r["type"] == "2" for r in selected), "not all five selected stages retagged GLES3.1AEP")
    check({r["stage"] for r in selected} == set("01234"), "stage reference lost")
    check(bool(sources), "no five-stage source")
    for index, code in enumerate(sorted(sources)):
        parts = dict(re.findall(r"(?ms)^#ifdef (VERTEX|FRAGMENT|GEOMETRY|HULL|DOMAIN)\n(.*?)^#endif\n", code))
        check(set(parts) == set(stages), "incomplete combined source")
        check("layout(vertices = 3) out;" in parts["HULL"], "patch output count changed")
        check("uniform highp float _TessellationUniform;" in parts["HULL"], "factor not reflected material scalar")
        check("layout(triangles, equal_spacing, cw) in;" in parts["DOMAIN"], "tessellator layout changed")
        check("num_views" not in code and "GL_OVR_multiview" not in code, "multiview entered donor")
        paths = []
        for stage, body in parts.items():
            path = destination / ("yoi-" + eye + "-" + str(index) + "." + stages[stage])
            path.write_text(body)
            paths.append(str(path))
        link = subprocess.run(["glslangValidator", "-l", *paths], text=True, capture_output=True)
        (destination / ("yoi-" + eye + "-" + str(index) + "-link.log")).write_text(link.stdout + link.stderr)
        check(link.returncode == 0, link.stdout + link.stderr)
    _, _, tree = grass_tree(target)
    check(tree["m_ParsedForm"]["m_Name"].startswith("__VivifyEye"), "shader namespace lost")
    # Existing program parameter tables remain serialized, with the actual
    # scalar address/name. Unity's runtime binding still needs device proof.
    for sub in tree["m_ParsedForm"]["m_SubShaders"]:
        for shader_pass in sub["m_Passes"]:
            names = {index: name for name, index in shader_pass["m_NameIndices"]}
            for ref in shader_pass["progHull"]["m_SubPrograms"]:
                if ref["m_GpuProgramType"] != 2:
                    continue
                matches = [(names[v["m_NameIndex"]], v["m_Index"], v["m_Dim"]) for b in ref["m_ConstantBuffers"]
                           for v in b["m_VectorParams"] if names.get(v["m_NameIndex"]) == "_TessellationUniform"]
                check(matches == [("_TessellationUniform", 32, 1)], "factor reflection changed")
    artifacts.append(dict(path=str(target.resolve()), sha256=hashlib.sha256(target.read_bytes()).hexdigest(), five_stage_pipelines=len(sources)))


# Independently reserialize valid-but-ineligible metadata. Each rejection must
# happen before an output bundle appears; no partial VS/GS output is allowed.
for mutation in ("missing-domain", "different-tier", "duplicate-hull", "wrong-factor-address"):
    environment, obj, tree = grass_tree(yoi)
    for sub in tree["m_ParsedForm"]["m_SubShaders"]:
        for shader_pass in sub["m_Passes"]:
            hull = shader_pass["progHull"]["m_SubPrograms"]
            if mutation == "missing-domain":
                shader_pass["progDomain"]["m_SubPrograms"] = []
            elif mutation == "different-tier":
                for ref in hull:
                    ref["m_ShaderHardwareTier"] = 2
            elif mutation == "duplicate-hull":
                shader_pass["progHull"]["m_SubPrograms"] += copy.deepcopy(hull)
            else:
                names = {index: name for name, index in shader_pass["m_NameIndices"]}
                for ref in hull:
                    for buffer in ref["m_ConstantBuffers"]:
                        for parameter in buffer["m_VectorParams"]:
                            if names.get(parameter["m_NameIndex"]) == "_TessellationUniform":
                                parameter["m_Index"] += 4
    obj.save_typetree(tree)
    source = destination / (mutation + ".vivify")
    source.write_bytes(environment.file.save())
    process = convert(source, destination / (mutation + "-rejected.vivify"), "--shaders-tess-eye-left-isolated", False)
    check("refused atomically" in process.stdout, process.stdout)

convert(lifelike, destination / "lifelike-rejected-validation.vivify", "--shaders-tess-eye-left-isolated", False)

for flag in ("VIVIFY_DROP_GEOMETRY", "VIVIFY_DROP_ALL_GEOMETRY"):
    env[flag] = "1"
    convert(yoi, destination / (flag.lower() + "-rejected.vivify"), "--shaders-tess-eye-left-isolated", False)
    del env[flag]

if len(sys.argv) > 5:
    saved = Path(sys.argv[5])
    for index in range(6):
        paths = [str(saved / ("yoi-grass-" + str(index) + "." + ext)) for ext in ("vert", "tesc", "tese", "geom", "frag")]
        link = subprocess.run(["glslangValidator", "-l", *paths], text=True, capture_output=True)
        (destination / ("saved-pipeline-" + str(index) + "-link.log")).write_text(link.stdout + link.stderr)
        check(link.returncode == 0, link.stdout + link.stderr)

check(hashlib.sha256(yoi.read_bytes()).hexdigest() == original_hash, "source map modified")
manifest = dict(checks=checks, source_sha256=original_hash, artifacts=artifacts,
                note="Host serialization and GLSL links only; Unity factor binding, patch draws, imagery and performance remain unverified.")
(destination / "tessellation-validation.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
