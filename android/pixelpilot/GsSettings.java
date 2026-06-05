package com.openipc.pixelpilot.apfpv;

/**
 * GsSettings — the GROUND-STATION-side settings the VRX gsmenu exposes that
 * aren't about the APFPV link itself:
 *   "gs wifi ..."  — the GS's OWN wifi (hotspot for config / home-network join),
 *                    distinct from the APFPV video link (which is the dongle).
 *   "gs main ..."  — status/diagnostics (channel, disk, version, HDMI-out, NICs).
 *   "gs system ..."— resolution/rec/rendering/rx_mode (already in ApfpvSettings).
 *
 * On a Radxa VRX these manage the board's onboard wifi + the player. On the
 * PHONE, most of "gs wifi" maps to Android's own connectivity (the phone is the
 * GS), so these are mostly READ-ONLY status the app surfaces, plus the few that
 * make sense (the phone doesn't run a config hotspot — it IS the device).
 *
 * Kept for full gsmenu parity and to drive a Status screen.
 */
public class GsSettings {

    // ---- gs main (status / diagnostics) — READ-ONLY surface ----------------
    public String  version       = "";      // get gs main Version  (app/driver ver)
    public int     channel       = 40;       // get gs main Channel  (current link ch)
    public String  disk          = "";      // get gs main Disk     (DVR storage free)
    public boolean hdmiOut       = false;   // get gs main HDMI-OUT (N/A on phone)
    public String  wfbNics       = "";      // get gs main WFB_NICS (the AU adapter id)

    // ---- gs wifi (the GS's own connectivity) -------------------------------
    // On the phone, "the GS wifi" is the phone's network stack. The APFPV link
    // is the USB dongle (separate). These are mostly informational on phone:
    public String  gsWifiIp      = "";      // get gs wifi IP     (phone IP on home net)
    public boolean hotspot       = false;   // get gs wifi hotspot (N/A on phone)
    public String  gsWifiSsid    = "";      // get gs wifi ssid   (phone's joined net)

    // ---- recording (GS-side DVR) maps to existing PixelPilot DVR -----------
    public boolean dvrEnabled    = false;
    public int     dvrFps        = 60;
    public String  dvrResolution = "1280x720@90";

    /**
     * On the phone these are read from Android / the driver rather than set:
     *  - version  : BuildConfig + native driver version string
     *  - channel  : current ApfpvStation channel
     *  - disk     : Environment.getExternalStorageDirectory().getFreeSpace()
     *  - wfbNics  : the bound UsbDevice product string
     *  - gsWifiIp : phone's WifiManager connection info
     * The Status screen calls these getters; nothing is pushed to a board.
     */
    public boolean isPhoneGs() { return true; }
}
