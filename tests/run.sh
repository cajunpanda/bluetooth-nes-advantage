#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright 2026 Aaron Perkins
#
# Host-side firmware logic tests. No hardware, no ESP-IDF: the code under test is lifted out of
# firmware/main/ and compiled against stubs by a normal host compiler.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
main="$here/../firmware/main"
build="$here/.build"
mkdir -p "$build"

CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--std=c++17 -O0 -g -Wall -Wextra -Wno-unused-parameter}"

# Lift a marked section out of a source file. Keyed on the section's banner comments rather than
# line numbers so it tracks edits above it, and hard-fails if the markers move or get renamed
# instead of silently testing an empty or truncated fragment.
extract() {
    local src="$1" start="$2" end="$3" out="$4" want="$5"
    awk -v s="$start" -v e="$end" '
        index($0, s) == 1 { f = 1 }
        index($0, e) == 1 { f = 0 }
        f
    ' "$src" > "$out"
    if ! grep -q "$want" "$out"; then
        echo "extract: '$want' not found between markers in $src" >&2
        echo "  start: $start" >&2
        echo "  end:   $end" >&2
        echo "Markers were renamed or moved. Fix them here and in the source." >&2
        exit 1
    fi
}

fail=0
run_case() {
    local name="$1"
    echo "== $name"
    if "$build/$name"; then :; else fail=1; fi
}

# --- chords: Select as a shift key (app_main.cpp) ----------------------------------------------
extract "$main/app_main.cpp" \
        "// --- Chords: Select as a shift key" \
        "// --- Gestures (hold-combos)" \
        "$build/chords_extracted.inc" \
        "static void apply_chords"

"$CXX" $CXXFLAGS -I "$build" -o "$build/chords_test" "$here/chords_test.cpp"
run_case chords_test

exit $fail
