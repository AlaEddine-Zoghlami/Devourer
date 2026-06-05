package com.openipc.pixelpilot;

import android.content.Context;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;

import com.openipc.pixelpilot.databinding.ActivityVideoBinding;

/**
 * LinkModeCoordinator — seamless WFB-ng <-> APFPV switching, mirroring exactly
 * what the VRX gsmenu does for "set gs system rx_mode" but adapted to the phone.
 *
 * THE VRX SEQUENCE (from gsmenu.sh, verified):
 *   to apfpv:  stop adaptive-link + wifibroadcast services
 *              disable them in /etc/default
 *              rmmod 8812eu / 88XXau_wfb ; modprobe them   (RELOAD THE DRIVER)
 *              write wpa_supplicant.apfpv.conf
 *              bring wlx iface up with udhcpc.apfpv.script (no routes/dns)
 *   to wfb:    ifdown + remove wlx iface configs
 *              rmmod / modprobe                              (RELOAD THE DRIVER)
 *              enable wifibroadcast + adaptive-link, start them
 *
 * THE KEY INSIGHT: the switch is NOT just a flag. The VRX RELOADS the USB
 * driver module (rmmod/modprobe) because the dongle must be re-initialized in a
 * different mode (monitor vs managed/station). On the phone the equivalent is:
 *   - fully stop the active LinkManager (release the UsbDevice connection)
 *   - close the native driver (frees the libusb handle on that fd)
 *   - re-open the USB device and re-init the driver in the target mode
 * That is what makes the switch SEAMLESS rather than requiring an app restart:
 * we tear down one transport's hold on the dongle and hand the same fd to the
 * other, exactly as rmmod/modprobe hands the netdev between drivers.
 */
public class LinkModeCoordinator {
    /**
     * Three link modes:
     *   WFB        — RTL8812AU dongle in monitor mode (devourer/WfbNgLink).
     *   APFPV      — RTL8812AU dongle in station mode (devourer/ApfpvStaLink).
     *   APFPV_WIFI — the PHONE'S Wi-Fi chip associates to greg's AP, no dongle.
     *                Video arrives as plain UDP to the existing 5600 socket;
     *                air config still SSH to 192.168.0.1; RSSI from WifiManager.
     */
    public enum Mode { WFB, APFPV, APFPV_WIFI }

    private final Context context;
    private final ActivityVideoBinding binding;
    private final WfbLinkManager wfb;
    private final ApfpvLinkManager apfpv;
    private final com.openipc.pixelpilot.apfpv.ApfpvWifiManager apfpvWifi;
    private final UsbManager usbManager;
    private Mode current;

    public interface ModeChangeListener { void onModeChanged(Mode m, boolean ok, String detail); }

    public LinkModeCoordinator(Context ctx, ActivityVideoBinding b,
                               WfbLinkManager wfb, ApfpvLinkManager apfpv,
                               com.openipc.pixelpilot.apfpv.ApfpvWifiManager apfpvWifi,
                               Mode initial) {
        this.context = ctx; this.binding = b; this.wfb = wfb; this.apfpv = apfpv;
        this.apfpvWifi = apfpvWifi;
        this.usbManager = (UsbManager) ctx.getSystemService(Context.USB_SERVICE);
        this.current = initial;
    }

    public Mode current() { return current; }

    /**
     * Seamless switch. Equivalent of the VRX's stop -> rmmod/modprobe ->
     * reconfigure -> start. Returns immediately; result via listener.
     *
     * @param target   the mode to switch to
     * @param ssid     APFPV SSID (ignored for WFB)
     * @param pass     APFPV password (ignored for WFB)
     */
    public synchronized void switchTo(Mode target, String ssid, String pass, ModeChangeListener cb) {
        if (target == current) { if (cb != null) cb.onModeChanged(current, true, "already in mode"); return; }

        // 1. STOP the active stack. Dongle modes release the libusb fd; the
        //    phone-Wi-Fi mode tears down its network binding + association.
        switch (current) {
            case WFB:        wfb.stopAdapters();   break;
            case APFPV:      apfpv.stopAdapters(); break;
            case APFPV_WIFI: if (apfpvWifi != null) apfpvWifi.stop(); break;
        }

        // 2. Persist the new mode (cold start comes up in the right mode).
        String modeStr = target == Mode.APFPV ? "apfpv"
                       : target == Mode.APFPV_WIFI ? "apfpv_wifi" : "wfb";
        context.getSharedPreferences("pixelpilot", Context.MODE_PRIVATE).edit()
            .putString("link_mode", modeStr)
            .putString("apfpv_ssid", ssid != null ? ssid : "OpenIPC")
            .putString("apfpv_pass", pass != null ? pass : "12345678")
            .apply();

        // 3. START the target mode.
        current = target;
        boolean ok;
        String detail;
        switch (target) {
            case APFPV:
                apfpv.setCredentials(ssid != null ? ssid : "OpenIPC",
                                     pass != null ? pass : "12345678");
                ok = rebind(apfpv::startAdapter);
                detail = ok ? "switched to APFPV (dongle)" : "no RTL8812AU dongle found";
                break;
            case APFPV_WIFI:
                // No dongle. Associate the phone's Wi-Fi to the AP and bind
                // sockets to it; video arrives on the existing 5600 UDP socket.
                if (apfpvWifi == null) { ok = false; detail = "wifi manager unavailable"; break; }
                apfpvWifi.setCredentials(ssid != null ? ssid : "OpenIPC",
                                         pass != null ? pass : "12345678");
                apfpvWifi.start();
                ok = true;  // async; real result arrives via the manager's listener
                detail = "joining " + (ssid != null ? ssid : "OpenIPC") + " (phone Wi-Fi)\u2026";
                break;
            case WFB:
            default:
                ok = rebind(wfb::startAdapter);
                detail = ok ? "switched to WFB-ng (dongle)" : "no RTL8812AU dongle found";
                break;
        }
        if (cb != null) cb.onModeChanged(current, ok, detail);
    }

    /** Find the AU dongle and hand it to the target manager's startAdapter. */
    private boolean rebind(java.util.function.Function<UsbDevice, Boolean> starter) {
        if (usbManager == null) return false;
        for (UsbDevice dev : usbManager.getDeviceList().values()) {
            if (dev.getVendorId() == 0x0bda) {   // Realtek (RTL8812AU family)
                Boolean r = starter.apply(dev);
                return r != null && r;
            }
        }
        return false;
    }
}
