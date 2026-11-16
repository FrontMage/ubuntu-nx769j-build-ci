# NX769J Kernel Configuration, Device Tree & Patches

The runtime kernel targets mainline **Linux 7.1.1** built from [`GUF296/linux`](https://github.com/GUF296/linux) (`TB321FU-7.1.1`, commit `5df8e852ea722929f5359a5ef28ebcec0c4443fd`).

> **Important architectural finding:** Zero out-of-tree `.ko` kernel module source hacks are needed for our hardware fixes. Every stock `.ko` built cleanly from the tree works unmodified; hardware enablement lives cleanly in the Device Tree (`kernel/dts/`) and userspace/topology layers.

## Directory Contents

| File | Description |
|---|---|
| `nx769j.config` | Kernel Kconfig fragment (enables `DM_MOD`, `BLK_DEV_DM`, `PWRSEQ_QCOM_WCN`, `CUSE`, `DRM_MSM`, `SND_SOC_SC8280XP`, `PWM_FAN`, `QCOM_BATTMGR`, `INPUT_PM8941_PWRKEY`, etc.) |
| `patches/0001-dt-bindings-arm-qcom-add-nubia-nx769j.patch` | Adds `nubia,nx769j` compatible string to ARM64 qcom DT bindings |
| `patches/0002-arm64-dts-qcom-add-nubia-nx769j-to-makefile.patch` | Registers `sm8650-nubia-nx769j.dtb` in `arch/arm64/boot/dts/qcom/Makefile` |
| `dts/sm8650-nubia-nx769j-base.dts` | Upstream base NX769J DTS compiled into `nx769j.dtb` by `scripts/build-kernel.sh` |
| `dts/sm8650-nubia-nx769j-linux.dts` | Full decompiled/patched production DTS (v23) with all hardware fixes applied |
| `dts/nx769j-v23-hardware.patch` | Clean diff between stock `nx769j.dtb` decompiled DTS and production `v23` DTS |
| `dts/patch_nx769j_dtb.py` | Reproducible Python DTB patcher (also injects configurable `local-bd-address`) |
| `scripts/build-kernel.sh` | Offline arm64 Linux kernel + modules + perf + patched DTB builder |

## Reproducing the Kernel Build (ARM64 Linux Host)

```bash
# Requires native aarch64 Linux host (or container), static busybox, zstd, dtc
bash kernel/scripts/build-kernel.sh out/nx769j-kernel
```

Outputs in `out/nx769j-kernel/`:
- `Image` (`7.1.1-nx769j+` EFI kernel image with embedded rescue busybox initramfs)
- `nx769j.dtb` (unpatched base DTB)
- `nx769j-linux.dtb` (if `dtc` is installed, auto-patched via `kernel/dts/patch_nx769j_dtb.py`)
- `modules.tar.zst` (verified module tarball)
- `perf` (static-python/llvm-free AArch64 `perf` binary)

Alternatively, if you have GitHub CLI (`gh`) and want the exact prebuilt kernel bundle while retention lasts:

```bash
gh run download 43067542050 -R FrontMage/ubuntu-nx769j-build-ci -n nx769j-kernel-untested -D out/nx769j-kernel
```

## Patching Any Existing `nx769j.dtb` Offline

```bash
python3 kernel/dts/patch_nx769j_dtb.py \
  --in /path/to/nx769j.dtb \
  --bt-mac 02:00:00:76:9a:01 \
  --out nx769j-linux.dtb
```
