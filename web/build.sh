#!/bin/sh
# Build Syrup for the web: the test simulator compiled to WebAssembly, packed into one page (build/web/index.html).
# Needs Emscripten (emcc). The page works offline; settings and the drum grid are kept in the browser (IndexedDB).
set -e
here=$(cd "$(dirname "$0")" && pwd); root=$here/..; out=$root/build/web
mkdir -p "$out"
"$root/tests/host/gen.sh" "$out"
cp "$root/tests/host/fake3ds.h" "$root/tests/host/fakecitro.h" "$root/tests/host/default_font.bin" "$here/web_main.c" "$out/"
cd "$out"
emcc -O2 web_main.c -I "$root/source" -w -o syrup.js -DSYRUP_BUILD=$(git -C "$root" rev-list --count HEAD 2>/dev/null || echo 0) \
  -s ASYNCIFY=1 -s ASYNCIFY_STACK_SIZE=65536 -s ALLOW_MEMORY_GROWTH=1 -s INITIAL_MEMORY=134217728 -s TOTAL_STACK=1048576 \
  -s SINGLE_FILE=1 -s ENVIRONMENT=web -s EXIT_RUNTIME=0 -s FORCE_FILESYSTEM=1 \
  -s EXPORTED_RUNTIME_METHODS='["FS","HEAPU8","HEAPU16","HEAPF32","addRunDependency","removeRunDependency","UTF8ToString"]' \
  --embed-file default_font.bin -lidbfs.js
python3 "$here/pack.py" "$here/shell.html" syrup.js index.html
echo "built $out/index.html ($(wc -c < index.html) bytes)"
