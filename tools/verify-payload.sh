#!/usr/bin/env bash
set -euo pipefail
variant=${1:?Usage: verify-payload.sh debug|release payload.elf}
payload=${2:?Usage: verify-payload.sh debug|release payload.elf}
sections=$("${LLVM_READOBJ:-llvm-readobj-18}" --sections "$payload")

# Dynamic symbols are required by the payload loader in both variants.
grep -q 'Name: .dynsym ' <<< "$sections"
case "$variant" in
    debug)
        grep -q 'Name: .debug_info ' <<< "$sections"
        grep -q 'Name: .debug_line ' <<< "$sections"
        grep -q 'Name: .symtab ' <<< "$sections"
        ;;
    release)
        if grep -Eq 'Name: \.debug_|Name: \.zdebug_|Name: \.symtab ' <<< "$sections"; then
            echo "Release payload still contains debug information or a static symbol table" >&2
            exit 1
        fi
        ;;
    *) echo "Unknown build variant: $variant" >&2; exit 1 ;;
esac
echo "PASS: $variant payload symbol policy ($payload)"
