import os, sys, random, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkbundle import build
HERE = os.path.dirname(os.path.abspath(__file__))
CONV = os.environ.get("VIVIFY_CONV", "/tmp/conv"); TMP = os.path.join(HERE, "work")
src = os.path.join(TMP, "fz_src.vivify"); mut = os.path.join(TMP, "fz.vivify")
rnd = random.Random(1234)
crashes = 0
for comp in (0, 1, 2, 3):
    build(src, compression=comp, block_compression=comp, n_files=2, payload=9000)
    base = bytearray(open(src, 'rb').read())
    for trial in range(150):
        b = bytearray(base)
        for _ in range(rnd.randrange(1, 12)):
            b[rnd.randrange(len(b))] = rnd.randrange(256)
        open(mut, 'wb').write(bytes(b))
        p = subprocess.run([CONV, mut, os.path.join(TMP, "fz.out")], capture_output=True)
        # exit 0/1 = clean success/handled failure. Anything else = crash/sanitizer.
        if p.returncode not in (0, 1):
            crashes += 1
            print(f"CRASH comp={comp} trial={trial} rc={p.returncode}")
            print(p.stderr[:3000].decode("utf-8", "replace"))
            open(os.path.join(TMP, f"crash_{comp}_{trial}.bin"), 'wb').write(bytes(b))
            if crashes > 3: sys.exit(1)
# A 2021.3.16-layout PC shader (written by run_tests.py): corrupting it walks
# m_ParsedForm, the segmented program store and the in-place program-list
# patches over hostile bytes. The mutations are kept inside the serialized
# file, where they reach the parser rather than only the archive header.
seed = os.path.join(TMP, "fuzz_seed_2021.vivify")
if os.path.exists(seed):
    base = bytearray(open(seed, 'rb').read())
    lo = len(base) // 8
    for trial in range(300):
        b = bytearray(base)
        for _ in range(rnd.randrange(1, 10)):
            b[rnd.randrange(lo, len(b))] = rnd.randrange(256)
        open(mut, 'wb').write(bytes(b))
        p = subprocess.run([CONV, "--shaders", mut, os.path.join(TMP, "fz.out")], capture_output=True)
        if p.returncode not in (0, 1):
            crashes += 1
            print(f"CRASH parsed-form trial={trial} rc={p.returncode}")
            print(p.stderr[:3000].decode("utf-8", "replace"))
            open(os.path.join(TMP, f"crash_pf_{trial}.bin"), 'wb').write(bytes(b))
            if crashes > 3: sys.exit(1)
else:
    print("note: no fuzz_seed_2021.vivify (run run_tests.py first); m_ParsedForm fuzzing skipped")
print("crashes:", crashes)
sys.exit(1 if crashes else 0)
