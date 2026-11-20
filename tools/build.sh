#!/bin/sh
# Build the THC modules from a ref of this repository.
#
#   tools/build.sh [-i] [ref]
#
#   ref      branch or tag to build, default: main
#   -i       install the result into /lib/modules/$(uname -r)/updates
#
# Environment:
#   KERNEL   kernel source tree to build against
#            (default ~/Debian/kbuild/linux-7.4-rc2)
#   OUT      where the .ko files are collected (default <repo>/out)
#   JOBS     parallel make jobs (default: nproc)
#
# Only drivers/hid/intel-thc-hid is carried here, so the rest of the kernel has
# to come from a full source tree. The in-tree Makefile selects its objects
# with obj-$(CONFIG_...), which builds as an external module once those four
# symbols are passed on the command line; nothing is copied into the tree.
set -eu

install=
if [ "${1:-}" = "-i" ]; then
	install=1
	shift
fi

ref=${1:-main}
repo=$(cd "$(dirname "$0")/.." && pwd)
kernel=${KERNEL:-$HOME/Debian/kbuild/linux-7.4-rc2}
out=${OUT:-$repo/out}
jobs=${JOBS:-$(nproc)}

[ -f "$kernel/Makefile" ] || {
	echo "KERNEL does not point at a kernel tree: $kernel" >&2
	exit 1
}
[ -f "$kernel/Module.symvers" ] || echo "warning: $kernel/Module.symvers is missing, run 'make modules_prepare' there" >&2

# Objects are written next to the sources, so build in a scratch copy rather
# than in the checkout; /tmp is a small tmpfs on some of these boxes.
work=$(mktemp -d "${TMPDIR:-$HOME}/thc-build.XXXXXX")
trap 'rm -rf "$work"' EXIT

git -C "$repo" archive "$ref" | tar -x -C "$work"
mkdir -p "$out"
rm -f "$out"/*.ko

echo "building $ref against $kernel"
make -C "$kernel" M="$work/drivers/hid/intel-thc-hid" \
	CONFIG_INTEL_THC_HID=m \
	CONFIG_INTEL_QUICKSPI=m \
	CONFIG_INTEL_QUICKI2C=m \
	CONFIG_INTEL_IPTS=m \
	-j"$jobs" modules

find "$work/drivers/hid/intel-thc-hid" -name '*.ko' -exec cp {} "$out/" \;

release=$(make -s -C "$kernel" kernelrelease)

magic() {
	if command -v strings >/dev/null 2>&1; then
		strings "$1" | sed -n 's/^vermagic=//p'
	else
		echo "unknown"
	fi
}

echo
echo "built against $release, running $(uname -r):"
for ko in "$out"/*.ko; do
	printf '  %-20s %s\n' "$(basename "$ko")" "$(magic "$ko")"
done

if [ -z "$install" ]; then
	echo
	echo "not installed; 'tools/build.sh -i' copies them to /lib/modules/$release/updates"
	exit 0
fi

[ "$(id -u)" = 0 ] || {
	echo "installing needs root; rerun with sudo" >&2
	exit 1
}
if [ "$release" != "$(uname -r)" ]; then
	echo "refusing to install $release modules into $(uname -r)" >&2
	exit 1
fi

dir=/lib/modules/$release/updates
install -d "$dir"
install -m 644 "$out"/*.ko "$dir/"
depmod -a "$release"
echo "installed into $dir, 'modprobe intel-ipts' to load"
