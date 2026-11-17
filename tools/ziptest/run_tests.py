#!/usr/bin/env python3
"""Tests for src/VivifyZip.cpp. Build with:
    g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I include -o /tmp/ziptest \\
        tools/ziptest/main.cpp src/VivifyZip.cpp -lz
    VIVIFY_ZIPTEST=/tmp/ziptest python3 tools/ziptest/run_tests.py
"""
import io, os, random, subprocess, sys, tempfile, zipfile

BIN = os.environ.get("VIVIFY_ZIPTEST", "/tmp/ziptest")
failures = 0


def run(data):
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "in.zip")
        dest = os.path.join(tmp, "out")
        with open(src, "wb") as f:
            f.write(data)
        r = subprocess.run([BIN, src, dest], capture_output=True, text=True, errors="replace")
        files = {}
        if os.path.isdir(dest):
            for root, _, names in os.walk(dest):
                for n in names:
                    p = os.path.join(root, n)
                    files[os.path.relpath(p, dest).replace(os.sep, "/")] = open(p, "rb").read()
        if "AddressSanitizer" in r.stderr or "runtime error" in r.stderr:
            raise AssertionError("sanitizer: " + r.stderr[:500])
        return r.returncode, r.stdout.strip(), files


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else " " + detail))
    if not cond:
        failures += 1


def make(entries, method=zipfile.ZIP_DEFLATED):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as z:
        for n, d in entries.items():
            z.writestr(n, d)
    return buf.getvalue()


rng = random.Random(1)
blob = bytes(rng.getrandbits(8) for _ in range(200000))
entries = {"Info.dat": b'{"_songName":"x"}' * 50, "song.ogg": blob, "bundleWindows2021.vivify": b"\0" * 100000,
           "sub/cover.png": b"png" * 7, "empty.txt": b""}

for label, method in (("deflate", zipfile.ZIP_DEFLATED), ("stored", zipfile.ZIP_STORED)):
    code, out, files = run(make(entries, method))
    check(label + " extracts", code == 0, out)
    check(label + " contents match", files == entries, str(sorted(files)))

# Streamed (flag bit 3) entries: sizes only in the central directory.
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
    with z.open("Info.dat", "w") as f:
        f.write(b"streamed" * 1000)
code, out, files = run(buf.getvalue())
check("data descriptor entries", code == 0 and files.get("Info.dat") == b"streamed" * 1000, out)

# Explicit directory entries.
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w") as z:
    z.writestr(zipfile.ZipInfo("dir/"), b"")
    z.writestr("dir/a.dat", b"a")
code, out, files = run(buf.getvalue())
check("directory entries", code == 0 and files == {"dir/a.dat": b"a"}, out)

for bad in ("../evil.dat", "/abs.dat", "a/../../evil.dat", "c:/x.dat", "..\\x.dat"):
    code, out, _ = run(make({bad: b"x"}))
    check("refuses " + bad, code == 1 and "unsafe" in out, out)

good = make(entries)
code, out, _ = run(good[:len(good) // 2])
check("truncated zip fails", code == 1, out)
code, out, _ = run(b"not a zip at all, just text" * 3)
check("non-zip fails", code == 1, out)

# Corrupt a byte inside the compressed song data: must fail (inflate or CRC).
data = bytearray(good)
i = data.find(b"song.ogg") + 200
data[i] ^= 0xFF
code, out, _ = run(bytes(data))
check("corrupt data fails", code == 1, out)

# Fuzz: random byte flips must never crash or trip the sanitizers.
for n in range(300):
    data = bytearray(good if n % 2 else make({"a": b"hello" * 100, "b/c": b"x"}))
    for _ in range(rng.randint(1, 8)):
        data[rng.randrange(len(data))] = rng.getrandbits(8)
    try:
        code, out, _ = run(bytes(data))
        if code not in (0, 1):
            raise AssertionError("exit %d %s" % (code, out))
    except AssertionError as e:
        check("fuzz %d" % n, False, str(e))
        break
else:
    check("fuzz 300 mutations", True)

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
