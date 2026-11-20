# intel-thc-hid: upstream history and patch series

Only `drivers/hid/intel-thc-hid/` and `Documentation/hid/intel-thc-hid.rst`
are carried here. That keeps a rebase onto a newer kernel down to
re-importing those two paths, and lets `git format-patch` produce exactly
what gets sent.

## Branches

| branch | what it is |
|---|---|
| `main` | every series that is in use, applied in order: `core-b`, `core-a`, `ipts` |
| `base` | the upstream files as of Linux 7.4-rc3 (`aebb72744`), nothing applied |
| `core-b` | DMA lifetime series, latest revision (5 commits) |
| `core-a` | PRD tables under a drain, latest revision (2 commits) |
| `rawlock` | raw spinlocks for the THC registers (1 commit) |
| `ipts` | the IPTS port support (6 commits: one in the core, five in the new directory) |
| `stack` | `core-b` + `core-a` on one line, as they have to be applied |

`core-a`, `core-b`, `rawlock` and `ipts` each branch off `base`
independently: the series touch disjoint files, so every one of them applies
to the baseline on its own. `stack` exists because the IPTS series is written
against a tree that already has both core fixes in it, even though its own
hunks do not overlap with theirs.

`main` is `stack` with the IPTS patches on top, so its IPTS code is identical
to what the `ipts` branch and `ipts-v10` carry; it is the tree to build.
`rawlock` is a separate proposal and is not part of it.

The first IPTS commit is in the core rather than in the new directory: it adds
the port calls the backend needs, and drops two warnings from
`thc_interrupt_quiesce()`. That is why `ipts` is no longer purely additive, and
why stacking it on `stack` takes one hand merge (see [Rebuilding](#rebuilding)).

## Tags

Every revision that was actually posted is tagged, oldest first:

    core-b-v1    2026-11-16   first public posting
    core-a-v7    2026-11-17
    core-b-v2    2026-11-18
    ipts-v7      2026-11-18
    ipts-v8      2026-11-18
    rawlock-v1   2026-11-18
    ipts-v9      2026-11-19
    rawlock-v2   2026-11-19
    ipts-v10     not posted yet

A tag points at the tree that was sent, so `git diff ipts-v9 ipts-v10`
is the delta that v10 asks reviewers to look at again.

## Producing a series

    git format-patch base..ipts          # the six IPTS patches
    git format-patch base..core-b        # the five DMA lifetime patches
    git format-patch base..core-a        # the two PRD patches on their own
    git format-patch core-b..stack       # the two PRD patches as stacked

The `--base=` trailer records which commit the series was made against;
pass `--base=base` so that it points at the Linux 7.4-rc3 import.

## Building the modules

Only this directory is carried, so building needs a full kernel source tree
for everything else:

    tools/build.sh                # builds main
    tools/build.sh ipts           # or any other branch or tag

`KERNEL` defaults to `~/Debian/kbuild/linux-7.4-rc2`, `OUT` to `out/`, and
`JOBS` to `nproc`. The ref is exported into a scratch copy and built as an
external module, so the kernel tree is never written to. The four modules
land in `out/`:

    intel-thc.ko  intel-quickspi.ko  intel-quicki2c.ko  intel-ipts.ko

The four config symbols are passed on the make command line because the
in-tree Makefile selects its objects with `obj-$(CONFIG_...)`; that is all it
takes to build this directory out of tree, and nothing has to be copied into
the kernel.

To load them:

    tools/build.sh -i             # needs root; refuses if the release is not the running one
    modprobe intel-ipts

Two things to remember:

* `CONFIG_MODVERSIONS` is off in the kernels this is tested on, so symbols are
  matched by name only. All four modules have to come from the same build, or
  they load and then use the wrong structure offsets.
* `intel-quickspi` and `intel-quicki2c` have no PCI ID for TGL, so on a TGL
  machine only `intel-ipts` binds; their patches can only be reviewed there,
  not run.

## Rebuilding

`tools/rebuild.sh` regenerates every single-series branch from the patch
files and retags it. It expects the series directories to be where the
kernel work area keeps them; override with `IPTS=` and `CORE=`.

`stack` and `main` are not rebuilt by that script: `stack` is `core-b` plus
`git cherry-pick core-a~2..core-a`, and three hunks in
`intel-thc/intel-thc-dma.c` need a hand every time, because both series
touch `thc_dma_unconfigure()`, the stop path inside
`thc_wait_for_dma_pause()`, and `thc_swdma_read_start()`:

* `thc_dma_unconfigure()`: take the `guard(prd_lock)` from `core-a` and
  keep the `thc_clear_start_bits()` + drain loop from `core-b`.
* `thc_wait_for_dma_pause()`: keep the `core-a` lock and the
  `is_started` store, but write with the `ctrl_bit` the switch already
  selected rather than the `core-a` hard-coded
  `THC_M_PRT_READ_DMA_CNTRL_START`.
* `thc_swdma_read_start()`: the three-argument `thc_wait_for_dma_pause()`
  from `core-b` plus the `THC_SWDMA` channel that `core-a` added.

`main` is that result with `git cherry-pick base..ipts` on top. One hunk needs
a hand: the slow SPI gears that `core-a` adds and the port calls that `ipts`
adds are inserted at the same place, just after `thc_port_select()`. Keep both,
gears first.
