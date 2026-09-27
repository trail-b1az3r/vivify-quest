#!/usr/bin/env python3
"""Rebuilds assets/audiolink/AudioLink.bundle from Aeroluna's PC bundle.

    python3 tools/audiolink/build_bundle.py <BSAudioLink>/AudioLink/Assets/Bundle2021 [conv]

<BSAudioLink> is a checkout of github.com/Aeroluna/BSAudioLink (MIT); the
bundle holds AudioLink 3.1.2's analysis shader, material and
CustomRenderTexture, built for PC Beat Saber 1.40 (Unity 2021.3.16). Two steps:

1. The mod's own converter (tools/bundleconvert, built from src/) translates
   the shader's DirectX programs to GLES, with separate stereo variants
   (--shaders-split): the CustomRenderTexture draws single-view, and GL
   refuses a multiview program in a single-view framebuffer.
2. The CustomRenderTexture's m_UpdateMode is set to Realtime (1). The PC mod
   switches it from code (it ships OnDemand); this game build strips
   CustomRenderTexture's scripting API, so the asset has to say it itself --
   which is also how RedBrumbler's Quest port shipped it.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "assets", "audiolink", "AudioLink.bundle")
MATERIAL_PATH_ID = 4111427781930821263  # mat_AudioLink, referenced by rt_AudioLink


def build_conv():
    out = os.path.join(tempfile.mkdtemp(prefix="vivify-conv-"), "conv")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-I", os.path.join(ROOT, "include"),
                    "-o", out, os.path.join(ROOT, "tools", "bundleconvert", "main.cpp"),
                    os.path.join(ROOT, "src", "VivifyBundleConvert.cpp"),
                    os.path.join(ROOT, "src", "VivifySerializedFile.cpp"),
                    os.path.join(ROOT, "src", "VivifyDxbc.cpp")], check=True)
    return out


def main():
    source = sys.argv[1]
    conv = sys.argv[2] if len(sys.argv) > 2 else build_conv()
    converted = os.path.join(tempfile.mkdtemp(), "AudioLink.bundle")
    proc = subprocess.run([conv, "--shaders-split", source, converted], capture_output=True, text=True)
    print(proc.stdout, end="")
    if proc.returncode != 0 or "translated=1" not in proc.stdout or "variantsRefused=0" not in proc.stdout:
        sys.exit("conversion did not translate the AudioLink shader completely")

    data = bytearray(open(converted, "rb").read())
    # rt_AudioLink's fields after m_Material: m_InitSource (int), m_InitMaterial
    # (PPtr, 12 bytes), m_InitColor (16), m_InitTexture (PPtr, 12), then
    # m_UpdateMode. The material's path ID also appears in the object table,
    # so the needle is the PPtr followed by rt_AudioLink's empty m_InitSource
    # and m_InitMaterial, which is unique in the (uncompressed) output.
    pptr = struct.pack("<iq", 0, MATERIAL_PATH_ID) + bytes(4 + 12)
    at = data.find(pptr)
    if at < 0 or data.find(pptr, at + 1) >= 0:
        sys.exit("could not find rt_AudioLink's material reference exactly once")
    mode_at = at + 12 + 4 + 12 + 16 + 12
    mode = struct.unpack_from("<i", data, mode_at)[0]
    if mode not in (1, 2):
        sys.exit(f"unexpected m_UpdateMode {mode} at {mode_at}")
    struct.pack_into("<i", data, mode_at, 1)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "wb").write(data)
    print(f"wrote {OUT} ({len(data)} bytes; m_UpdateMode {mode} -> 1)")


if __name__ == "__main__":
    main()
