#!/usr/bin/env bash
# Build Phi, its background chat host, and bundled widget modules.
set -euo pipefail
cd -- "$(dirname -- "$0")"
if (( $# )); then
    echo "usage: $0" >&2
    exit 2
fi
./scripts/embed-agent-docs.sh
flags=(-O2 -Wall -Wextra -Wshadow -std=c11 -Ibuild/generated
    -DPHI_DOCS_DIR=\""$PWD/docs"\"
    -DPHI_EXTENSION_JSON_CFLAGS=\""$(pkg-config --cflags json-c)"\"
    -DPHI_EXTENSION_JSON_LIBS=\""$(pkg-config --libs json-c)"\"
    -DPHI_TERMINFO_DIR=\""$PWD/build/terminfo"\")
mkdir -p build/widgets build/terminfo

temporary=""
trap 'if [[ -n $temporary ]]; then rm -f -- "$temporary"; fi' EXIT

# Keep temporary outputs beside their destinations for atomic replacement.
temporary=$(mktemp ./build/.phi-host-XXXXXX)
cc -O2 -Wall -Wextra -Wshadow -std=c11 \
    src/host/phi-host.c src/core/host_attach.c src/core/proctitle.c \
    -o "$temporary" -lutil
chmod 0755 "$temporary"
mv -fT "$temporary" phi-host
temporary=""

tic -x -o "$PWD/build/terminfo" assets/phi.terminfo
for widget in life orbit; do
    temporary=$(mktemp "build/widgets/.$widget-XXXXXX.so")
    cc -O2 -Wall -Wextra -std=c11 -shared -fPIC -Iinclude \
        "src/widgets/$widget.c" -o "$temporary"
    chmod 0644 "$temporary"
    mv -f "$temporary" "build/widgets/$widget.so"
    temporary=""
done

temporary=$(mktemp ./build/.phi-XXXXXX)
cc "${flags[@]}" -DPHI_UI src/phi.c src/ui/ui.c src/ui/board.c \
    src/core/*.c src/tools/*.c -o "$temporary" -ldl \
    $(pkg-config --cflags --libs libcurl json-c ncursesw openssl)
chmod 0755 "$temporary"
mv -fT "$temporary" phi
temporary=""
