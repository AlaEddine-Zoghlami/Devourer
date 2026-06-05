package com.openipc.pixelpilot.apfpv;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;

/**
 * ApfpvWifiManager — the THIRD link mode: APFPV over the PHONE'S OWN Wi-Fi chip,
 * with no RTL8812AU dongle and no devourer driver.
 *
 * WHY THIS IS A SEPARATE MODE (vs the dongle APFPV path in ApfpvLinkManager):
 *   - There is no USB device, no monitor mode, no native 802.11 de-encapsulation.
 *   - The phone associates to greg's AP ("OpenIPC") as a normal managed station
 *     and gets a DHCP lease on 192.168.0.0/24 (phone ~= 192.168.0.10, VTX .0.1).
 *   - greg's majestic streams plain UDP/RTP to 192.168.0.10:5600, which the OS
 *     hands to the EXISTING VideoPlayer UDP socket (INADDR_ANY:5600) — so the
 *     video path needs no new native code at all.
 *   - Air config is the SAME as dongle APFPV: SSH to 192.168.0.1 (AirSshClient).
 *   - RSSI for OSD comes from WifiManager.getConnectionInfo().getRssi(), not
 *     dongle radiotap.
 *   - The LQ feedback that greg's aalink expects (it PINGs 192.168.0.10 and
 *     listens on UDP 12345 for an RSSI byte) is sent from here using the
 *     WifiManager RSSI.
 *
 * THE CRITICAL ANDROID PROBLEM THIS SOLVES:
 *   greg's AP has NO internet uplink. By default Android marks such a Wi-Fi
 *   network "no internet" and routes app sockets (the SSH/TCP connection AND
 *   the video UDP socket) to cellular/another network instead — so without
 *   binding, video never arrives and SSH connects to the wrong place (or fails).
 *   We request the specific Wi-Fi network and bindProcessToNetwork() so ALL of
 *   this process's sockets traverse the Wi-Fi link to the VTX.
 *
 * NOTE: This is new ground — upstream PixelPilot is dongle-only and has no
 * network-binding code to mirror. Untested on hardware in this form.
 */
public class ApfpvWifiManager {

    public interface Listener {
        void onState(String state);
        void onRssi(int dbm);
        void onError(String detail);
    }

    private final Context ctx;
    private final ConnectivityManager cm;
    private final WifiManager wifi;
    private Listener listener;

    private String ssid = "OpenIPC";
    private String pass = "12345678";

    // VTX expects LQ feedback here (greg10.2 aalink.conf: PING_DEST=192.168.0.10,
    // UDP_PORT=12345). We are 192.168.0.10; the VTX (192.168.0.1) reads our RSSI.
    private static final String VTX_IP   = "192.168.0.1";
    private static final int    LQ_PORT  = 12345;
    private static final int    LQ_PERIOD_MS = 100;

    private ConnectivityManager.NetworkCallback netCb;
    private volatile Network boundNetwork;
    private volatile boolean running = false;

    private Thread rssiThread, lqThread;
    private final Handler ui = new Handler(Looper.getMainLooper());

    public ApfpvWifiManager(Context ctx, Listener l) {
        this.ctx = ctx.getApplicationContext();
        this.listener = l;
        this.cm = (ConnectivityManager) this.ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        this.wifi = (WifiManager) this.ctx.getSystemService(Context.WIFI_SERVICE);
    }

    public void setCredentials(String s, String p) {
        if (s != null) this.ssid = s;
        if (p != null) this.pass = p;
    }

    /**
     * Associate to the AP and bind this process to it. API 29+ uses
     * WifiNetworkSpecifier (the modern, permissionless local-network path).
     * On success, bindProcessToNetwork pins sockets so video + SSH go over Wi-Fi.
     */
    public synchronized void start() {
        if (running) return;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            emitError("phone-Wi-Fi APFPV needs Android 10+ (WifiNetworkSpecifier)");
            return;
        }
        running = true;
        emitState("requesting Wi-Fi association to " + ssid);

        WifiNetworkSpecifier spec = new WifiNetworkSpecifier.Builder()
                .setSsid(ssid)
                .setWpa2Passphrase(pass)
                .build();

        NetworkRequest req = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                // do NOT require INTERNET — greg's AP has no uplink
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .setNetworkSpecifier(spec)
                .build();

        netCb = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) {
                boundNetwork = network;
                // Pin ALL sockets in this process (video UDP + SSH TCP) to Wi-Fi.
                boolean ok = cm.bindProcessToNetwork(network);
                emitState(ok ? "associated + bound to " + ssid
                             : "associated but bind FAILED (sockets may use cellular)");
                startRssiLoop();
                startLqLoop();
            }
            @Override public void onUnavailable() {
                emitError("could not join " + ssid + " (user denied or AP not found)");
            }
            @Override public void onLost(Network network) {
                emitError("lost association to " + ssid);
            }
        };
        try {
            cm.requestNetwork(req, netCb);
        } catch (Exception e) {
            running = false;
            emitError("requestNetwork failed: " + e.getMessage());
        }
    }

    public synchronized void stop() {
        running = false;
        if (rssiThread != null) { rssiThread.interrupt(); rssiThread = null; }
        if (lqThread != null)   { lqThread.interrupt();   lqThread = null; }
        try { cm.bindProcessToNetwork(null); } catch (Exception ignored) {}
        if (netCb != null) {
            try { cm.unregisterNetworkCallback(netCb); } catch (Exception ignored) {}
            netCb = null;
        }
        boundNetwork = null;
        emitState("stopped");
    }

    /** Current Wi-Fi RSSI in dBm, or -127 if unavailable. */
    public int currentRssi() {
        try {
            if (wifi != null && wifi.getConnectionInfo() != null)
                return wifi.getConnectionInfo().getRssi();
        } catch (Exception ignored) {}
        return -127;
    }

    // Poll WifiManager RSSI for the OSD/link metric.
    private void startRssiLoop() {
        rssiThread = new Thread(() -> {
            while (running && !Thread.currentThread().isInterrupted()) {
                int dbm = currentRssi();
                emitRssi(dbm);
                try { Thread.sleep(250); } catch (InterruptedException e) { break; }
            }
        }, "apfpv-wifi-rssi");
        rssiThread.start();
    }

    // Send LQ feedback to the VTX exactly like the dongle LqFeedback path, but
    // sourced from WifiManager RSSI. greg's aalink listens on 192.168.0.1:12345?
    // No — aalink PINGs 192.168.0.10 (us) and reads RSSI sent back to it; the GS
    // sends a small datagram with the signal percentage. We mirror that: convert
    // dBm -> percent with the SAME formula the OpenIPC GS uses (2*(dBm+100),
    // clamped 0..100) and send it to the VTX's LQ port.
    private void startLqLoop() {
        lqThread = new Thread(() -> {
            DatagramSocket sock = null;
            try {
                sock = new DatagramSocket();
                if (boundNetwork != null) {
                    try { boundNetwork.bindSocket(sock); } catch (Exception ignored) {}
                }
                InetAddress dst = InetAddress.getByName(VTX_IP);
                while (running && !Thread.currentThread().isInterrupted()) {
                    int pct = rssiToPct(currentRssi());
                    byte[] payload = Integer.toString(pct).getBytes();
                    DatagramPacket pkt = new DatagramPacket(payload, payload.length, dst, LQ_PORT);
                    try { sock.send(pkt); } catch (Exception ignored) {}
                    try { Thread.sleep(LQ_PERIOD_MS); } catch (InterruptedException e) { break; }
                }
            } catch (Exception e) {
                emitError("LQ sender: " + e.getMessage());
            } finally {
                if (sock != null) sock.close();
            }
        }, "apfpv-wifi-lq");
        lqThread.start();
    }

    /** OpenIPC GS calibration: sig_pct = 2*(dBm+100), clamped [0,100]. */
    static int rssiToPct(int dbm) {
        int pct = 2 * (dbm + 100);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        return pct;
    }

    private void emitState(String s) { if (listener != null) ui.post(() -> listener.onState(s)); }
    private void emitRssi(int d)     { if (listener != null) ui.post(() -> listener.onRssi(d)); }
    private void emitError(String s) { if (listener != null) ui.post(() -> listener.onError(s)); }
}
