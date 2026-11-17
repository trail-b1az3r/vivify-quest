#!/usr/bin/env python3
"""Tests for src/VivifyGlobalLog.cpp. Build with:
    g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I include -o /tmp/globallog \\
        tools/globallog/main.cpp src/VivifyGlobalLog.cpp
    VIVIFY_GLOBALLOG=/tmp/globallog python3 tools/globallog/run_tests.py
"""
import os, re, subprocess, sys, tempfile

BIN = os.environ.get("VIVIFY_GLOBALLOG", "/tmp/globallog")
failures = 0


def check(name, cond, detail=""):
    global failures
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else " " + str(detail)))
    if not cond:
        failures += 1


def run(d, part_max, total_max, lines, length, header="=== session ==="):
    r = subprocess.run([BIN, d, str(part_max), str(total_max), str(lines), str(length), header],
                       capture_output=True, text=True)
    if "AddressSanitizer" in r.stderr or "runtime error" in r.stderr:
        raise AssertionError(r.stderr[:500])
    return r.returncode


def parts(d):
    out = {}
    for name in os.listdir(d):
        if name == "vivify_global.txt":
            out[1] = name
        m = re.fullmatch(r"vivify_global-p(\d+)\.txt", name)
        if m:
            out[int(m.group(1))] = name
    return dict(sorted(out.items()))


with tempfile.TemporaryDirectory() as d:
    check("first session writes the base file", run(d, 1000, 10**9, 5, 10) == 0 and list(parts(d)) == [1])
    first = open(os.path.join(d, "vivify_global.txt")).read()
    check("header comes first", first.startswith("=== session ===\n"), first[:40])
    run(d, 1000, 10**9, 5, 10, "=== second ===")
    text = open(os.path.join(d, "vivify_global.txt")).read()
    check("a second session appends instead of truncating", "=== session ===" in text and "=== second ===" in text)

with tempfile.TemporaryDirectory() as d:
    run(d, 1000, 10**9, 100, 99)  # 10 KB at 1 KB a part
    p = parts(d)
    check("rotates into -p2, -p3, ... past the part size", list(p) == list(range(1, len(p) + 1)) and len(p) >= 10, p)
    sizes = [os.path.getsize(os.path.join(d, n)) for n in p.values()]
    check("no part grows past the limit by more than a line", all(s <= 1000 + 100 for s in sizes), sizes)
    last = max(p)
    run(d, 1000, 10**9, 1, 10, "=== later ===")
    p2 = parts(d)
    newest = open(os.path.join(d, p2[max(p2)])).read()
    check("a later session continues in the newest part", "=== later ===" in newest and max(p2) in (last, last + 1),
          (last, list(p2)))

with tempfile.TemporaryDirectory() as d:
    # 20 parts of ~1 KB, 10 KB budget: crossing it drops the older half.
    run(d, 1000, 10_000, 200, 99)
    p = parts(d)
    total = sum(os.path.getsize(os.path.join(d, n)) for n in p.values())
    check("over the total budget, older parts are deleted", 1 not in p and total <= 10_000 + 1100, (list(p), total))
    numbers = list(p)
    check("what is left is the newest, contiguous parts", numbers == list(range(numbers[0], numbers[-1] + 1)), numbers)
    before = max(p)
    run(d, 1000, 10_000, 3, 10, "=== after prune ===")
    p2 = parts(d)
    check("numbering continues after a prune (no reuse)", max(p2) >= before, (before, list(p2)))

with tempfile.TemporaryDirectory() as d:
    open(os.path.join(d, "vivify_global-pX.txt"), "w").write("not ours")
    open(os.path.join(d, "vivify_global-p0.txt"), "w").write("not ours")
    open(os.path.join(d, "VivifySession.txt"), "w").write("session")
    run(d, 1000, 2000, 100, 99)
    check("files that are not parts are never touched",
          all(os.path.exists(os.path.join(d, n)) for n in ("vivify_global-pX.txt", "vivify_global-p0.txt",
                                                            "VivifySession.txt")))

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
