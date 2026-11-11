# NX769J offline Kubuntu candidate

This build is an **offline, experimental candidate**, not a tested phone image.
No flashing, booting, partitioning, recovery, or device-side action is part of
the workflow. A successful build does not prove that the NX769J boots or that
its storage, USB, network, display, touch, GPU, audio, or PMU works on Linux.

The code reuses the public Y700 rootfs-builder scripts pinned at
`4f87bbc51c6e8ab4434d421bd16fac3f7a8cb1d3`, fetched by content hash at build
time. It does not ship the Y700 boot image, firmware, audio policy, camera,
haptics, sensor packages, screen mode, or root-on-`userdata` default. The
root filesystem has its own generated UUID and is not assigned to a phone
partition or A/B slot. Original Y700 source and workflow remain upstream.

## Inputs and outputs

The manual GitHub Actions workflow `NX769J offline candidate` runs separate
ARM64 kernel and rootfs jobs. It expects the kernel job to produce `Image`,
`nx769j.dtb`, `modules.tar.zst`, matching `perf`, and `kernel-manifest.json`.
The rootfs job installs the modules and perf into an Ubuntu 26.04 arm64 KDE
image, verifies its UUID, installed packages, account policy, kernel release,
and perf startup, then uploads a zstd-compressed image, manifests and hashes.
Artifacts expire after seven days. They are accessible to people with access
to the public repository and must not contain device-specific firmware,
calibration, identifiers, boot images or secrets.

`FEX` build tools and development dependencies are included. An upstream FEX
source reference is recorded, but **FEX executables and the x86 guest rootfs
are not installed**. FEX, its JIT symbols and `perf` sampling are untested on
the target. Package/version selection and successful compilation do not prove
that GPU-accelerated rendering or any hardware feature works on the phone.

The `developer` and `root` passwords are locked, SSH root login and password
authentication are disabled, and desktop autologin is off. This public image
contains no default usable password or SSH key. Before any separate future
device experiment, a user must provision an appropriate local-only access
method; this project does not provide a flashing procedure.

## Building and validation

Only dispatch the workflow when the NX769J board source is ready for an
offline candidate build. The standard GitHub runner has limited disk space;
each job checks its available space, and out-of-space failures require a
reviewed solution rather than silently selecting a paid runner. No Release is
created. Rootfs building needs an isolated ARM64 Linux builder with root,
loop-mount, and chroot capabilities; it must never be run on the phone.

The original Y700 rootfs scripts are fetched individually at the pinned
commit, checked against SHA-256 hashes, and patched with
`patches/nx769j/y700-rootfs-generic.patch`. The patch leaves upstream Y700
defaults intact while allowing the NX candidate to disable its hardware
overlays and fixed display geometry. Kernel and rootfs artifacts are checked
separately; `perf --version` is only a userspace startup check.

No bootable `boot.img`, firmware payload, or deployment commands are included.
An NX769J-specific boot bundle requires its own matching Android image format
and verified private inputs; see `NX769J-STOCK-OTA-AUDIT.md` for the current
evidence and unresolved deployment gates.
