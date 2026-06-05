package com.openipc.pixelpilot.apfpv;

import com.jcraft.jsch.ChannelExec;
import com.jcraft.jsch.JSch;
import com.jcraft.jsch.Session;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.Properties;

/**
 * AirSshClient — config transport to the AIR unit over SSH.
 *
 * This mirrors EXACTLY what the real OpenIPC ground station (sbc-groundstations
 * gsmenu.sh) does: it is overwhelmingly SSH-based (80+ ops over `ssh
 * root@<air>`), with the air unit's knobs set via `wifibroadcast cli` (WFB-ng),
 * `cli -s .video0/.image && killall -1 majestic` (camera), and
 * `sed -i /etc/aalink.conf && kill -SIGHUP $(pidof aalink)` (aalink).
 *
 * For greg10.2 APFPV the air unit is reachable at 192.168.0.1 over the AP, so
 * SSH works directly. For WFB-ng mode the air is reached over the wfb tunnel.
 *
 * Transport: JSch (com.github.mwiede:jsch). Each call opens a session, execs
 * the command(s), collects stdout, and returns. Runs OFF the main thread.
 */
public class AirSshClient {

    public String host = "192.168.0.1";   // greg10.2 air over the AP (APFPV)
    public String user = "root";
    public String pass = "12345";          // OpenIPC default (SSH_PASS in gsmenu)
    public int    port = 22;
    public int    timeoutMs = 11000;       // gsmenu uses timeout -k 1 11

    public interface Result { void onDone(boolean ok, String output); }

    public void useApfpvHost() { this.host = "192.168.0.1"; }
    public void useWfbTunnelHost(String h) { this.host = h; }

    /** Run commands sequentially over one SSH session; callback on a worker thread. */
    public void run(String[] commands, Result cb) {
        new Thread(() -> {
            Session session = null;
            StringBuilder out = new StringBuilder();
            boolean ok = true;
            try {
                JSch jsch = new JSch();
                session = jsch.getSession(user, host, port);
                session.setPassword(pass);
                Properties cfg = new Properties();
                cfg.put("StrictHostKeyChecking", "no");   // gsmenu: -o StrictHostKeyChecking=no
                cfg.put("PreferredAuthentications", "password");
                session.setConfig(cfg);
                session.setTimeout(timeoutMs);
                session.connect(timeoutMs);

                for (String cmd : commands) {
                    ChannelExec ch = (ChannelExec) session.openChannel("exec");
                    ch.setCommand(cmd);
                    ByteArrayOutputStream bos = new ByteArrayOutputStream();
                    ch.setOutputStream(bos);
                    ch.setErrStream(bos);
                    InputStream in = ch.getInputStream();
                    ch.connect();
                    byte[] buf = new byte[1024];
                    while (true) {
                        while (in.available() > 0) { int n = in.read(buf); if (n < 0) break; bos.write(buf, 0, n); }
                        if (ch.isClosed()) { if (in.available() > 0) continue; break; }
                        try { Thread.sleep(50); } catch (InterruptedException ignored) {}
                    }
                    out.append(bos.toString());
                    if (ch.getExitStatus() != 0 && ch.getExitStatus() != -1) ok = false;
                    ch.disconnect();
                }
            } catch (Exception e) {
                ok = false;
                out.append("SSH error: ").append(e.getMessage());
            } finally {
                if (session != null) session.disconnect();
            }
            final boolean fok = ok; final String fout = out.toString();
            if (cb != null) cb.onDone(fok, fout);
        }, "AirSsh").start();
    }

    // ---- exact command builders matching gsmenu.sh -------------------------

    private static String wfbRestart() {
        return "(wifibroadcast stop ; wifibroadcast stop; sleep 1; wifibroadcast start) >/dev/null 2>&1 &";
    }

    /** WFB-ng knob: wifibroadcast cli -s <path> <val> + restart (gsmenu pattern). */
    public void setWfbng(String yamlPath, String value, Result cb) {
        run(new String[]{ "wifibroadcast cli -s " + yamlPath + " " + value, wfbRestart() }, cb);
    }
    public void setMcsIndex(int v, Result cb)  { setWfbng(".broadcast.mcs_index", String.valueOf(v), cb); }
    public void setFecK(int v, Result cb)      { setWfbng(".broadcast.fec_k", String.valueOf(v), cb); }
    public void setFecN(int v, Result cb)      { setWfbng(".broadcast.fec_n", String.valueOf(v), cb); }
    public void setWidth(int v, Result cb)     { setWfbng(".wireless.width", String.valueOf(v), cb); }
    public void setAirChannel(int v, Result cb){ setWfbng(".wireless.channel", String.valueOf(v), cb); }
    public void setTxPower(int v, Result cb)   { setWfbng(".wireless.txpower", String.valueOf(v), cb); }
    public void setMlink(int v, Result cb)     { setWfbng(".wireless.mlink", String.valueOf(v), cb); }
    public void setStbc(boolean on, Result cb) { setWfbng(".broadcast.stbc", on ? "1" : "0", cb); }
    public void setLdpc(boolean on, Result cb) { setWfbng(".broadcast.ldpc", on ? "1" : "0", cb); }

    /** Camera (majestic): cli -s <path> <val> && killall -1 majestic (gsmenu pattern). */
    public void setCamera(String yamlPath, String value, Result cb) {
        run(new String[]{ "cli -s " + yamlPath + " " + value + " && killall -1 majestic" }, cb);
    }
    public void setBitrate(int kbps, Result cb)   { setCamera(".video0.bitrate", String.valueOf(kbps), cb); }
    public void setCodec(String c, Result cb)     { setCamera(".video0.codec", c, cb); }
    public void setSize(String s, Result cb)      { setCamera(".video0.size", s, cb); }
    public void setFps(int f, Result cb)          { setCamera(".video0.fps", String.valueOf(f), cb); }
    public void setGop(double g, Result cb)       { setCamera(".video0.gopSize", String.valueOf(g), cb); }
    public void setContrast(int v, Result cb)     { setCamera(".image.contrast", String.valueOf(v), cb); }
    public void setSaturation(int v, Result cb)   { setCamera(".image.saturation", String.valueOf(v), cb); }
    public void setLuminance(int v, Result cb)    { setCamera(".image.luminance", String.valueOf(v), cb); }
    public void setHue(int v, Result cb)          { setCamera(".image.hue", String.valueOf(v), cb); }
    public void setMirror(boolean on, Result cb)  { setCamera(".image.mirror", on ? "true" : "false", cb); }
    public void setFlip(boolean on, Result cb)    { setCamera(".image.flip", on ? "true" : "false", cb); }

    /** aalink: sed -i /etc/aalink.conf + kill -SIGHUP $(pidof aalink) (gsmenu pattern). */
    public void setAalink(String key, String value, Result cb) {
        String cmd = "sed -i \"s/^" + key + "=.*/" + key + "=" + value + "/\" /etc/aalink.conf"
                   + "; kill -SIGHUP $(pidof aalink)";
        run(new String[]{ cmd }, cb);
    }

    /** Legacy entry point retained for callers. */
    public void applyWfbng(String[] cliAndRestart, Result cb) { run(cliAndRestart, cb); }
}
