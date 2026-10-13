#!/bin/sh
# Build the R5 darkbase consumer against the R3 store.
#
# The workspace build graph owns the engine's own source set (`setup_darkbase` in
# tools/workspace.c) and produces the archives below; this host links them rather
# than re-listing the engine's sources, so the two can never drift.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="${B_OUT:-$HOME/Library/Application Support/vexgraph/b/out/debug}"

clang -std=gnu23 -Wall -Wextra -Werror -O1 -UNDEBUG -mcpu=apple-m1 -mmacosx-version-min=14.0 \
    -I "$root/drivers/darkbase-engine/src" \
    -I "$root/host-behavior/vexspoke/src" \
    -I "$root/host-behavior/relational-engine/src" \
    "$here/src/main.c" \
    "$out/lib/libdarkbase-engine.a" "$out/lib/libvexspoke.a" "$out/lib/librelational_engine.a" \
    -o "$here/darkbase-host"
echo "built $here/darkbase-host"
