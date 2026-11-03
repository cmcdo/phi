#!/bin/sh
# Embed text files as C byte arrays so installed binaries do not depend on a
# source-tree path: the base agent prompt, the on-demand extension guide and
# the public extension SDK header. Files are only rewritten when they change.
set -eu
cd -- "$(dirname -- "$0")/.."
mkdir -p build/generated
embed() {
    source=$1
    destination=$2
    temporary=$(mktemp build/generated/.agent-doc-XXXXXX)
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    {
        printf '/* Generated from %s; do not edit. */\n' "$source"
        od -An -v -tu1 "$source" |
            awk '{for (i=1; i<=NF; i++) printf "%s,", $i; printf "\n";}'
        printf '0\n'
    } > "$temporary"
    if ! cmp -s "$temporary" "$destination"; then
        chmod 644 "$temporary"
        mv "$temporary" "$destination"
    fi
    trap - EXIT HUP INT TERM
    rm -f "$temporary"
}
embed docs/agent-system.md build/generated/agent-system.inc
embed docs/extensions-agent.md build/generated/extension-guide.inc
embed include/phi_extension.h build/generated/extension-sdk.inc
