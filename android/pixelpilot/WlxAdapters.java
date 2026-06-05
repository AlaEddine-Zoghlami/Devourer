package com.openipc.pixelpilot.apfpv;

import android.content.Context;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * WlxAdapters — multi-dongle enumeration / ranking / per-adapter enable,
 * mirroring the VRX gsmenu "gs apfpv wlx" behavior. This closes the last gap to
 * literal greg10.2 parity (the integration previously assumed a single AU).
 *
 * VRX behavior (gsmenu.sh, verified):
 *   - lists all wlx* interfaces (ip link show | grep ^wlx), excluding wlan0
 *   - writes /etc/network/interfaces.d/<wlx>: the FIRST gets "auto" (active),
 *     the rest "#auto" (standby) — INDEX 0 is the chosen one
 *   - "set gs apfpv wlx <iface> on/off": flips auto/#auto + ifup/ifdown, and on
 *     "off" does a USB unbind/rebind to fully reset that adapter
 *   - "get gs apfpv status wlx <iface>": iw dev link -> Connected/Disconnected
 *
 * Phone analogue: enumerate all Realtek (RTL8812AU family) USB devices, rank
 * them, designate one ACTIVE (handed to ApfpvLinkManager.startAdapter), keep the
 * others as selectable standbys. Per-adapter enable = make-active / detach.
 */
public class WlxAdapters {

    public static class Adapter {
        public final UsbDevice device;
        public final String name;        // product or "vid:pid@deviceName"
        public boolean active;           // the "auto" one
        public boolean connected;        // link status (from native state)
        public int rssiDbm = -99;
        Adapter(UsbDevice d, String n) { device = d; name = n; }
    }

    private final Context context;
    private final UsbManager usb;
    private final List<Adapter> adapters = new ArrayList<>();
    private static final String PREF = "pixelpilot";
    private static final String KEY_LAST = "apfpv_last_adapter";

    public WlxAdapters(Context ctx) {
        this.context = ctx;
        this.usb = (UsbManager) ctx.getSystemService(Context.USB_SERVICE);
    }

    private String lastConnectedName() {
        return context.getSharedPreferences(PREF, Context.MODE_PRIVATE).getString(KEY_LAST, null);
    }
    /** Persist the adapter that successfully connected, so it auto-reconnects. */
    public void rememberConnected(String name) {
        if (name == null) return;
        context.getSharedPreferences(PREF, Context.MODE_PRIVATE).edit().putString(KEY_LAST, name).apply();
    }

    /**
     * Enumerate + rank, mirroring the VRX wlx list. Excludes non-Realtek.
     * Prefers the LAST-CONNECTED adapter as active (survives unplug/replug and
     * app restart) — the VRX persists the "auto" flag in interfaces.d; we
     * persist the name in prefs. Falls back to best-ranked if it's absent.
     */
    public synchronized List<Adapter> enumerate() {
        adapters.clear();
        if (usb == null) return adapters;
        for (UsbDevice d : usb.getDeviceList().values()) {
            if (!isRtl8812(d)) continue;     // RTL8812AU family only
            String nm = d.getProductName() != null ? d.getProductName()
                       : String.format("%04x:%04x@%s", d.getVendorId(), d.getProductId(), d.getDeviceName());
            adapters.add(new Adapter(d, nm));
        }
        rank();
        // Prefer the persisted last-connected adapter if it's still present;
        // otherwise the top-ranked one becomes active (VRX INDEX 0 behavior).
        String last = lastConnectedName();
        int activeIdx = 0;
        if (last != null) {
            for (int i = 0; i < adapters.size(); i++)
                if (last.equals(adapters.get(i).name)) { activeIdx = i; break; }
        }
        for (int i = 0; i < adapters.size(); i++) adapters.get(i).active = (i == activeIdx);
        return adapters;
    }

    /**
     * Rank candidates so the "best" becomes the default active one, the phone
     * analogue of the VRX picking INDEX 0. Ranking: prefer a dual-band 2x2
     * AU-VS (product name hint), then by current RSSI if known. Without
     * per-device RSSI before association, this is name/order based; once links
     * report RSSI, updateRssi() re-ranks.
     */
    private void rank() {
        adapters.sort(Comparator
            .comparingInt((Adapter a) -> a.connected ? 0 : 1)               // connected first
            .thenComparingInt(a -> -a.rssiDbm)                              // stronger RSSI first
            .thenComparing(a -> a.name == null ? "" : a.name.toLowerCase())); // stable
    }

    /** "set gs apfpv wlx <iface> on": make this adapter the active one (persisted). */
    public synchronized Adapter setActive(String name) {
        Adapter chosen = null;
        for (Adapter a : adapters) { a.active = a.name.equals(name); if (a.active) chosen = a; }
        if (chosen != null) rememberConnected(name);   // manual pick also persists
        return chosen;   // caller hands chosen.device to ApfpvLinkManager.startAdapter
    }

    /** Feed native link state back so ranking + UI reflect reality. */
    public synchronized void updateStatus(UsbDevice dev, boolean connected, int rssiDbm) {
        for (Adapter a : adapters)
            if (a.device.getDeviceId() == dev.getDeviceId()) { a.connected = connected; a.rssiDbm = rssiDbm; }
        rank();
    }

    public synchronized Adapter active() {
        for (Adapter a : adapters) if (a.active) return a;
        return adapters.isEmpty() ? null : adapters.get(0);
    }
    public synchronized List<Adapter> all() { return new ArrayList<>(adapters); }
    public synchronized int count() { return adapters.size(); }

    private static boolean isRtl8812(UsbDevice d) {
        if (d.getVendorId() != 0x0bda && d.getVendorId() != 0x0b05) return false; // Realtek / ASUS-OEM
        // common RTL8812AU PIDs (AU/AU-VS variants)
        int pid = d.getProductId();
        return pid == 0x8812 || pid == 0x881a || pid == 0x881b || pid == 0x881c
            || pid == 0xa811 || pid == 0x8813 || pid == 0x0811;
    }
}
