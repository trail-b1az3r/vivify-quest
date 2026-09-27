#!/usr/bin/env python3
"""Regenerates fixtures/shader_2021_3_16_typetree.json from UnityPy's type-tree
package. Only needed if the fixture has to change; the tests read the JSON.

  pip install UnityPy && python3 tools/bundleconvert/dump_shader_typetree.py
"""
import json
import os

from UnityPy.helpers.Tpk import get_common_strings, get_typetree_node
from UnityPy.helpers.UnityVersion import UnityVersion

HERE = os.path.dirname(os.path.abspath(__file__))

node = get_typetree_node(48, UnityVersion.from_str("2021.3.16f1"))
flat = []


def walk(n, level):
    # The package does not carry typeFlags; a real bundle sets bit 0 (IsArray) on
    # every Array node, which is how a reader tells [size, data] apart.
    flags = 1 if n.m_Type == "Array" else (n.m_TypeFlags or 0)
    flat.append([level, n.m_Type, n.m_Name, n.m_ByteSize, flags, n.m_MetaFlag or 0,
                 n.m_Version or 1])
    for child in (n.m_Children or []):
        walk(child, level + 1)


walk(node, 0)
common = {int(k): v for k, v in get_common_strings().items()}
with open(os.path.join(HERE, "fixtures", "shader_2021_3_16_typetree.json"), "w") as handle:
    json.dump({"_comment": [
        "UnityEngine.Shader (class 48) type tree for Unity 2021.3.16f1, the version Beat Saber 1.40 and",
        "VivifyTemplate build with. Dumped from UnityPy's type-tree package (lzma.tpk); regenerate with",
        "tools/bundleconvert/dump_shader_typetree.py. Each node: [level, type, name, byteSize, typeFlags,",
        "metaFlag, version]. commonStrings is Unity's built-in string table, offset -> string."],
        "nodes": flat, "commonStrings": {str(k): v for k, v in sorted(common.items())}}, handle, indent=0)
print(len(flat), "nodes")
