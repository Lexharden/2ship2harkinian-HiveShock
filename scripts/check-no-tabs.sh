#!/bin/sh
# Fail if any new HiveShock/Network C/C++ source contains a tab.

cd "$(dirname "$0")/.." || exit 2

if git grep -n "$(printf '\t')" -- 'mm/2s2h/Network/*.cpp' 'mm/2s2h/Network/*.h' 'mm/2s2h/Network/*.hpp'; then
    echo "replace tabs with spaces"
    exit 1
fi
