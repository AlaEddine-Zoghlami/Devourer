# PixelPilot APFPV integration — COMPLETE gsmenu feature coverage

Source: OpenIPC/sbc-groundstations package/pixelpilot/files/gsmenu.sh
Every gsmenu category mapped to a fork file. Mode toggle: rx_mode = wfb | apfpv.

## COVERED (in scope for APFPV-on-PixelPilot)
| gsmenu category | settings | fork file |
|-----------------|----------|-----------|
| gs apfpv        | ssid, password, channel, bw, reset, status | ApfpvSettings + ApfpvStaLink (nativeStaConnect) |
| air aalink      | channel, MCS_SOURCE, THROUGHPUT_PCT, HIGH_TEMP, SCALE_TX_POWER, THRESH_SHIFT, OSD_SCALE/LEVEL, SHOW_SIGNAL_BARS | ApfpvSettings + AirWebUiClient |
| air camera      | bitrate, codec, size, fps, gopsize, fpv_enable, mirror, flip, contrast, hue, saturation, luminance, exposure, antiflicker, noiselevel, sensor_file, rec_enable/maxusage/split | AirCameraSettings + AirWebUiClient |
| air telemetry   | osd_fps, router, serial, gs_rendering | AirCameraSettings + AirWebUiClient |
| air presets     | apply named preset | AirWebUiClient.applyPreset |
| gs system       | resolution, rec_fps, rec_enabled, gs_rendering, rx_mode | ApfpvSettings |
| gs main         | version, channel, disk, hdmi-out, wfb_nics (status) | GsSettings (read-only on phone) |
| gs wifi         | ip, ssid, hotspot (phone connectivity) | GsSettings (informational on phone) |

## OUT OF SCOPE (WFB-ng-only; existing PixelPilot wfb path handles)
air wfbng (fec_k/fec_n/ldpc/stbc/mcs_index/air_channel/width/power/adaptivelink/mlink),
gs wfbng (bandwidth/gs_channel/txpower/adaptivelink),
air alink (the WFB-ng adaptive-link timing knobs: check_xtx_period_ms,
exp_smoothing_factor*, fallback_ms, hysteresis_percent*, min_between_changes_ms,
power_level_0_to_4, request_keyframe_interval_ms — these are WFB-ng's alink,
NOT APFPV's aalink).

## REAL DRIVER INTEGRATION (not config — the actual data path)
- android/ApfpvStaLink.java         credentialed/stateful JNI surface
- android/jni/apfpv_jni.cpp         JNI <-> native ApfpvStation (libusb fd path)
- android/pixelpilot/ApfpvLinkManager.java  USB perm + connect + state->UI
- native devourer fork              arm->assoc->WPA2(verified)->DHCP->RTP->LQ
- RTP -> 127.0.0.1:5600 -> existing VideoPlayer (decoder/OSD/DVR UNCHANGED)

## Settings application paths
- gs apfpv ssid/pw/channel -> nativeStaConnect (drives the dongle association)
- air * (camera/aalink/telemetry/presets) -> AirWebUiClient HTTP to 192.168.0.1
  (reachable while associated; mirrors gsmenu's SSH `cli -s`/`fw_setenv` writes)
- gs system/main/wifi -> Android-native (player, storage, WifiManager)

## UPDATE 2 — complete coverage (both modes) + seamless switch

Re-audited against PixelPilot's REAL in-code menu (not assumptions). Added what
was actually missing:

WFB-ng (was assumed covered, wasn't): air mcs_index/fec_k/fec_n/air_channel/
width/mlink -> GsMenuWfbng + AirSshClient. These live in the air unit's wfb.yaml
and gsmenu sets them via SSH `wifibroadcast cli -s` — so PixelPilot needs an SSH
path (AirSshClient; needs JSch dep wired — throws until then, not faked).

APFPV: air presets (git-cloned tuning profiles from OpenIPC/fpv-presets) were
stubbed, now real -> AirPresets (lists/applies preset-config.yaml settings via
REST in apfpv mode or SSH cli -s in wfb mode).

SEAMLESS SWITCH (the VRX "set gs system rx_mode"): LinkModeCoordinator mirrors
the VRX sequence exactly — stop active stack, RELOAD the dongle (phone analogue
of rmmod/modprobe = release+reopen the USB fd, re-init driver in target mode),
reconfigure, start. No app restart. Same dongle handed between monitor (wfb) and
station (apfpv) modes, just as the VRX hands the netdev between drivers.
