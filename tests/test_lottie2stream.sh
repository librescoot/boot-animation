#!/bin/sh
set -eu

packer=${1:?packer path required}
checker=${2:?checker path required}
tmp=${TMPDIR:-/tmp}/lottie2stream-test.$$
mkdir "$tmp"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

json=$tmp/test.json
long_json=$tmp/one-second.json
printf '%s\n' '{"v":"5.7.4","fr":25,"ip":0,"op":1,"w":2,"h":2,"layers":[]}' >"$json"
printf '%s\n' '{"v":"5.7.4","fr":25,"ip":0,"op":25,"w":2,"h":2,"layers":[]}' >"$long_json"

fail()
{
    printf 'test_lottie2stream: %s\n' "$1" >&2
    exit 1
}

expect_failure()
{
    if "$@" >"$tmp/failure.log" 2>&1; then
        fail "command unexpectedly succeeded: $*"
    fi
}

"$packer" "$json" 2 2 1000 "$tmp/fps1000.lsba"
"$checker" "$tmp/fps1000.lsba" 2 2 1
"$packer" "$json" 2 2 1 "$tmp/fps1.lsba" --loop
"$checker" "$tmp/fps1.lsba" 2 2 1000
"$packer" "$long_json" 2 2 501 "$tmp/fps501.lsba"
"$checker" "$tmp/fps501.lsba" 2 2 1 1000

for bad_fps in 0 1001 -1 +1 1x 4294967296; do
    output=$tmp/bad-fps-$bad_fps.lsba
    expect_failure "$packer" "$json" 2 2 "$bad_fps" "$output"
    [ ! -e "$output" ] || fail "invalid fps created an output"
done
for bad_width in 0 -1 2x 4294967296; do
    output=$tmp/bad-width-$bad_width.lsba
    expect_failure "$packer" "$json" "$bad_width" 2 25 "$output"
    [ ! -e "$output" ] || fail "invalid width created an output"
done
expect_failure "$packer" "$json" 1073741824 1 25 "$tmp/overflow.lsba"
[ ! -e "$tmp/overflow.lsba" ] || fail "overflowing geometry created an output"

mkdir "$tmp/special.lsba"
expect_failure "$packer" "$json" 2 2 25 "$tmp/special.lsba"
[ -d "$tmp/special.lsba" ] || fail "special target was replaced"
expect_failure "$packer" "$json" 2 2 25 "$tmp/missing/output.lsba"
[ ! -e "$tmp/missing/output.lsba" ] || fail "missing parent produced output"

printf 'original target\n' >"$tmp/preserved.lsba"
(
    trap '' 25
    ulimit -f 1
    "$packer" "$json" 128 128 1000 "$tmp/preserved.lsba"
) >"$tmp/write-failure.log" 2>&1 && fail "file-size-limited write succeeded"
[ "$(cat "$tmp/preserved.lsba")" = "original target" ] ||
    fail "failed write replaced the target"
set -- "$tmp"/preserved.lsba.tmp.*
[ ! -e "$1" ] || fail "failed write left a temporary file"

printf 'read only\n' >"$tmp/unwritable.lsba"
chmod 444 "$tmp/unwritable.lsba"
if [ ! -w "$tmp/unwritable.lsba" ]; then
    expect_failure "$packer" "$json" 2 2 25 "$tmp/unwritable.lsba"
    [ "$(cat "$tmp/unwritable.lsba")" = "read only" ] ||
        fail "unwritable target was replaced"
fi

printf '%s\n' 'lottie2stream integration tests passed'
