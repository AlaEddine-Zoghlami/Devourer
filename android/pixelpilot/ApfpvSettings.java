package com.openipc.pixelpilot.apfpv;

/**
 * ApfpvSettings — mirrors the feature set the real VRX exposes via gsmenu.sh,
 * adapted for PixelPilot (phone GS). Sourced from OpenIPC/sbc-groundstations
 * package/pixelpilot/files/gsmenu.sh.
 *
 * The GS menu splits settings across scopes. For APFPV-on-PixelPilot the
 * relevant ones are:
 *   - GS APFPV connection : ssid, password, per-adapter enable (wlx), status
 *   - AIR aalink (adaptive): the VTX-side adaptation knobs, set over the VTX
 *                            WebUI at http://192.168.0.1 (root/12345)
 *   - GS system           : resolution, rec_fps, rec_enabled, rendering, rx_mode
 *
 * WFB-ng-only knobs (fec_k/fec_n/ldpc/stbc/mcs/air_channel/width/txpower/
 * adaptivelink/mlink) are NOT part of APFPV mode and are handled by the
 * existing WFB-ng path, not here.
 */
public class ApfpvSettings {

    // ---- GS APFPV connection (set gs apfpv ssid/password; multi-card wlx) ---
    public String  ssid       = "OpenIPC";
    public String  password   = "12345678";
    public int     channel    = 40;       // legal 5.2 GHz UNII-1 default (DE)
    public int     bandwidth  = 20;       // 20 MHz = full 200 mW under PSD cap

    // Multi-adapter: the VRX picks the best wlx card via ip-route; on the phone
    // we may have 1 (the AU). Kept for parity/future dual-dongle diversity.
    public boolean lqFeedbackEnabled = true;   // dongle RSSI -> VTX:12345
    public boolean staticIp          = false;  // else minimal DHCP (.10)

    // ---- AIR aalink knobs (set air aalink ...; via VTX WebUI HTTP) ----------
    // Exactly the keys gsmenu exposes under "values air aalink ...".
    public int     aalinkChannel     = 40;
    public String  mcsSource         = "lowest";   // lowest|highest|both|uplink|downlink
    public int     throughputPct     = 39;
    public int     highTemp          = 95;
    // Default 0.74: verified from the aalink binary, SCALE_TX_POWER multiplies the
    // per-MCS PWR table only when < 1.0 (>=1.0 is a no-op). 0.74 scales the peak
    // EU entry 2800 -> ~2072 index. NOTE: because this is < 1.0 it is ACTIVE, so the
    // app reduces commanded TX power from stock on first apply. Range enforced [0,1]
    // in the menu. (Index value, not certified mW — see project notes.)
    public double  scaleTxPower      = 0.74;
    public int     threshShift       = 0;
    public double  osdScale          = 0.8;
    public int     osdLevel          = 3;          // 0..3 (air burns OSD into stream)
    public boolean showSignalBars    = true;

    // ---- GS system (set gs system ...) -------------------------------------
    public String  resolution        = "1280x720@90";
    public int     recFps            = 60;
    public boolean recEnabled        = false;
    public String  gsRendering       = "hardware";
    public String  rxMode            = "apfpv";    // "wfb" | "apfpv" — the toggle

    public boolean isApfpv() { return "apfpv".equals(rxMode); }
}
