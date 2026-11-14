#!/bin/sh
# Optional F4-scale check (roadmap f4-scale). Not part of make test / CI.
#
#   make f4-scale
#
# Boots SCALE_NODES x86_64 UEFI guests (default 8) on one simulated
# network (tests/netsim.py). They all TFTP the same image. One node is a
# large NUMA machine: -smp SCALE_SMP (default 256) with SCALE_NUMA memory
# nodes. Under QEMU TCG, x2APIC is unavailable and APIC ID 255 is
# invalid, so the script falls back to 255 CPUs (the xAPIC maximum) and
# says so. With KVM and a host CPU that has x2APIC, 256 runs as asked.
#
# Knobs: SCALE_NODES, SCALE_SMP, SCALE_MEM, SCALE_NUMA, SCALE_MEM_NODE,
# F4_SCALE_TIMEOUT (seconds for the large node; default 300),
# NETSIM_IDLE (seconds of silence before netsim exits; default 300 here),
# QEMU_X86_64. Logs: build/f4-scale/*.log.

set -u

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TEST_DIR=${TEST_DIR:-$ROOT/build/tests}
WORK=${F4_SCALE_WORK:-$ROOT/build/f4-scale}
BIOS=${BIOS:-$ROOT/x86_64/squirrel-bios.fd}
APP=${APP:-$TEST_DIR/x86_64-app.efi}
QEMU=${QEMU_X86_64:-qemu-system-x86_64}

SCALE_NODES=${SCALE_NODES:-8}
SCALE_SMP=${SCALE_SMP:-256}
SCALE_MEM=${SCALE_MEM:-1024}
SCALE_NUMA=${SCALE_NUMA:-4}
SCALE_MEM_NODE=${SCALE_MEM_NODE:-$((SCALE_MEM / SCALE_NUMA))}
TIMEOUT=${F4_SCALE_TIMEOUT:-300}
export NETSIM_IDLE=${NETSIM_IDLE:-300}

die() { echo "f4-scale: $*" >&2; exit 1; }

[ -f "$BIOS" ] || die "missing $BIOS (build x86_64 first)"
[ -f "$APP" ] || die "missing $APP (build the test images first)"
[ "$SCALE_NODES" -ge 2 ] || die "SCALE_NODES must be at least 2"
[ "$SCALE_NUMA" -ge 1 ] || die "SCALE_NUMA must be at least 1"
[ $((SCALE_MEM_NODE * SCALE_NUMA)) -eq "$SCALE_MEM" ] || \
    die "SCALE_MEM ($SCALE_MEM) must equal SCALE_NUMA * SCALE_MEM_NODE ($SCALE_NUMA * $SCALE_MEM_NODE)"

mkdir -p "$WORK"
pass=0
fail=0

# Can this QEMU start -smp N with the given extra args? TCG rejects 256
# (APIC ID 255 invalid); KVM may be present but not usable.
smp_ok() {
    n=$1
    shift
    err=$WORK/smp-probe-$n.err
    rm -f "$err"
    timeout 5 "$QEMU" -M q35 -m 128 -smp "$n" "$@" \
        -display none -monitor none -serial none \
        -bios "$BIOS" >/dev/null 2>"$err" &
    q=$!
    sleep 0.5
    alive=
    case $(ps -o stat= -p "$q" 2>/dev/null) in
    ""|Z*) ;;
    *) alive=1; kill "$q" 2>/dev/null ;;
    esac
    wait "$q" 2>/dev/null
    [ -n "$alive" ] || return 1
    ! grep -q 'APIC ID .* is invalid' "$err" 2>/dev/null
}

ACCEL=
CPU=
if [ -r /dev/kvm ] && smp_ok 1 -enable-kvm -cpu host; then
    ACCEL="-enable-kvm"
    CPU="-cpu host"
fi

BIG_SMP=$SCALE_SMP
if ! smp_ok "$BIG_SMP" $ACCEL $CPU; then
    if [ "$BIG_SMP" -gt 255 ]; then
        echo "f4-scale: -smp $BIG_SMP needs KVM/x2APIC; using 255 (xAPIC max under TCG)"
        BIG_SMP=255
        smp_ok "$BIG_SMP" $ACCEL $CPU || \
            die "QEMU refuses -smp $BIG_SMP (see $WORK/smp-probe-$BIG_SMP.err)"
    else
        die "QEMU refuses -smp $BIG_SMP (see $WORK/smp-probe-$BIG_SMP.err)"
    fi
fi

# NUMA: split BIG_SMP across SCALE_NUMA nodes as evenly as possible.
numa_args() {
    smp=$1
    nodes=$2
    mb=$3
    base=0
    i=0
    while [ "$i" -lt "$nodes" ]; do
        n=$((smp / nodes))
        rem=$((smp % nodes))
        [ "$i" -lt "$rem" ] && n=$((n + 1))
        last=$((base + n - 1))
        printf ' -object memory-backend-ram,id=nm%d,size=%dM -numa node,nodeid=%d,cpus=%d-%d,memdev=nm%d' \
            "$i" "$mb" "$i" "$base" "$last" "$i"
        base=$((last + 1))
        i=$((i + 1))
    done
}

# Node i uses MAC ...:$((0x56+i)) and address 10.0.2.$((15+i)).
mac_of() { printf '52:54:00:12:34:%02x' $((0x56 + $1)); }
ip_of() { echo "10.0.2.$((15 + $1))"; }

expect_net() {
    i=$1
    mac=$(mac_of "$i")
    ip=$(ip_of "$i")
    echo "net: virtio-net at PCI 00:02.0, MAC $mac|net: address $ip from 192.168.50.5, boot file x86_64-app.efi|net: boot server on another network, through router 10.0.2.1|net: loaded 12288 bytes|APP x86_64 st=ok bs=ok li=ok base=ok reloc=ok pool=ok map=ok|efi: x86_64-app.efi returned 0x1234"
}

expect_big() {
    echo "smp: $BIG_SMP CPUs|numa: $SCALE_NUMA nodes:|$(expect_net 0)"
}

log_ok() {
    log=$1
    expect=$2
    [ -f "$log" ] || return 1
    ok=1
    old_ifs=$IFS
    IFS='|'
    for want in $expect; do
        case $want in
        "~"*) grep -qE -- "${want#?}" "$log" || ok=0 ;;
        "!"*) grep -qF -- "${want#?}" "$log" && ok=0 ;;
        *)    grep -qF -- "$want" "$log" || ok=0 ;;
        esac
    done
    IFS=$old_ifs
    return $((1 - ok))
}

wait_log() {
    name=$1
    log=$2
    expect=$3
    ticks=$((TIMEOUT * 10))
    while :; do
        if log_ok "$log" "$expect"; then
            echo "PASS  $name"
            pass=$((pass + 1))
            return 0
        fi
        [ "$ticks" -le 0 ] && break
        sleep 0.1
        ticks=$((ticks - 1))
    done
    echo "FAIL  $name (expected: $expect)"
    sed 's/^/      | /' "$log" | tail -40
    fail=$((fail + 1))
    return 1
}

NETSIM_PORT=$((20000 + $$ % 20000))
NETSIM_IDLE=$NETSIM_IDLE python3 "$ROOT/tests/netsim.py" \
    "$NETSIM_PORT" $((NETSIM_PORT + 1)) "$TEST_DIR" x86_64-app.efi &
netsim=$!
sleep 0.3
kill -0 "$netsim" 2>/dev/null || die "netsim failed to start"

cleanup() {
    # shellcheck disable=SC2086
    kill $nodes $netsim 2>/dev/null
    # shellcheck disable=SC2086
    wait $nodes $netsim 2>/dev/null
}
nodes=
trap cleanup EXIT INT TERM

# Nodes 1..SCALE_NODES-1: ordinary UP diskless guests in the background.
i=1
while [ "$i" -lt "$SCALE_NODES" ]; do
    log=$WORK/node-$i.log
    rm -f "$log"
    "$QEMU" -M q35 -m 128 -bios "$BIOS" -display none -monitor none \
        -serial "file:$log" \
        -netdev socket,id=n0,udp=127.0.0.1:$NETSIM_PORT,localaddr=127.0.0.1:$((NETSIM_PORT + 10 + i)) \
        -device virtio-net-pci,netdev=n0,mac="$(mac_of "$i")" >/dev/null 2>&1 &
    nodes="$nodes $!"
    i=$((i + 1))
done

# Node 0: the large NUMA node (foreground wait).
BIG_LOG=$WORK/node-0.log
rm -f "$BIG_LOG"
# shellcheck disable=SC2046
"$QEMU" -M q35 -m "${SCALE_MEM}M" -smp "$BIG_SMP" $ACCEL $CPU \
    $(numa_args "$BIG_SMP" "$SCALE_NUMA" "$SCALE_MEM_NODE") \
    -bios "$BIOS" -display none -monitor none \
    -serial "file:$BIG_LOG" \
    -netdev socket,id=n0,udp=127.0.0.1:$NETSIM_PORT,localaddr=127.0.0.1:$((NETSIM_PORT + 1)) \
    -device virtio-net-pci,netdev=n0,mac="$(mac_of 0)" >/dev/null 2>&1 &
nodes="$nodes $!"

wait_log "x86_64-uefi-f4-scale-0-smp$BIG_SMP" "$BIG_LOG" "$(expect_big)" || true

i=1
while [ "$i" -lt "$SCALE_NODES" ]; do
    wait_log "x86_64-uefi-f4-scale-$i" "$WORK/node-$i.log" "$(expect_net "$i")" || true
    i=$((i + 1))
done

echo "f4-scale: $pass passed, $fail failed ($SCALE_NODES nodes, big node -smp $BIG_SMP -m ${SCALE_MEM}M, $SCALE_NUMA NUMA nodes)"
[ "$fail" -eq 0 ]
