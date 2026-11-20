#!/bin/bash
# Rebuild every single-series branch from its patch files and retag it.
#
# The series themselves live outside this repository, in the kernel work
# area; IPTS= and CORE= override where they are looked up.
set -eu

IPTS=${IPTS:-$HOME/Debian/kernel/ithc/series}
CORE=${CORE:-$HOME/Debian/kernel/thc-core}

cd "$(dirname "$0")/.."

# When each revision was posted, so that rebuilding does not move the tags.
posted() {
	case $1 in
	core-b-v1)  echo 2026-11-16T20:49:47+0800 ;;
	core-a-v7)  echo 2026-11-17T18:50:37+0800 ;;
	core-b-v2)  echo 2026-11-18T12:53:34+0800 ;;
	ipts-v7)    echo 2026-11-18T13:39:11+0800 ;;
	ipts-v8)    echo 2026-11-18T14:28:29+0800 ;;
	rawlock-v1) echo 2026-11-18T14:47:57+0800 ;;
	ipts-v9)    echo 2026-11-19T16:01:55+0800 ;;
	rawlock-v2) echo 2026-11-19T09:13:11+0800 ;;
	ipts-v10)   echo 2026-11-19T22:56:32+0800 ;;
	esac
}

am_branch() {
	local branch=$1 tag=$2

	shift 2
	git checkout -q -B "$branch" base
	git am "$@"
	GIT_COMMITTER_DATE=$(posted "$tag") git tag -f -a "$tag" -m "$tag"
}

am_branch r/core-b-v1  core-b-v1  "$CORE"/series-dma-lifetime-v11/000[1-5]-*.patch
am_branch r/core-b-v2  core-b-v2  "$CORE"/series-dma-lifetime-v12/v2-000[1-5]-*.patch
am_branch r/core-a-v7  core-a-v7  "$CORE"/series-prd-v7/v7-000[12]-*.patch
am_branch r/rawlock-v1 rawlock-v1 "$CORE"/series-rawlock/*.patch
am_branch r/rawlock-v2 rawlock-v2 "$CORE"/series-rawlock-v2/*.patch
am_branch r/ipts-v7    ipts-v7    "$IPTS"/v7/v7-000[1-5]-*.patch
am_branch r/ipts-v8    ipts-v8    "$IPTS"/v8/v8-000[1-5]-*.patch
am_branch r/ipts-v9    ipts-v9    "$IPTS"/v9/v9-000[1-5]-*.patch
am_branch r/ipts-v10   ipts-v10   "$IPTS"/v10/v10-000[1-5]-*.patch

git branch -f core-b  r/core-b-v2
git branch -f core-a  r/core-a-v7
git branch -f rawlock r/rawlock-v2
git branch -f ipts    r/ipts-v10

echo "rebuilt; 'stack' and 'main' still need their hand-merged hunks (see README)"
