#!/usr/bin/env bash
# Syntax-check the mod's sources with a host clang, no Android NDK required.
#
# Why this exists: src/VivifyAssets.cpp reached CI carrying four compile errors
# of its own -- a Runtime member called from a free function, and a duplicated
# `score` declaration -- and nobody could see them, because the build had been
# dying earlier in the vendored-dependency include chain for several runs. The
# only compiler in the loop was CI, at roughly one error report per round trip.
#
# This is not a substitute for `qpm s build`:
#   - it targets the host, so pointer-size and aarch64-only code paths differ
#     (Hooking's non-__aarch64__ #else branch is filtered out below);
#   - it uses glibc rather than bionic, so a few identifiers bionic provides are
#     supplied by hostcheck/hostcompat.h, and jni.h by hostcheck/jni.h;
#   - it does not link.
# What it does catch is every type, name-lookup and template error in our own
# code, which is the entire class of failure that has cost round trips so far.
#
# Requires clang and libc++ headers:
#   sudo apt-get install -y clang libc++-dev libc++abi-dev
#
# Usage: scripts/host-syntax-check.sh [file...]     (default: every src/*.cpp)
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stub="$root/scripts/hostcheck"
cd "$root" || exit 1

CLANG="${CLANG:-clang++}"
if ! command -v "$CLANG" >/dev/null; then
  echo "error: $CLANG not found. Install clang and libc++ headers." >&2
  exit 2
fi

if [ "$#" -gt 0 ]; then files=("$@"); else mapfile -t files < <(ls src/*.cpp); fi

# -funsigned-char matches aarch64, where plain char is unsigned; without it,
# paper2_scotland2's logger.hpp trips a narrowing diagnostic that the real build
# never sees.
# VIVIFY_HOST_TARGET=1.45.3 with VIVIFY_HOST_EXTERN=<dir holding includes/>
# checks against Beat Saber 1.45.3's libraries instead (bs-cordl 4503,
# beatsaber-hook 9, Unity 6), laid out as qpm's extern/ would be.
ext="${VIVIFY_HOST_EXTERN:-extern}/includes"
if [ "${VIVIFY_HOST_TARGET:-}" = "1.45.3" ]; then
  game=(-DUNITY_6 -DUNITY_6_3 -DVIVIFY_GAME_1_45_3=1 -std=gnu++23)
  il2cpp="$ext/libil2cpp/il2cpp"
  fmtinc="$ext/fmt/fmt/include"
else
  game=(-DUNITY_2021 -DNEED_UNSAFE_CSHARP -std=gnu++20)
  il2cpp="$ext/libil2cpp"
  fmtinc="$ext/fmt/include"
fi
flags=(
  -fsyntax-only -stdlib=libc++ -funsigned-char
  -DFMT_HEADER_ONLY -DANDROID -DHAS_CODEGEN "${game[@]}"
  -fdeclspec -Wno-invalid-offsetof -Wno-extra-qualification -fexceptions -frtti
  -DMOD_ID='"Vivify"' -DVERSION='"0.0.0"' -ferror-limit=60
  -include "$stub/hostcompat.h"
  -I include -I shared -I "$ext" -I "$ext/bs-cordl/include"
  -isystem "$stub"
  -isystem "$fmtinc"
  -isystem "$il2cpp/libil2cpp"
  -isystem "$il2cpp/external/baselib/Include"
  -isystem "$il2cpp/external/baselib/Platforms/Android/Include"
  -isystem "$ext/paper2_scotland2/shared/utfcpp/source"
)

fail=0
for f in "${files[@]}"; do
  # hooking.hpp 623-624 is the #else of #ifdef __aarch64__: it casts pointers to
  # uint32_t and is never compiled for the real target.
  out=$("$CLANG" "${flags[@]}" "$f" 2>&1 | grep -E "error:" | grep -v -e "hooking.hpp:62[34]" -e "Hooking is only supported on ARM64")
  if [ -n "$out" ]; then
    echo "FAIL $f"
    echo "$out" | sed 's/^/     /'
    fail=1
  else
    echo "ok   $f"
  fi
done

if [ "$fail" -ne 0 ]; then
  echo
  echo "Host syntax check failed. These are real errors in this project's code." >&2
fi
exit "$fail"
