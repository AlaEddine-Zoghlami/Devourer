package com.openipc.pixelpilot.apfpv;

import java.util.Arrays;
import java.util.List;

/**
 * AirCameraSettings — the AIR camera/telemetry/preset controls the VRX gsmenu
 * exposes ("set air camera ...", "set air telemetry ...", "set air presets ...").
 * These are majestic settings on the VTX, applied the way gsmenu does:
 *     cli -s .video0.<key> <val> && killall -1 majestic     (over the VTX)
 * On the phone we drive the same over SSH (AirSshClient), exactly as gsmenu
 * does: cli -s .image/.video0 && killall -1 majestic. The VTX is reachable at
 * 192.168.0.1 while associated in APFPV mode. (The real GS uses SSH for every
 * air-side write; there is no REST/curl path in gsmenu.)
 *
 * This is the largest GS-menu category and was the main gap in the prior
 * integration pass (which had aalink + apfpv + gs system, but not camera).
 * Value ranges below are copied verbatim from gsmenu.sh "values air camera ...".
 */
public class AirCameraSettings {

    // ---- video0.* (majestic) -----------------------------------------------
    public int     bitrateKbps = 8192;     // 1024..30720 step 1024
    public String  codec       = "h265";   // h264 | h265
    public String  size        = "1280x720";
    public int     fps         = 90;        // 60 | 90 | 120
    public int     gopSize     = 1;         // 0..10  (low GOP = fast recovery)
    public boolean fpvEnable   = true;      // video0.fpv mode

    // ---- image (isp) --------------------------------------------------------
    public boolean mirror      = false;
    public boolean flip        = false;
    public int     contrast    = 50;        // 0..100
    public int     hue         = 50;
    public int     saturation  = 50;
    public int     luminance   = 50;
    public String  exposure    = "auto";
    public String  antiflicker = "disabled";// disabled|50|60
    public int     noiseLevel  = 0;
    public String  sensorFile  = "";        // sensor tuning .bin

    // ---- recording (on the AIR unit) ---------------------------------------
    public boolean recEnable   = false;
    public int     recMaxUsage = 90;
    public int     recSplit    = 60;

    // ---- telemetry / OSD router --------------------------------------------
    public int     osdFps      = 20;
    public String  router      = "msposd";  // values air telemetry router
    public String  serial      = "/dev/ttyS2";
    public String  rcMode      = "";        // values air camera rc_mode

    // ---- value ranges (verbatim from gsmenu.sh) ----------------------------
    public static final List<String> SIZES = Arrays.asList(
        "1280x720","1456x816","1920x1080","1440x1080","1920x1440","2104x1184",
        "2208x1248","2240x1264","2312x1304","2436x1828","2512x1416","2560x1440",
        "2560x1920","2720x1528","2944x1656","3200x1800","3840x2160");
    public static final List<String> FPS    = Arrays.asList("60","90","120");
    public static final List<String> CODECS = Arrays.asList("h264","h265");
    public static final int BITRATE_MIN = 1024, BITRATE_MAX = 30720, BITRATE_STEP = 1024;
    public static final int GOP_MIN = 0, GOP_MAX = 10;
}
