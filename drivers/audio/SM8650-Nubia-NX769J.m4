# Copyright (c) 2023, Linaro Limited
# SPDX-License-Identifier: BSD-3-Clause
#
# AudioReach Topology for Nubia RedMagic 9 Pro (NX769J, SM8650)
# Target firmware path: /lib/firmware/qcom/sm8650/SM8650-Nubia-NX769J-tplg.bin
#
# Hardware Backends on NX769J:
#   - PRIMARY_MI2S_RX   (id=16)  -> Dual Awinic AW88261 SmartPAs (0x35 Left, 0x34 Right/Earpiece)
#                                   LPAIF, PRIMARY_MI2S, I2S_SD1 (gpio128 aux0_pcm_dout -> SmartPA SDI),
#                                   internal WS/BCLK master (gpio126 i2s0_sck 1.536 MHz, gpio129 i2s0_ws 48 kHz)
#                                   [Note: gpio127 i2s0_data0 / I2S_SD0 is aux0_pcm_din from SmartPA SDO!]
#   - RX_CODEC_DMA_RX_0 (id=113) -> LPASS RX Macro -> SoundWire SWR1 -> WCD9395 3.5mm Headphones
#   - TX_CODEC_DMA_TX_3 (id=120) -> LPASS TX Macro -> SoundWire SWR2 -> WCD9395 Mic1/Mic2/Headset Mic
#
# Frontend PCMs:
#   - MultiMedia1 (id=0) : Playback 48 kHz 16-bit 2ch (primary Headphones FE)
#   - MultiMedia2 (id=1) : Playback 48 kHz 16-bit 2ch (primary Stereo Speakers / Earpiece FE)
#   - MultiMedia3 (id=2) : Capture  48 kHz 16-bit 1..2ch (Analog Mics & 3.5mm Headset Mic FE)

include(`audioreach/audioreach.m4')
include(`audioreach/stream-subgraph.m4')
include(`audioreach/device-subgraph.m4')
include(`util/route.m4')
include(`audioreach/tokens.m4')

#
# Stream SubGraphs (Frontend PCMs)
#
dnl STREAM_SG_PCM_ADD(stream, sg_iid, perf_mode, dir, cont_id, cont_cap,
dnl                   stack_size, heap_id, pd_iid, pconv_iid, vol_iid, mfc_iid, log_iid, log_code,
dnl                   format, min_rate, max_rate, min_ch, max_ch)

dnl MultiMedia1 Playback (PCM 0)
STREAM_SG_PCM_ADD(audioreach/subgraph-stream-vol-playback.m4, `MultiMedia1',
	0x4001, APM_SG_PERF_MODE_LOW_POWER, APM_SUB_GRAPH_DIRECTION_TX,
	0x4001, APM_CONTAINER_CAP_ID_CD, 8192, APM_CONT_HEAP_DEFAULT,
	0x6001, 0x6002, 0x6003, 0x6004, 0x6005, 0x6006, 0x19af, 0,
	S16_LE, 48000, 48000, 2, 2)

dnl MultiMedia2 Playback (PCM 1)
STREAM_SG_PCM_ADD(audioreach/subgraph-stream-vol-playback.m4, `MultiMedia2',
	0x4002, APM_SG_PERF_MODE_LOW_POWER, APM_SUB_GRAPH_DIRECTION_TX,
	0x4002, APM_CONTAINER_CAP_ID_CD, 8192, APM_CONT_HEAP_DEFAULT,
	0x6010, 0x6011, 0x6012, 0x6013, 0x6014, 0x6015, 0x19af, 1,
	S16_LE, 48000, 48000, 2, 2)

dnl MultiMedia3 Capture (PCM 2)
STREAM_SG_PCM_ADD(audioreach/subgraph-stream-capture.m4, `MultiMedia3',
	0x4003, APM_SG_PERF_MODE_LOW_POWER, APM_SUB_GRAPH_DIRECTION_RX,
	0x4003, APM_CONTAINER_CAP_ID_CD, 8192, APM_CONT_HEAP_DEFAULT,
	0x6020, 0x6021, 0x6022, 0x6023, 0x6024, 0x19af, 2,
	S16_LE, 48000, 48000, 1, 2)

#
# Device SubGraphs (Backend DAIs)
#
dnl PRIMARY_MI2S_RX (id=16) -> Dual AW88261 SmartPAs on i2s0 (I2S_SD1 = gpio128 aux0_pcm_dout, internal WS)
DEVICE_SG_I2S_ADD(audioreach/subgraph-device-i2s-playback.m4, `Primary MI2S',
	0x4006, APM_SG_PERF_MODE_LOW_POWER, APM_SUB_GRAPH_DIRECTION_RX,
	0x4006, APM_CONTAINER_CAP_ID_EP, 8192, APM_CONT_HEAP_DEFAULT,
	PRIMARY_MI2S_RX, 0x6060, 0x19ab, 0x6061,
	LPAIF, I2S_INTF_TYPE_PRIMARY, I2S_SD1, CONFIG_I2S_WS_SRC_INTERNAL)

dnl RX_CODEC_DMA_RX_0 (id=113) -> LPASS RX Macro -> WCD9395 3.5mm Headphones
DEVICE_SG_CODEC_DMA_ADD(audioreach/subgraph-device-codec-dma-playback.m4, `RX_CODEC_DMA_RX_0',
	0x4007, APM_SG_PERF_MODE_LOW_POWER, APM_SUB_GRAPH_DIRECTION_RX,
	0x4007, APM_CONTAINER_CAP_ID_EP, 8192, APM_CONT_HEAP_DEFAULT,
	RX_CODEC_DMA_RX_0, 0x6070, 0x19ab, 0x6071, 0x6072,
	LPAIF_AUD, LPASS_CDC_DMA_RX0, 1)

dnl TX_CODEC_DMA_TX_3 (id=120) -> LPASS TX Macro -> WCD9395 Analog & Headset Mics
DEVICE_SG_CODEC_DMA_ADD(audioreach/subgraph-device-codec-dma-capture.m4, `TX_CODEC_DMA_TX_3',
	0x4009, APM_SG_PERF_MODE_LOW_POWER, APM_SUB_GRAPH_DIRECTION_TX,
	0x4009, APM_CONTAINER_CAP_ID_EP, 8192, APM_CONT_HEAP_DEFAULT,
	TX_CODEC_DMA_TX_3, 0x6090, 0x6091, 0x19ab,
	LPAIF_VA, LPASS_CDC_DMA_TX3, 1)

#
# Frontend <-> Backend Mixers & Routes
#
AUDIOREACH_MIXER_DEVICE_SG_ROUTE_ADD(`PRIMARY_MI2S_RX', `Primary MI2S', PRIMARY_MI2S_RX,
	`MultiMedia1', 0, 0x4001, `stream0.logger1',
	`MultiMedia2', 1, 0x4002, `stream1.logger1')

AUDIOREACH_MIXER_DEVICE_SG_ROUTE_ADD(`RX_CODEC_DMA_RX_0', `RX_CODEC_DMA_RX_0', RX_CODEC_DMA_RX_0,
	`MultiMedia1', 0, 0x4001, `stream0.logger1',
	`MultiMedia2', 1, 0x4002, `stream1.logger1')

AUDIOREACH_MIXER_STREAM_SG_ROUTE_ADD(`MultiMedia3', 2, 0x4003, `stream2.logger1',
	`TX_CODEC_DMA_TX_3', TX_CODEC_DMA_TX_3, 0x4009, `device120.logger1')
