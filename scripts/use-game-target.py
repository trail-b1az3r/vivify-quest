#!/usr/bin/env python3
"""Point qpm.json and mod.json at another Beat Saber version.

The repository builds for 1.40.8_7379 (qpm.json, mod.json and the vendored
extern/). A file in targets/ describes another game version: the dependency
ranges it needs and the game package version. This applies it in place, for
a CI job that then runs `qpm restore` against qpackages.com.

    python3 scripts/use-game-target.py 1.45.3 --check   # are its dependencies released?
    python3 scripts/use-game-target.py 1.45.3           # rewrite qpm.json and mod.json

--check exits 0 when every range resolves on qpackages.com to a release that
is built for the target game (its bs-cordl dependency names the target), and
3 when something is not released yet, listing what. Anything else is an error.
"""

import argparse
import json
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
NOT_READY = 3


def fetch_json(url):
    request = urllib.request.Request(url, headers={"User-Agent": "vivify-quest-target"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def parse(version):
    core = re.split(r"[-+]", version, maxsplit=1)[0]
    parts = [int(p) if p.isdigit() else 0 for p in core.split(".")]
    return tuple((parts + [0, 0, 0])[:3])


def satisfies(version, version_range):
    v = parse(version)
    r = version_range.strip()
    if r.endswith(".*"):
        return version.startswith(r[:-1])
    if r.startswith("^"):
        low = parse(r[1:])
        if low[0] > 0:
            high = (low[0] + 1, 0, 0)
        elif low[1] > 0:
            high = (0, low[1] + 1, 0)
        else:
            high = (0, 0, low[2] + 1)
        return low <= v < high
    if r.startswith(">="):
        return v >= parse(r[2:])
    if r.startswith("="):
        return v == parse(r[1:])
    return v == parse(r)


def newest_matching(package, version_range):
    listing = fetch_json(f"https://qpackages.com/{package}?limit=0")
    versions = [entry["version"] if isinstance(entry, dict) else entry for entry in listing]
    matching = [v for v in versions if satisfies(v, version_range)]
    return max(matching, key=parse) if matching else None


def package_config(package, version):
    data = fetch_json(f"https://qpackages.com/{package}/{version}")
    return data.get("config", data)


def check(target):
    missing = []
    resolved = {}
    for package, version_range in target["qpmDependencies"].items():
        version = newest_matching(package, version_range)
        if version is None:
            missing.append(f"{package} {version_range}: no release")
            continue
        if package in target.get("mustTargetThisGame", []):
            config = package_config(package, version)
            cordl = next((d["versionRange"] for d in config.get("dependencies", []) if d["id"] == "bs-cordl"), "")
            if target["bsCordl"] not in cordl:
                missing.append(f"{package} {version}: built for bs-cordl {cordl or '?'}, not {target['bsCordl']}")
                continue
        resolved[package] = version
    return resolved, missing


def apply(target, resolved):
    qpm_path = ROOT / "qpm.json"
    qpm = json.loads(qpm_path.read_text())
    removed = set(target.get("removedQpmDependencies", []))
    extra = target.get("qpmDependencyData", {})
    dependencies = []
    seen = set()
    for dep in qpm["dependencies"]:
        if dep["id"] in removed:
            continue
        if dep["id"] in target["qpmDependencies"]:
            dep["versionRange"] = target["qpmDependencies"][dep["id"]]
            data = {k: v for k, v in dep.get("additionalData", {}).items() if k != "branchName"}
            data.update(extra.get(dep["id"], {}))
            dep["additionalData"] = data
        dependencies.append(dep)
        seen.add(dep["id"])
    for package, version_range in target["qpmDependencies"].items():
        if package not in seen:
            dependencies.append({"id": package, "versionRange": version_range,
                                 "additionalData": extra.get(package, {})})
    qpm["dependencies"] = dependencies
    qpm_path.write_text(json.dumps(qpm, indent=2) + "\n")

    mod_path = ROOT / "mod.json"
    mod = json.loads(mod_path.read_text())
    mod["packageVersion"] = target["gameVersion"]
    for dep in mod.get("dependencies", []):
        package = dep["id"]
        if package not in target["qpmDependencies"]:
            continue
        dep["version"] = target["qpmDependencies"][package]
        version = resolved.get(package)
        if version:
            link = package_config(package, version)["info"].get("additionalData", {}).get("modLink")
            if link:
                dep["downloadIfMissing"] = link
    mod_path.write_text(json.dumps(mod, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("target", help="a file name in targets/ without .json, e.g. 1.45.3")
    parser.add_argument("--check", action="store_true", help="only report whether the dependencies are released")
    args = parser.parse_args()

    target = json.loads((ROOT / "targets" / f"{args.target}.json").read_text())
    resolved, missing = check(target)
    for package, version in sorted(resolved.items()):
        print(f"  ok       {package} {version}")
    for line in missing:
        print(f"  missing  {line}")
    if missing:
        print(f"Beat Saber {target['gameVersion']}: {len(missing)} dependency(ies) not released yet")
        return NOT_READY
    if args.check:
        print(f"Beat Saber {target['gameVersion']}: every dependency is released")
        return 0
    apply(target, resolved)
    print(f"qpm.json and mod.json now target Beat Saber {target['gameVersion']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
