#!/bin/bash
# Build the combined core stack (B v2 + A v7) out of the history repo,
# then put the build tree back the way it was.
set -u
T=$HOME/Debian/kbuild/linux-7.4-rc2
S=$HOME/Debian/kernel/intel-thc-hid

cd "$S" || exit 1
git checkout -q stack || exit 1

cp -a "$S/drivers/hid/intel-thc-hid/." "$T/drivers/hid/intel-thc-hid/" || exit 1

cd "$T" || exit 1
TMPDIR=$HOME/Debian/kbuild/.tmpdir make -j8 M=drivers/hid/intel-thc-hid 2>&1 | \
	grep -vE "safe-delete|^  CC|^  LD|^  AR"
rc=${PIPESTATUS[0]}

git checkout -- drivers/hid/intel-thc-hid
exit "$rc"
