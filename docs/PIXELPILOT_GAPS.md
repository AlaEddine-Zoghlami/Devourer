# What PixelPilot misses to use station-mode devourer

PixelPilot drives devourer through the WfbNgLink JNI surface, which is entirely
WFB-ng-shaped (nativeRun(channel,bw,fd), nativeRefreshKey, nativeSetUseFec,
nativeStartAdaptivelink, stats). It models a STATELESS BROADCAST receiver.
Station mode is a CREDENTIALED, STATEFUL CONNECTION — a model PixelPilot has
never had. Gaps:

1. ApfpvStaLink (new JNI class, android/ApfpvStaLink.java)
   - nativeStaConnect(fd,ch,bw, SSID, PASSPHRASE)  <- credentials (new concept)
   - nativeStaGetState() -> lifecycle enum that can FAIL (new concept)
   - nativeStaSetLqFeedback() -> dongle-RSSI -> VTX:12345 (new; better than stock)
   - WfbNgLink's gs.key/FEC/alink methods are MEANINGLESS in APFPV (absent)

2. Credential UI — SSID + WPA2 passphrase fields. PixelPilot only has a channel
   picker today (WFB-ng needs no SSID/password). New settings surface.

3. Connection-state UI + failure messages — association can fail (wrong PSK,
   AP absent, deauth, no-ACK). Must surface SCANNING/ASSOCIATING/HANDSHAKING/
   STREAMING and FAIL_NO_AP/FAIL_TX/FAIL_NO_ACK/FAIL_AUTH/FAIL_DHCP/LINK_LOST.
   The always-on WFB-ng model never needed this.

4. Mode toggle WFB-ng <-> APFPV-station (which link class to instantiate).

5. OSD: show dongle RSSI (from radiotap) as the link metric.

NOT NEEDED (reused untouched):
   - Video decode path: station devourer de-encaps 802.11->UDP->RTP into the
     SAME port-5600 / onNewRTPData pipeline. Decoder, OSD render, DVR unchanged.

Summary: PixelPilot needs a CREDENTIALED STATEFUL CONNECTION MANAGER + its UI;
the video half is reused as-is.
