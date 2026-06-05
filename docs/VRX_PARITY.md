# Fork vs greg10.2 VRX — parity scorecard

KEY FACT: aalink (the adaptation engine) runs on the AIR UNIT, not the VRX.
The VRX's ONLY contribution to APFPV adaptation is sending downlink LQ to
UDP 192.168.0.1:12345. Everything else in aalink.conf (MCS tables, power
tables PWR_*, bitrate ladder PR, OSD, channel-width re-probe, ROI QP) executes
on the VTX regardless of ground station.

## What a VRX contributes vs our fork
| VRX role | greg10.2 VRX | our fork | status |
|----------|--------------|----------|--------|
| Associate (WPA2) | wpa_supplicant | StationMode + Wpa2Supplicant | scaffold; finish HS |
| Receive RTP | kernel + udp | de-encap -> port 5600 | de-encap TODO |
| Get IP | OS DHCP | (DHCP client) | TODO |
| Downlink LQ -> 12345 | yes | LqFeedback.cpp | DONE (format+33ms) |
| Per-antenna RSSI (MCS_SOURCE=lowest) | 2-ant | LqFeedback sends rssi_a/rssi_b | DONE |

## Air-side (NOT our gap — runs on the VTX no matter what)
MCS selection, PWR_* power tables, bitrate ladder, THRESH_* tables, OSD overlay,
channel-width re-probe (REFRESH_CNT), ROI QP, thermal/REDUCED_POWER_MODE,
USE_CHEAP_PROC EU-chip hooks. None of this belongs to the ground station.

## LQ format (reverse-engineered from aalink binary sscanf strings)
Primary:        "rssi : %d\n"                      (RSSI as 0..100 %)
Per-antenna:    "rssi_a = %d(%), rssi_b = %d(%)\n"  (2x2 -> aalink takes lowest)
SNR (optional): "SNR_A=%d SNR_B=%d"                 (only if air USE_SNR>0)
Cadence:        every 33 ms (RSSI_SAMPLE_INTERVAL_MS). Send RAW %, air scales.

## Bottom line
Remaining deltas vs a working VRX are 3: finish WPA2 handshake bodies, DHCP
client, RTP de-encap. LQ feedback (the VRX-defining contribution) is now at
parity — and arguably better, since we report the diversity (2x2) RSSI of the
receiver that actually decodes.

## UPDATE — verified from sbc-groundstations buildroot-snapshot source

The VRX image supports BOTH transports and switches at runtime:
  configs/runcam_wifilink_defconfig: WIFIBROADCAST_NG=y + ADAPTIVE_LINK=y + PIXELPILOT=y
  gsmenu.sh: AIR_FIRMWARE_TYPE = wfb | apfpv ; separate alink.conf (WFB-ng) and
  aalink.conf (APFPV). => Our fork mirrors this with WfbLinkManager + ApfpvLinkManager
  behind a mode toggle. Correct architecture confirmed.

APFPV-mode GS bringup (source-confirmed):
  - associate: wpa_supplicant (etc/network/if-up.d/wpasupplicant)  [WPA2]
  - IP: udhcpc with udhcpc.apfpv.script — "adapted to NOT add routes and dns
        for apfpv interfaces". Our ApfpvDhcp.cpp matches this: take IP, skip
        route+DNS. (On Android/userspace there's no netstack to pollute anyway,
        but we mirror the intent and the .10 lease.)
  - LQ feedback: OPTIONAL. The buildroot snapshot does NOT contain a finished
    APFPV-mode 12345 sender; aalink accepts it but the GS sender is in-flux.
    => Our LqFeedback is HONESTLY a value-add modeled on aalink's documented
       input ("rssi : %d", RSSI_SAMPLE_INTERVAL_MS=33), NOT a byte-match to a
       published VRX sender (none is fully present). Parameterized (FixedTimer
       33ms | FrameDriven) so it can be tuned to whatever a real sender does
       once one is observed on the wire.
