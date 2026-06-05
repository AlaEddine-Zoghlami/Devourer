#pragma once
#include <cstdint>
#include <array>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <memory>
#include <chrono>
#include <mutex>
#include <set>
#include "ScanProbe.h"
namespace apfpv {
using Mac = std::array<uint8_t,6>;
// Top-level APFPV station orchestrator: assembles arming -> auth/assoc ->
// WPA2 -> DHCP -> RTP+LQ into one connect() the JNI layer can call.
//
// DRIVER-LEVEL PERSISTENT RECONNECT (matches/exceeds the VRX): the VRX gets
// auto-reconnect from running wpa_supplicant -B as a daemon (it re-associates
// on link loss automatically). We replicate that AT THE DRIVER LEVEL with a
// supervisor thread: it watches for link loss (deauth, or RTP/beacon RX
// timeout) and re-runs the gated connect chain — no app/UI involvement, exactly
// like the supplicant daemon. We can be faster: on loss we re-arm on the SAME
// known channel (no rescan), so recovery is a re-auth/assoc/4-way, not a full
// network search.
class ApfpvStation {
public:
    enum class State { Idle, Scanning, Arming, Authenticating, Associating, Handshaking,
                       Dhcp, Streaming, FailNoAp, FailTx, FailNoAck, FailAuth,
                       FailDhcp, LinkLost, Reconnecting };
    using OnRtpFn   = std::function<void(const uint8_t*, size_t)>;
    using OnStateFn = std::function<void(State)>;
    struct Params { int channel=40; int bandwidth=20; std::string ssid="OpenIPC";
                    std::string passphrase="12345678"; bool lqFeedback=true;
                    bool staticIp=false;
                    // discovery: if scan=true we find BSSID+channel+cipher via
                    // beacons (security+discovery parity with wpa_supplicant);
                    // channel above is the starting hint / fallback.
                    bool scan=true;
                    // reconnect tuning — matched to wpa_supplicant defaults the
                    // VRX relies on: immediate re-associate, ~5s scan interval.
                    bool autoReconnect=true;
                    int  rxTimeoutMs=1500;         // no RTP/beacon => lost
                    int  reconnectBackoffMs=0;     // immediate (wpa_supplicant-like)
                    int  scanIntervalMs=5000;      // wpa_supplicant default scan_interval
                    int  maxBackoffMs=5000; };
    ApfpvStation(void* rtlUsbAdapter, void* radioMgmt, OnRtpFn onRtp, OnStateFn onState);
    // Provide the RtlJaguarDevice so the station can register the RX callback
    // (RxDeframe) as the device packet processor — the receive path.
    void setDevice(void* rtlJaguarDevice) { _rtl = rtlJaguarDevice; }
    ~ApfpvStation();
    bool connect(const Params& p);   // runs the chain + starts the supervisor
    void disconnect();               // stops supervisor + tears down

    // All-SSID RF scan for the UI picker. Channel-hops the APFPV/5GHz/2.4 set,
    // parses every beacon, and calls onAp(ssid, info) once per NEW SSID as it is
    // discovered (live updates). Runs synchronously — call it from a worker
    // thread, and only while NOT connected (it drives the RX path). Requires
    // setDevice() to have been called.
    using OnApFn = std::function<void(const std::string&, const ApInfo&)>;
    // includeDfs=false skips the DFS 5GHz channels (52-64, 100-144) — faster, and
    // avoids tuning the radio onto radar-protected channels.
    void scanAll(int perChannelMs, bool includeDfs, const OnApFn& onAp);
    State state() const { return _state.load(); }
    int   rssiDbm() const { return _rssi.load(); }
    // RX path calls this on every received data/beacon frame so the supervisor
    // knows the link is alive (resets the loss timer). Wire from RxDeframe.
    void notifyRxAlive() { _lastRxMs.store(nowMs()); }
    // RX path calls this if a deauth/disassoc is seen (immediate loss).
    void notifyDeauth()  { _deauth.store(true); }
private:
    void set(State s);
    bool runConnectChain();          // the gated arm->...->stream sequence
    void supervisorLoop();           // persistent reconnect watcher (the daemon)
    static int64_t nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }
    void* _dev; void* _rm; void* _rtl = nullptr; OnRtpFn _onRtp; OnStateFn _onState;
    // RX pipeline (lives across the connection so the device callback stays valid)
    std::unique_ptr<class RxDeframe> _rx;
    std::unique_ptr<class Wpa2Supplicant> _wpa;
    std::unique_ptr<class LqFeedback> _lq;
    std::unique_ptr<class ApfpvDhcp> _dhcp;
    Params _params;
    std::atomic<State> _state{State::Idle};
    std::atomic<int> _rssi{-90};
    std::atomic<int64_t> _lastRxMs{0};
    std::atomic<bool> _deauth{false};
    std::atomic<bool> _run{false};   // supervisor running
    std::thread _supervisor;
    // scanAll() state — the RX collector runs on the device thread, so access is
    // mutex-guarded and _onScanAp is cleared before scanAll returns so the still-
    // running RX thread can never call into a destroyed callback.
    std::mutex _scanMtx;
    std::set<std::string> _scanSeen;
    OnApFn _onScanAp;
};
}
