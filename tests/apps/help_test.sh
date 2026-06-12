#!/bin/sh
set -u
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
export RAIKIRI_RUNTIME_DIR="$scratch/run" RAIKIRI_DATA_DIR="$scratch/data"
status=0
for bin in "$@"; do
    for flag in --help -h; do
        out=$("$bin" "$flag" 2>/dev/null)
        code=$?
        name=$(basename "$bin")
        if [ "$code" -ne 0 ]; then
            echo "$name $flag: exit $code"
            status=1
        fi
        case "$out" in
            "usage: $name"*) ;;
            *) echo "$name $flag: no usage on stdout"; status=1 ;;
        esac
        if [ -n "$(ls -A "$scratch")" ]; then
            echo "$name $flag: left files behind:"
            find "$scratch" -mindepth 1
            status=1
        fi
    done
done
exit $status
