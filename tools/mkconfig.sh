#!/bin/sh
# Turn a configs/<platform>/<name>.cfg into headers for the build.
# Original code.
#
#   tools/mkconfig.sh nasm|gas|c CFG > OUT
#
# A .cfg file has one KEY=VALUE per line. Blank lines and lines starting
# with # are ignored. KEY is CONFIG_ followed by capitals, digits and _.
# VALUE is a decimal or 0x hex number, or y / n (written as 1 / 0).
# Anything else is an error, so a typo cannot silently fall back to the
# source's built-in default.
#
#   nasm  %define CONFIG_X value     (x86, included with -DSQ_CONFIG)
#   gas   .equ CONFIG_X, value       (AArch64, RISC-V; --defsym SQ_CONFIG=1)
#   c     #define CONFIG_X value     (config.h, for C sources to come)

set -eu

[ $# -eq 2 ] || { echo "usage: $0 nasm|gas|c CFG" >&2; exit 2; }
fmt=$1
cfg=$2
case $fmt in nasm|gas|c) ;; *) echo "mkconfig: unknown format $fmt" >&2; exit 2 ;; esac
[ -f "$cfg" ] || { echo "mkconfig: $cfg not found" >&2; exit 1; }

awk -v fmt="$fmt" -v cfg="$cfg" '
function fail(msg) { printf "%s:%d: %s\n", cfg, NR, msg > "/dev/stderr"; bad = 1; exit 1 }
BEGIN {
    c = (fmt == "nasm") ? ";" : (fmt == "gas") ? "/*" : "/*"
    e = (fmt == "nasm") ? "" : " */"
    printf "%s Generated from %s by tools/mkconfig.sh. Do not edit.%s\n", c, cfg, e
}
{
    sub(/\r$/, "")
    line = $0
    sub(/^[ \t]+/, "", line); sub(/[ \t]+$/, "", line)
    if (line == "" || substr(line, 1, 1) == "#") next
    eq = index(line, "=")
    if (eq == 0) fail("expected KEY=VALUE")
    key = substr(line, 1, eq - 1); val = substr(line, eq + 1)
    sub(/[ \t]+$/, "", key); sub(/^[ \t]+/, "", val)
    if (key !~ /^CONFIG_[A-Z0-9_]+$/) fail("bad key \"" key "\"")
    if (key in seen) fail(key " set twice")
    seen[key] = 1
    if (val == "y") val = 1
    else if (val == "n") val = 0
    else if (val !~ /^(0|[1-9][0-9]*|0x[0-9A-Fa-f]+)$/) fail("bad value \"" val "\" for " key)
    if (fmt == "nasm")     printf "%%define %s %s\n", key, val
    else if (fmt == "gas") printf "        .equ %s, %s\n", key, val
    else                   printf "#define %s %s\n", key, val
}
' "$cfg"
