# NX769J Hardware Bringup & Driver Fixes Reference

This document catalogs every hardware quirk discovered on the RedMagic 9 Pro (`NX769J`, `SM8650`) under Mainline Linux and how each component in this repository fixes it.

---

## 1. Physical Stereo Speakers (`AW88261` + `PRIMARY_MI2S_RX` on `I2S_SD1`)

### Root Causes Fixed
1. **Reversed Data Pin Routing (`I2S_SD0` vs `I2S_SD1`)**:
   - On Nubia NX769J hardware, `gpio127` (`I2S_SD0`) is wired to amplifier IV-sense feedback (`DIN`), while **`gpio128` (`I2S_SD1`) is the actual audio data output (`DOUT` -> AW88261 `SDI`)**.
   - Mainline DT had `qcom,sd-lines` without `sd1-pins` output enable (`GPIO_OE=in`), AND upstream `SM8650-QRD-tplg.bin` set `PRIMARY_MI2S_RX` token `256` (`AR_TKN_U32_MODULE_SD_LINE_IDX`) to `1` (`SD0`) instead of `2` (`SD1`).
2. **ADSP Crash via Incorrect Multi-PCM Capture Topology**:
   - Simultaneously opening `pcm1p` + `pcm2c` with an mismatched subgraph triggers an ADSP (`remoteproc0`) crash, which also drops `qcom_battmgr` battery reporting (`0%`).
3. **`AW88261` Variant & Zero-CPU Auto Power Management**:
   - The live hardware silicon ID at `i2c@98c000` (`0x34` upper/right, `0x35` lower/left) reports **`chip_id = 0x2113` (`aw_pid_2113`)**, not standard `aw88261` IDs.
   - Earlier `inotify` polling on ALSA controls pinned 1 CPU core at 100%.

### Repository Fixes
- **`kernel/dts/patch_nx769j_dtb.py`**:
  - Configures `pri-mi2s-active-state` `sd1-pins` (`gpio128`, `output-low;`) so `GPIO_OE` is `out`, and keeps `gpio127` as `sd0-pins` input.
- **`drivers/audio/gen_nx769j_tplg.py` + `SM8650-Nubia-NX769J-tplg.bin`**:
  - Prunes unused subgraphs to prevent ADSP crashes (`MultiMedia1 Playback`, `MultiMedia2 Playback`, `MultiMedia3 Capture` + `PRIMARY_MI2S_RX`, `RX_CODEC_DMA_RX_0`, `TX_CODEC_DMA_TX_3`) and patches `device16.i2s_rx1` `AR_TKN_U32_MODULE_SD_LINE_IDX = 2` (`SD1`).
- **`drivers/audio/aw88261_init.py`**:
  - Blocks with **0.0% idle CPU** on ALSA kernel timer events (`SNDRV_TIMER_IOCTL_TREAD` on `/dev/snd/timer`), automatically powers on `0x34` / `0x35` with factory ACF register tuning when PCM streams run, and automatically mutes & powers down both SmartPAs cleanly ~1.5s after stream stop (verified with Chromium YouTube & `speaker-test`).
- **`drivers/audio/ucm2/` & `rootfs/overlay/etc/wireplumber/`**:
  - UCM2 `HiFi.conf` + WirePlumber hardware volume curves and `Modem` conflict avoidance.

---

## 2. Display, Headless GPU & Multitouch (`nx769j_fb_wl` + `Aquamarine`)

### Root Causes Fixed
- Mainline DT initially lacked panel bringup for the BOE RM692E5 CMD DSI panel while keeping UEFI/ABL's `simple-framebuffer` (`1116x2480`, `a8r8g8b8`, `0xe3940000`) active alongside `msm.separate_gpu_kms=1` (`renderD128` Adreno 750 without active KMS display scanout).
- Hyprland's `aquamarine 0.16.0` Wayland backend did not forward Linux evdev software touch events properly into headless Wayland sessions.

### Repository Fixes
- **`drivers/display/nx769j_fb_wl.c`**: High-throughput Wayland compositor host server bridging Hyprland's nested Wayland/DMA-BUF frames into `/dev/fb0` (`1116x2480` NEON/stride blitter), forwarding 10-slot Goodix `gt9916` capacitive touch, powerkey/panel blanking (`panel_power.sh` hook), and `nx769j_vkbd` input.
- **`drivers/display/patches/aquamarine-0.16.0-wayland-touch.patch`**: Enables nested Wayland touch slot (`wl_touch`) routing in `aquamarine 0.16.0`.
- **`drivers/display/nx769j_vkbd.c`**: Userspace `uinput` virtual keyboard helper.
- **`kernel/dts/patch_nx769j_dtb.py`**: Enables `oled-backlight` PWM and `display_panel` / `gt9916` touch nodes.

---

## 3. ADSP ChargerPD Battery & 6-Axis IMU (`remoteproc0` + FastRPC)

### Fixes
- **`firmware-extractors/stage_adsp_fw.py` & `stage_gyro_fw.py`**: Extract `adsp.mdt`, `adsp_dtb.mdt` (`battman` DT embedded), `battmgr.jsn`, `adspr.jsn`, `adsps.jsn`, and factory `persist/sensors` registry calibration directly from stock `modem_b` / `super` / `dsp_b` / `persist`.
- **`drivers/sensors/nx769j_hexagonrpcd.c` + `libhexagonrpc`**: Provides the ADSP Sensors PD FastRPC reverse-tunnel daemon (`hexagonrpcd.service`, `hexagonrpcd-rootpd.service`). Note `qrtr-ns.service` is masked because kernel `qrtr` already registers name service cleanly.
- **`drivers/sensors/nx769j_imu_daemon.c` + `nx769j-sensor-proxy`**: Subscribes to QSH (`service 400`) 6-axis accelerometer/gyroscope samples from ADSP, exposes `/dev/nx769j-imu` via CUSE + Unix state socket, and bridges orientation to `iio-sensor-proxy` and `omamobile-autorotate`.

---

## 4. Turbo Cooling Fan, Thermal Perf Profiles & Power Key

- **`drivers/thermal-fan/nx769j-fan.c` & DT thermal zones**: Drives RedMagic's built-in centrifugal cooling fan (`pwm-fan`) with auto thermal ramp + manual RPM/mode overrides (`nx769j-fan-ctl` + Omarchy QuickSettings widget).
- **`drivers/thermal-fan/nx769j_perf.c`**: Implements `Eco` / `Balanced` / `Diablo-Turbo` CPU cluster frequency policies + thermal scaling (`nx769j-perf.service`).
- **`drivers/power/nx769j_powerkey.c`**: Intercepts `pmic_pwrkey` events: short press toggles AMOLED blank/standby & wakes Wi-Fi/touch cleanly (masking `sleep.target` / `suspend.target` to avoid SM8650 S2idle hangs); long press cleanly initiates system poweroff/reboot.
- **`drivers/power/nx769j_charge_limit.py`**: Enforces configurable battery charge ceiling (`80%` default via `/sys/class/power_supply/qcom-battmgr-bat/charge_control_end_threshold` or USB input suspend).

---

## 5. Wi-Fi 7 & Bluetooth (`WCN7850`)

- `kernel/dts/patch_nx769j_dtb.py`: Patches `wcn7850-pmu` voltage/enable sequencing and injects a deterministic locally administered Bluetooth MAC (`local-bd-address`) into `&bluetooth` so BlueZ initializes `hci0` immediately on boot (`pwrseq-qcom-wcn.ko` loaded early in initrd).
- `rootfs/overlay/etc/NetworkManager/conf.d/20-nx769j-wifi.conf`: Configures MAC randomization off (`wifi.scan-rand-mac-address=no`) and power-save compatibility for `ath12k`.
