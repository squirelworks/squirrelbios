#!/bin/sh
# Stress the RISC-V firmware under Linux: many boots, many harts, and a
# guest program that keeps the harts asking each other for work.
#
#   make linux-stress                      the default run below
#   STRESS_SMP="2 8" STRESS_ROUNDS=10 tests/linux-stress.sh
#
# Each boot is tests/linux-shell.sh with tests/smpstress.c (built here
# with $RISCV_CC, default riscv64-linux-gnu-gcc, static) in the
# initramfs, run as its workload. A boot passes when linux-shell.sh
# passes, smpstress prints PASS, and the serial log has no failed SBI
# call, oops, BUG, WARNING or RCU stall.
#
#   STRESS_SMP      hart counts to boot (default "1 2 3 4 8")
#   STRESS_ROUNDS   boots per hart count (default 3)
#   STRESS_SECS     seconds smpstress runs per boot (default 20)
#   STRESS_MEM      guest RAM (default 1G)
#   STRESS_MB       also write and check this many MB first (default 0)
#   STRESS_HOTPLUG  if 1, take every hart but 0 offline and back online,
#                   STRESS_HOTPLUG_ROUNDS times each (default 30), while
#                   smpstress runs (SBI HSM stop and start)
#   STRESS_JOBS     boots at once (default 2)
#
# Logs of failed boots are kept in build/stress/fail-*.log.
set -eu

SMP_LIST=${STRESS_SMP:-1 2 3 4 8}
ROUNDS=${STRESS_ROUNDS:-3}
SECS=${STRESS_SECS:-20}
MEM=${STRESS_MEM:-1G}
MB=${STRESS_MB:-0}
HOTPLUG=${STRESS_HOTPLUG:-0}
HP_ROUNDS=${STRESS_HOTPLUG_ROUNDS:-30}
JOBS=${STRESS_JOBS:-2}
RISCV_CC=${RISCV_CC:-riscv64-linux-gnu-gcc}
OUT=build/stress

die() { echo "linux-stress: $*" >&2; exit 1; }
command -v "$RISCV_CC" >/dev/null || die "$RISCV_CC not found (a riscv64 Linux C compiler)"
mkdir -p "$OUT"
"$RISCV_CC" -O2 -static -pthread -Wall -o "$OUT/smpstress" tests/smpstress.c
# One plain run first downloads and checks the kernel and busybox
[ -f build/linux/Image ] || tests/linux-shell.sh >/dev/null

# boot ID SMP: one boot in its own work directory -> a result line
boot() {
    w=$OUT/$1
    mkdir -p "$w"
    cp build/linux/*.deb build/linux/Image "$w/" 2>/dev/null || true
    wl="smpstress $SECS $MB"
    if [ "$HOTPLUG" = 1 ] && [ "$2" -gt 1 ]; then
        wl="$wl & for i in \$(seq $HP_ROUNDS); do for c in \$(seq $(($2 - 1))); do echo 0 > /sys/devices/system/cpu/cpu\$c/online; echo 1 > /sys/devices/system/cpu/cpu\$c/online; done; done; wait"
    fi
    start=$(date +%s)
    ok=1
    LINUX_WORK=$w LINUX_SMP=$2 LINUX_MEM=$MEM LINUX_EXPECT_HARTS=$2 \
        LINUX_TIMEOUT=$((SECS * 4 + 300)) LINUX_EXTRA=$OUT/smpstress \
        LINUX_WORKLOAD="$wl" tests/linux-shell.sh > "$w/out.txt" 2>&1 || ok=0
    log=$w/linux-shell.log
    # (kernel messages can land in the middle of the line: cut them off)
    result=$(grep -a 'smpstress: \(PASS\|FAIL\)' "$log" 2>/dev/null | head -n 1 | tr -d '\r' | sed 's/ *\[ *[0-9.]*\].*//')
    case $result in "smpstress: PASS"*) ;; *) ok=0 ;; esac
    bad=$(grep -a -E 'failed \(error|Oops|BUG:|WARNING:|rcu_.*stall|Kernel panic' "$log" 2>/dev/null | head -n 1 | tr -d '\r')
    [ -z "$bad" ] || ok=0
    secs=$(( $(date +%s) - start ))
    if [ $ok = 1 ]; then
        echo "ok    smp=$2 ${secs}s ${result#smpstress: PASS }"
    else
        keep=$OUT/fail-$1-$(date +%s).log
        cp "$log" "$keep" 2>/dev/null || cp "$w/out.txt" "$keep"
        echo "FAIL  smp=$2 ${secs}s ${result:-$(grep FAIL "$w/out.txt" | head -n 1)} ${bad} (log: $keep)"
    fi
}

results=$OUT/results.txt
: > "$results"
n=0
for r in $(seq "$ROUNDS"); do
    for smp in $SMP_LIST; do
        boot "job$((n % JOBS))" "$smp" | tee -a "$results" &
        n=$((n + 1))
        [ $((n % JOBS)) -ne 0 ] || wait
    done
done
wait
pass=$(grep -c '^ok' "$results" || true)
fail=$(grep -c '^FAIL' "$results" || true)
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
