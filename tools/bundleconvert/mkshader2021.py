"""Builds Shader objects exactly as Unity 2021.3.16 serializes them.

mkshader.py's fixtures use a hand-built, minimal Shader type tree: enough to
test the program store, but with no m_ParsedForm to speak of. Conversion now
depends on m_ParsedForm -- that is where Unity decides which compiled program a
pass uses, and what type it thinks that program is -- so these fixtures use
Unity's own type tree for the class (fixtures/shader_2021_3_16_typetree.json,
2304 nodes, dumped from UnityPy's type-tree package) and serialize values
through it field by field, alignment and all. Names Unity keeps in its common
string table are written as common-string references, as a real bundle does.
"""
import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))

with open(os.path.join(HERE, "fixtures", "shader_2021_3_16_typetree.json")) as _handle:
    _DUMP = json.load(_handle)

COMMON_STRINGS = {v: int(k) for k, v in _DUMP["commonStrings"].items()}
ALIGN = 0x4000

VERTEX, FRAGMENT, GEOMETRY = "progVertex", "progFragment", "progGeometry"


class Node:
    __slots__ = ("level", "type", "name", "size", "flags", "meta", "version", "children")

    def __init__(self, level, type_name, name, size, flags, meta, version):
        self.level, self.type, self.name, self.size = level, type_name, name, size
        self.flags, self.meta, self.version = flags, meta, version
        self.children = []

    @property
    def is_array(self):
        return bool(self.flags & 1)

    def child(self, name):
        for c in self.children:
            if c.name == name:
                return c
        raise KeyError(name)


def _build_tree():
    flat = [Node(*entry) for entry in _DUMP["nodes"]]
    stack = []
    for node in flat:
        while stack and stack[-1].level >= node.level:
            stack.pop()
        if stack:
            stack[-1].children.append(node)
        stack.append(node)
    return flat[0], flat


ROOT, FLAT = _build_tree()


class RealTypeTree:
    """Encodes the dumped tree as a SerializedFile type-tree blob."""

    def encode(self, sf_version):
        strings = bytearray()
        local = {}

        def offset(text):
            if text in COMMON_STRINGS:
                return COMMON_STRINGS[text] | 0x80000000
            if text not in local:
                local[text] = len(strings)
                strings.extend(text.encode() + b"\0")
            return local[text]

        out = bytearray(struct.pack('<ii', len(FLAT), 0))
        for index, node in enumerate(FLAT):
            out += struct.pack('<HBBIIiiI', node.version, node.level, node.flags, offset(node.type),
                               offset(node.name), node.size, index, node.meta)
            if sf_version >= 19:
                out += struct.pack('<Q', 0)
        struct.pack_into('<i', out, 4, len(strings))
        return bytes(out) + bytes(strings)


def default(node):
    if node.type == "string":
        return ""
    if node.is_array:
        return []
    if not node.children:
        return 0
    if node.type in ("vector", "map", "set", "staticvector") and len(node.children) == 1:
        return []
    return {c.name: default(c) for c in node.children}


_SCALARS = {
    ("SInt8", 1): 'b', ("UInt8", 1): 'B', ("char", 1): 'B', ("bool", 1): 'B',
    ("SInt16", 2): 'h', ("UInt16", 2): 'H', ("short", 2): 'h', ("unsigned short", 2): 'H',
    ("int", 4): 'i', ("SInt32", 4): 'i', ("unsigned int", 4): 'I', ("UInt32", 4): 'I',
    ("float", 4): 'f', ("SInt64", 8): 'q', ("UInt64", 8): 'Q', ("long long", 8): 'q',
    ("unsigned long long", 8): 'Q', ("double", 8): 'd', ("FileSize", 8): 'Q',
}


def _align(out):
    while len(out) % 4:
        out.append(0)


def write(node, value, out):
    if node.type == "string" and not node.is_array:
        encoded = value.encode() if isinstance(value, str) else bytes(value)
        out += struct.pack('<i', len(encoded)) + encoded
        array = node.children[0]
        if array.meta & ALIGN or node.meta & ALIGN:
            _align(out)
        return
    if node.is_array:
        data = node.children[1]
        items = list(value)
        out += struct.pack('<i', len(items))
        for item in items:
            write(data, item, out)
    elif not node.children:
        fmt = _SCALARS.get((node.type, node.size))
        if fmt is None:
            raise ValueError("no scalar format for %s %s (%d bytes)" % (node.type, node.name, node.size))
        out += struct.pack('<' + fmt, value)
    elif len(node.children) == 1 and node.children[0].is_array:
        # vector / map / set: a wrapper whose only child is the Array.
        write(node.children[0], value, out)
    else:
        for child in node.children:
            write(child, value[child.name], out)
    if node.meta & ALIGN:
        _align(out)


def player_sub_program(blob, gpu_type, keywords=()):
    return {"m_BlobIndex": blob, "m_KeywordIndices": list(keywords), "m_ShaderRequirements": 0,
            "m_GpuProgramType": gpu_type}


def shader_body(name, platforms, store, passes, keyword_names=()):
    """One 2021.3.16 Shader body.

    store: (offsets, compressed_lengths, decompressed_lengths, blob) as
    mkshader.build_program_store returns. passes: one dict per pass, mapping a
    stage field ("progVertex", ...) to {"player": [[player_sub_program, ...]],
    "params": [[parameter blob index, ...]]} (2021.3.10+ layout)."""
    value = default(ROOT)
    parsed = value["m_ParsedForm"]
    parsed["m_Name"] = name
    parsed["m_KeywordNames"] = list(keyword_names)
    parsed["m_KeywordFlags"] = [0] * len(keyword_names)
    pass_node = ROOT.child("m_ParsedForm").child("m_SubShaders").children[0].child("data") \
        .child("m_Passes").children[0].child("data")
    pass_values = []
    for spec in passes:
        entry = default(pass_node)
        for stage, programs in spec.items():
            entry[stage]["m_PlayerSubPrograms"] = programs.get("player", [])
            entry[stage]["m_ParameterBlobIndices"] = programs.get("params", [])
        entry["m_ProgramMask"] = 0
        pass_values.append(entry)
    subshader = default(ROOT.child("m_ParsedForm").child("m_SubShaders").children[0].child("data"))
    subshader["m_Passes"] = pass_values
    parsed["m_SubShaders"] = [subshader]
    value["m_Name"] = name
    offsets, compressed, decompressed, blob = store
    value["platforms"] = list(platforms)
    value["offsets"] = offsets
    value["compressedLengths"] = compressed
    value["decompressedLengths"] = decompressed
    value["compressedBlob"] = list(blob)
    value["stageCounts"] = []
    out = bytearray()
    write(ROOT, value, out)
    return bytes(out)
