package com.openipc.pixelpilot.apfpv;

/**
 * GsMenuWfbng — the WFB-ng air/gs settings the VRX gsmenu exposes that
 * PixelPilot's existing popup menu does NOT cover.
 *
 * Audit of PixelPilot's existing menu vs the full gsmenu wfbng set:
 *   PRESENT in PixelPilot today:
 *     - gs channel        (Channel submenu)
 *     - gs bandwidth      (Bandwidth submenu)
 *     - adaptivelink on   (Adaptive link > Enable)
 *     - gs txpower        (Adaptive link > Power: 1/10/20/30/40)
 *     - fec toggle, ldpc, stbc, FEC thresholds (Adaptive link submenu)
 *     - wfb-ng key import (WFB-NG key submenu)
 *   MISSING (this class fills them):
 *     - air wfbng mcs_index     (.broadcast.mcs_index)
 *     - air wfbng fec_k         (.broadcast.fec_k)
 *     - air wfbng fec_n         (.broadcast.fec_n)
 *     - air wfbng air_channel   (.wireless.channel — set the AIR unit's channel)
 *     - air wfbng width         (.wireless.width)
 *     - air wfbng mlink         (.wireless.mlink)
 *     - air wfbng power         (.wireless.txpower on the AIR side)
 *
 * TRANSPORT: gsmenu sets ALL air-side settings over SSH — in both APFPV and
 *   WFB-ng modes. (In WFB-ng the air is in monitor/broadcast mode and reachable
 *   over the wfb tunnel; in APFPV it's reachable at 192.168.0.1. Neither uses
 *   REST.) The VRX gsmenu sets these via SSH:
 *       wifibroadcast cli -s .broadcast.mcs_index <v>
 *       (then: wifibroadcast stop; stop; sleep 1; start)
 *   PixelPilot therefore needs an SSH path to the air unit (over the wfb tunnel
 *   / mavlink link) to apply these. That SSH client is NOT yet in PixelPilot —
 *   flagged below. APFPV mode does NOT have this problem (air unit IS on
 *   192.168.0.1 over the AP, so REST works).
 */
public class GsMenuWfbng {

    // ---- AIR-side WFB-ng knobs (wfb.yaml via SSH cli -s) -------------------
    public int     mcsIndex   = 1;        // .broadcast.mcs_index
    public int     fecK       = 8;        // .broadcast.fec_k
    public int     fecN       = 12;       // .broadcast.fec_n
    public int     airChannel = 161;      // .wireless.channel (AIR unit channel)
    public int     width      = 20;       // .wireless.width  (20|40)
    public boolean mlink      = false;    // .wireless.mlink
    public int     airTxPower = 20;       // .wireless.txpower (air side)

    // ---- command templates (exactly what gsmenu runs over SSH) -------------
    // These are applied by an SSH client (see AirSshClient, to be wired). After
    // any change WFB-ng must be restarted on the air unit:
    //   wifibroadcast stop; wifibroadcast stop; sleep 1; wifibroadcast start
    public static String cliSet(String yamlKey, String value) {
        return "wifibroadcast cli -s ." + yamlKey + " " + value;
    }
    public static final String WFB_RESTART =
        "(wifibroadcast stop; wifibroadcast stop; sleep 1; wifibroadcast start) >/dev/null 2>&1 &";

    public String[] applyMcs()        { return new String[]{ cliSet("broadcast.mcs_index", String.valueOf(mcsIndex)), WFB_RESTART }; }
    public String[] applyFecK()       { return new String[]{ cliSet("broadcast.fec_k", String.valueOf(fecK)), WFB_RESTART }; }
    public String[] applyFecN()       { return new String[]{ cliSet("broadcast.fec_n", String.valueOf(fecN)), WFB_RESTART }; }
    public String[] applyAirChannel() { return new String[]{ cliSet("wireless.channel", String.valueOf(airChannel)), WFB_RESTART }; }
    public String[] applyWidth()      { return new String[]{ cliSet("wireless.width", String.valueOf(width)), WFB_RESTART }; }
    public String[] applyMlink()      { return new String[]{ cliSet("wireless.mlink", mlink ? "1" : "0"), WFB_RESTART }; }
    public String[] applyAirTxPower() { return new String[]{ cliSet("wireless.txpower", String.valueOf(airTxPower)), WFB_RESTART }; }

    // value ranges from gsmenu "values air wfbng ..."
    public static final int[] MCS    = {0,1,2,3,4,5,6,7};
    public static final int[] FEC_K  = {1,2,3,4,5,6,7,8,9,10,11,12};
    public static final int[] FEC_N  = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    public static final int[] WIDTHS = {20,40};
}
