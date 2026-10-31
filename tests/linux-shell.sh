#!/bin/sh
# Boot a real Linux kernel on the RISC-V firmware, all the way to a shell.
#
#   make linux-shell                 run the check (not part of make test)
#   tests/linux-shell.sh --interactive   same image, but you get the shell
#
# Nothing here is checked into the repo. The script downloads two pinned
# Ubuntu 24.04 packages (the riscv64 generic kernel and busybox-static),
# checks their SHA-256, and keeps them in build/linux/. It then builds a
# tiny initramfs: busybox plus a short /init that mounts /proc, /sys and
# /dev and starts a shell on the serial console.
#
# The check passes when, in order:
#   1. the kernel reaches the shell prompt,
#   2. a command typed at the prompt runs (echo prints linux-shell=42),
#   3. poweroff makes QEMU exit by itself (SBI SRST shutdown, status 0).
#
# LINUX_KERNEL picks the kernel: 6.8 (default, Ubuntu 24.04, built for
# at most 32 harts) or 6.14 (Ubuntu 25.04, used by make linux-big for 64).
# LINUX_TIMEOUT (seconds, default 300) bounds the whole run,
# LINUX_SMP (default 1) sets the number of harts, and LINUX_MEM
# (default 512M) the guest RAM. LINUX_EXPECT_HARTS, when set, also
# requires the shell's banner to report that many harts online.
# LINUX_NUMA (default 1) splits the harts and the RAM evenly into that
# many NUMA nodes (QEMU makes each one a socket with its own CLINT), and
# then requires Linux to bring up that many nodes.
# The serial log is kept in build/linux/linux-shell.log.
#
# For tests/linux-stress.sh: LINUX_WORK (default build/linux) is the
# work directory, so several runs can go at once; LINUX_EXTRA lists
# files copied into the initramfs /bin; LINUX_WORKLOAD is a command line
# typed at the prompt after the first command, and the run waits for it
# to finish before poweroff.

set -eu

QEMU_RISCV=${QEMU_RISCV:-qemu-system-riscv64}
TIMEOUT=${LINUX_TIMEOUT:-300}
SMP=${LINUX_SMP:-1}
MEM=${LINUX_MEM:-512M}
EXPECT_HARTS=${LINUX_EXPECT_HARTS:-}
NUMA=${LINUX_NUMA:-1}
FIRMWARE=riscv/squirrel-bios.bin
WORK=${LINUX_WORK:-build/linux}
WORKLOAD=${LINUX_WORKLOAD:-}
EXTRA=${LINUX_EXTRA:-}
MIRROR=${UBUNTU_PORTS:-http://ports.ubuntu.com/ubuntu-ports}
# Where a release's packages move once it reaches end of life
OLD_MIRROR=${UBUNTU_OLD_RELEASES:-http://old-releases.ubuntu.com/ubuntu}

# Pinned to a release pocket, which the mirror keeps for the life of the
# release (and old-releases after that). The SHA-256 values come from
# its Packages index.
case ${LINUX_KERNEL:-6.8} in
6.8)    # noble (24.04): NR_CPUS=32
    KERNEL_VER=6.8.0-31-generic
    KERNEL_DEB=pool/main/l/linux-riscv/linux-image-6.8.0-31-generic_6.8.0-31.31.1_riscv64.deb
    KERNEL_SHA=dffba9df38826a26320c41dac556bd5bda2b447c8ea71433176bde601117d6e9 ;;
6.14)   # plucky (25.04): brings up 64 harts. Later Ubuntu kernels need
        # the RVA23 profile and "riscv,isa-extensions", which QEMU 8.2's
        # default hart and device tree do not give them.
    KERNEL_VER=6.14.0-13-generic
    KERNEL_DEB=pool/main/l/linux-riscv/linux-image-6.14.0-13-generic_6.14.0-13.13.2_riscv64.deb
    KERNEL_SHA=27b7a8fbb170203650459565bb187a9ed5275f4cd1299fabc82622d5ce27ad1c ;;
*)  echo "linux-shell: LINUX_KERNEL must be 6.8 or 6.14" >&2; exit 1 ;;
esac
BUSYBOX_DEB=pool/main/b/busybox/busybox-static_1.36.1-6ubuntu3_riscv64.deb
BUSYBOX_SHA=c567d8dc47320ad4b17cc20a5a91d583b76a35da618d3b389f1770a358fafd41

interactive=0
[ "${1:-}" = "--interactive" ] && interactive=1

die() { echo "linux-shell: $*" >&2; exit 1; }

[ -f "$FIRMWARE" ] || die "$FIRMWARE is missing; run make riscv first"
command -v "$QEMU_RISCV" >/dev/null || die "$QEMU_RISCV not found"
command -v cpio >/dev/null || die "cpio not found"
mkdir -p "$WORK"

# fetch POOL_PATH SHA256: download once into $WORK, verify every time
fetch() {
    file=$WORK/$(basename "$1")
    if [ ! -f "$file" ]; then
        echo "linux-shell: downloading $(basename "$1")"
        curl -fsSL -o "$file.part" "$MIRROR/$1" ||
            curl -fsSL -o "$file.part" "$OLD_MIRROR/$1" ||
            die "download of $1 failed"
        mv "$file.part" "$file"
    fi
    echo "$2  $file" | sha256sum -c --quiet - || {
        rm -f "$file"
        die "$(basename "$1") does not match its pinned SHA-256"
    }
}

# unpack DEB DIR: extract a .deb without needing root
unpack() {
    rm -rf "$2"
    mkdir -p "$2"
    if command -v dpkg-deb >/dev/null; then
        dpkg-deb -x "$1" "$2"
    else
        (cd "$2" && ar x "$OLDPWD/$1" && tar xf data.tar.* && rm -f data.tar.* control.tar.* debian-binary)
    fi
}

fetch "$KERNEL_DEB" "$KERNEL_SHA"
fetch "$BUSYBOX_DEB" "$BUSYBOX_SHA"

# Kernel: the package ships the Image compressed as vmlinuz, either
# gzipped (6.8) or as an EFI zboot image (6.14: "zimg" at offset 4, the
# payload offset and size at 8 and 12, the method at 24). QEMU's -kernel
# on riscv wants it raw.
image=$WORK/Image-$KERNEL_VER
if [ ! -f "$image" ]; then
    unpack "$WORK/$(basename "$KERNEL_DEB")" "$WORK/kernel-deb"
    vmlinuz=$WORK/kernel-deb/boot/vmlinuz-$KERNEL_VER
    [ -f "$vmlinuz" ] || die "no vmlinuz-$KERNEL_VER in the kernel package"
    if gzip -t "$vmlinuz" 2>/dev/null; then
        gzip -dc "$vmlinuz" > "$image.part"
    elif [ "$(dd if="$vmlinuz" bs=1 skip=4 count=4 2>/dev/null)" = zimg ]; then
        off=$(od -An -tu4 -j8 -N4 "$vmlinuz" | tr -d ' ')
        size=$(od -An -tu4 -j12 -N4 "$vmlinuz" | tr -d ' ')
        method=$(dd if="$vmlinuz" bs=1 skip=24 count=4 2>/dev/null)
        case $method in
        zstd) command -v zstd >/dev/null || die "zstd not found (needed for $KERNEL_VER)"
              unz="zstd -dcq" ;;
        gzip) unz="gzip -dc" ;;
        *)    die "vmlinuz-$KERNEL_VER is compressed with $method, not gzip or zstd" ;;
        esac
        tail -c +$((off + 1)) "$vmlinuz" | head -c "$size" | $unz > "$image.part"
    else
        cp "$vmlinuz" "$image.part"
    fi
    mv "$image.part" "$image"
    rm -rf "$WORK/kernel-deb"
fi

# Initramfs: busybox and an /init script. Rebuilt every run; it is small.
root=$WORK/initramfs
unpack "$WORK/$(basename "$BUSYBOX_DEB")" "$WORK/busybox-deb"
rm -rf "$root"
mkdir -p "$root/bin" "$root/dev" "$root/proc" "$root/sys" "$root/tmp"
cp "$WORK/busybox-deb/usr/bin/busybox" "$root/bin/busybox"
rm -rf "$WORK/busybox-deb"
ln -s busybox "$root/bin/sh"
for f in $EXTRA; do cp "$f" "$root/bin/"; done
# /init runs before /dev/console exists (an unprivileged build cannot
# make device nodes), so it mounts devtmpfs and then reattaches its own
# stdin, stdout and stderr to the console
cat > "$root/init" <<'EOF'
#!/bin/sh
/bin/busybox mount -t devtmpfs devtmpfs /dev
exec 0</dev/console 1>/dev/console 2>&1
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "squirrel: busybox shell on $(uname -sr), $(grep -c ^processor /proc/cpuinfo) hart(s)"
export PS1='squirrel# ' HOME=/ PATH=/bin
while true; do
    setsid cttyhack sh
done
EOF
chmod 755 "$root/init"
initrd=$WORK/initramfs.cpio.gz
(cd "$root" && find . | cpio -o -H newc -R 0:0 --quiet) | gzip -9 > "$initrd"

# The RAM is a backend with reserve=off, so QEMU maps it without asking
# the host to set the whole size aside. A 64G guest then starts on a
# host with much less, as long as Linux does not touch all of it.
# With NUMA each node gets its own backend and an equal share of harts.
if [ "$NUMA" -le 1 ]; then
    set -- -M virt,memory-backend=ram \
        -object "memory-backend-ram,id=ram,size=$MEM,reserve=off"
else
    case $MEM in
    *G) unit=G ;;
    *M) unit=M ;;
    *)  die "LINUX_MEM must end in G or M when LINUX_NUMA is set" ;;
    esac
    total=${MEM%?}
    [ $((total % NUMA)) -eq 0 ] && [ $((SMP % NUMA)) -eq 0 ] ||
        die "LINUX_MEM and LINUX_SMP must divide evenly into $NUMA nodes"
    set -- -M virt
    n=0
    while [ "$n" -lt "$NUMA" ]; do
        first=$((n * SMP / NUMA))
        last=$(((n + 1) * SMP / NUMA - 1))
        set -- "$@" -object "memory-backend-ram,id=ram$n,size=$((total / NUMA))$unit,reserve=off" \
            -numa "node,nodeid=$n,cpus=$first-$last,memdev=ram$n"
        n=$((n + 1))
    done
fi
set -- "$QEMU_RISCV" "$@" -m "$MEM" -smp "$SMP" -no-reboot \
    -bios "$FIRMWARE" -kernel "$image" -initrd "$initrd" \
    -append "console=ttyS0 earlycon panic=-1"

if [ "$interactive" -eq 1 ]; then
    echo "linux-shell: Ctrl-A X quits QEMU; poweroff shuts Linux down"
    exec "$@" -nographic
fi

# Scripted run: QEMU's serial port is a pair of FIFOs. Its output is
# copied to a log, and the steps below type at the prompt.
log=$WORK/linux-shell.log
pipe=$WORK/serial
rm -f "$log" "$pipe.in" "$pipe.out"
mkfifo "$pipe.in" "$pipe.out"
"$@" -display none -monitor none \
    -chardev "pipe,id=con,path=$pipe" -serial chardev:con \
    >/dev/null 2>"$WORK/qemu.err" &
qemu=$!
cat "$pipe.out" > "$log" &
reader=$!
# Read-write, so the open does not block if QEMU fails to start
exec 3<>"$pipe.in"
cleanup() {
    exec 3>&-
    kill "$qemu" "$reader" 2>/dev/null || true
    rm -f "$pipe.in" "$pipe.out"
}
trap cleanup EXIT
deadline=$(( $(date +%s) + TIMEOUT ))

running() {
    case $(ps -o stat= -p "$qemu" 2>/dev/null) in
    ""|Z*) return 1 ;;
    esac
}

fail() {
    echo "FAIL  linux-shell ($1)"
    tail -n 40 "$log" | sed 's/^/      | /'
    [ -s "$WORK/qemu.err" ] && sed 's/^/      qemu: /' "$WORK/qemu.err"
    echo "      full log: $log"
    exit 1
}

# wait_for TEXT WHAT: wait until TEXT is in the log
wait_for() {
    while ! grep -qF -- "$1" "$log"; do
        running || fail "QEMU exited before $2"
        [ "$(date +%s)" -lt "$deadline" ] || fail "no $2 within ${TIMEOUT}s"
        sleep 1
    done
    echo "ok    $2"
}

wait_for "squirrel# " "shell prompt"
# /init prints the hart count just before the first prompt
if [ -n "$EXPECT_HARTS" ]; then
    online=$(sed -n 's/^squirrel: .*, \([0-9]*\) hart(s).*/\1/p' "$log" | head -n 1)
    [ "$online" = "$EXPECT_HARTS" ] ||
        fail "${online:-no} of $EXPECT_HARTS harts online"
    echo "ok    $EXPECT_HARTS harts online"
fi
if [ "$NUMA" -gt 1 ]; then
    grep -qa "smp: Brought up $NUMA nodes" "$log" ||
        fail "Linux did not bring up $NUMA NUMA nodes"
    echo "ok    $NUMA NUMA nodes"
fi
# The shell's own echo of this line shows $((40+2)), not 42
printf 'echo linux-shell=$((40+2))\n' >&3
wait_for "linux-shell=42" "command at the prompt"
# An optional workload, typed as one line; it must end by printing
# workload=done (the shell echo shows workload=do""ne, so it is split)
if [ -n "$WORKLOAD" ]; then
    printf '%s; echo workload=do""ne\n' "$WORKLOAD" >&3
    wait_for "workload=done" "workload"
fi
printf 'poweroff -f\n' >&3
while running; do
    [ "$(date +%s)" -lt "$deadline" ] || fail "QEMU still running after poweroff"
    sleep 1
done
wait "$qemu" && status=0 || status=$?
[ "$status" -eq 0 ] || fail "QEMU exited with status $status after poweroff"
echo "ok    poweroff (QEMU exited with status 0)"
echo "PASS  linux-shell"
