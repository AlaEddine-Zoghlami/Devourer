package com.openipc.pixelpilot.apfpv;

import java.util.ArrayList;
import java.util.List;

/**
 * AirPresets — the VRX gsmenu "air presets" feature.
 *
 * On the VRX (gsmenu.sh):
 *   - presets live in /etc/presets, git-cloned from OpenIPC/fpv-presets
 *   - "set air presets <name>" reads the preset's settings and applies each
 *     key=value as a cli command ON THE AIR UNIT OVER SSH, then restarts the
 *     relevant service. gsmenu uses SSH for EVERY air-side write — camera
 *     (cli -s .image/.video0 && killall -1 majestic), wfb-ng (cli -s
 *     .wireless/.broadcast && wifibroadcast restart) and aalink (sed
 *     /etc/aalink.conf && kill -SIGHUP aalink). There is NO REST/curl path
 *     anywhere in gsmenu.
 *
 * We mirror that exactly: presets always apply over SSH, routing each setting
 * to the correct air command by its key prefix. APFPV vs WFB-ng no longer
 * changes the TRANSPORT (always SSH) — it only affects which services exist,
 * which is handled naturally by routing per key.
 *
 * Source of truth for the catalog: https://github.com/OpenIPC/fpv-presets
 */
public class AirPresets {

    public static class Preset {
        public String name, author, description, category, sensor, status, tags;
        // flattened key=value settings the preset applies to the air unit
        public final List<String[]> settings = new ArrayList<>(); // [key, value]
    }

    private final List<Preset> catalog = new ArrayList<>();

    public List<Preset> list() { return catalog; }
    public void add(Preset p) { catalog.add(p); }
    public Preset byName(String name) {
        for (Preset p : catalog) if (p.name != null && p.name.equals(name)) return p;
        return null;
    }

    /**
     * Apply a preset to the air unit — ALWAYS over SSH, matching gsmenu.
     * Each setting is routed by key to the right air command:
     *   .image.* / .video0.*  -> cli -s <key> <val> && killall -1 majestic
     *   .wireless.* / .broadcast.* -> cli -s <key> <val> (+ one wfb restart)
     *   aalink keys (UPPER_CASE / "channel") -> sed /etc/aalink.conf + SIGHUP
     * One combined script is run so services restart once, not per setting.
     */
    public void apply(Preset p, boolean apfpvMode, AirSshClient ssh,
                      AirSshClient.Result cb) {
        if (p == null) { if (cb != null) cb.onDone(false, "no such preset"); return; }
        if (ssh == null) { if (cb != null) cb.onDone(false, "no SSH client"); return; }

        List<String> cmds = new ArrayList<>();
        boolean touchedMajestic = false;
        boolean touchedWfb = false;
        boolean touchedAalink = false;

        for (String[] kv : p.settings) {
            String key = kv[0], val = kv[1];
            String k = key.startsWith(".") ? key.substring(1) : key;
            if (k.startsWith("image.") || k.startsWith("video0.")) {
                cmds.add("cli -s ." + k + " " + val);
                touchedMajestic = true;
            } else if (k.startsWith("wireless.") || k.startsWith("broadcast.")) {
                cmds.add("cli -s ." + k + " " + val);
                touchedWfb = true;
            } else {
                // aalink.conf key (e.g. SCALE_TX_POWER, MCS_SOURCE, channel)
                cmds.add("sed -i \"s/^" + key + "=.*/" + key + "=" + val + "/\" /etc/aalink.conf");
                touchedAalink = true;
            }
        }
        // Restart each touched service exactly once, after all writes (gsmenu order).
        if (touchedMajestic) cmds.add("killall -1 majestic");
        if (touchedWfb)      cmds.add(GsMenuWfbng.WFB_RESTART);
        if (touchedAalink)   cmds.add("kill -SIGHUP $(pidof aalink)");

        if (cmds.isEmpty()) { if (cb != null) cb.onDone(true, "empty preset"); return; }
        try {
            ssh.run(cmds.toArray(new String[0]),
                    (ok, out) -> { if (cb != null) cb.onDone(ok, "preset via SSH: " + out); });
        } catch (UnsupportedOperationException e) {
            if (cb != null) cb.onDone(false, "SSH not wired: " + e.getMessage());
        }
    }
}
