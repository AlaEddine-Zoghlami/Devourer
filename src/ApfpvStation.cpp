// ============================================================================
//  ApfpvStation.cpp — orchestrator + DRIVER-LEVEL persistent reconnect
//  Mirrors the VRX's wpa_supplicant -B daemon behavior at the driver level:
//  a supervisor thread re-establishes the link on loss (deauth or RX timeout),
//  with no app/UI involvement. Faster than the VRX on recovery because we
//  re-arm on the known channel (no full rescan).
// ============================================================================
#include "ApfpvStation.h"
#include "StationMode.h"
#include "Wpa2Supplicant.h"
#include "Wpa2Crypto.h"
#include "ApfpvDhcp.h"
#include "RxDeframe.h"
#include "StationTxDesc.h"
#include "Dot11Frames.h"
#include "FrameParser.h"
#include "ScanProbe.h"
#include "LqFeedback.h"
#include "RtlUsbAdapter.h"
#include "RtlJaguarDevice.h"
#include "SelectedChannel.h"
#include "RadioManagementModule.h"
#include <algorithm>
#include <cstring>
#ifdef __ANDROID__
#include <android/log.h>
#define SCANLOG(...) __android_log_print(ANDROID_LOG_INFO, "apfpv-scan", __VA_ARGS__)
#else
#define SCANLOG(...) ((void)0)
#endif

namespace apfpv {

// Map a primary channel + desired bandwidth (MHz) to a SelectedChannel kept
// INSIDE the legal DE APFPV band 5170-5250 MHz (5.2 GHz UNII-1; 200 mW @ 20 MHz
// under the 10 mW/MHz PSD cap). For 40 MHz we pick the HT40 secondary side that
// never crosses 5170/5250 — primary 36/44 extend ABOVE (offset LOWER = primary
// is the lower 20 MHz), primary 40/48 extend BELOW (offset UPPER); both land on
// centres 38 (5170-5210) or 46 (5210-5250). 20 MHz is pass-through; 80 MHz
// centres on 42 and fills the band exactly. NB: WFB's 5.8 GHz / 25 mW band is a
// separate code path and is unaffected by this helper.
static SelectedChannel legalApfpvChannel(int ch, int bwMHz) {
    uint8_t c = (uint8_t)(ch > 0 ? ch : 40);
    if (bwMHz >= 80)
        return SelectedChannel{ c, (uint8_t)HAL_PRIME_CHNL_OFFSET_DONT_CARE, CHANNEL_WIDTH_80 };
    if (bwMHz == 40) {
        uint8_t off = (c == 36 || c == 44) ? (uint8_t)HAL_PRIME_CHNL_OFFSET_LOWER
                                           : (uint8_t)HAL_PRIME_CHNL_OFFSET_UPPER;
        return SelectedChannel{ c, off, CHANNEL_WIDTH_40 };
    }
    return SelectedChannel{ c, (uint8_t)HAL_PRIME_CHNL_OFFSET_DONT_CARE, CHANNEL_WIDTH_20 };
}

void ApfpvStation::set(State s) {
    _state.store(s);
    // 0Idle 1Scan 2Arm 3Auth 4Assoc 5Handshake 6Dhcp 7Stream 8FailNoAp 9FailTx
    // 10FailNoAck 11FailAuth 12FailDhcp 13LinkLost 14Reconnecting
    SCANLOG("apfpv state -> %d", (int)s);
    if (_onState) _onState(s);
}

ApfpvStation::ApfpvStation(void* dev, void* rm, OnRtpFn onRtp, OnStateFn onState)
    : _dev(dev), _rm(rm), _onRtp(std::move(onRtp)), _onState(std::move(onState)) {}

ApfpvStation::~ApfpvStation() { stopBeaconCal(); disconnect(); }

// VRX EIRP-calibration beacon: inject an open beacon periodically so a second
// phone can scan + measure the dongle's EIRP. Not a station while beaconing.
void ApfpvStation::startBeaconCal(const std::string& ssid, int channel, int txIndex) {
    stopBeaconCal();
    disconnect();                       // can't be a station and beacon at once
    if (!_rtl || !_dev) return;
    auto* rtl = reinterpret_cast<RtlJaguarDevice*>(_rtl);
    auto* dev = reinterpret_cast<RtlUsbAdapter*>(_dev);
    try {
        rtl->Init([](const Packet&){}, legalApfpvChannel(channel, 20));   // tune + bring up
        rtl->SetTxPower((uint8_t)std::max(0, std::min(63, txIndex)));
    } catch (...) { return; }
    // our MAC (REG_MACID); synthesize a locally-administered one if unprogrammed
    Mac self{};
    for (int i = 0; i < 6; ++i) self[i] = dev->rtw_read8(0x0610 + i);
    bool bad = true; for (int i = 0; i < 6; ++i) if (self[i] != 0 && self[i] != 0xFF) bad = false;
    if (bad) { const uint8_t m[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
               for (int i = 0; i < 6; ++i) self[i] = m[i]; }
    auto mpdu = BuildBeacon(self, ssid, (uint8_t)channel);
    _beaconRun.store(true);
    _beaconThread = std::thread([this, dev, mpdu]() {
        std::vector<uint8_t> frame(40 + mpdu.size(), 0);   // 40 = 8812 TX desc
        std::memcpy(frame.data() + 40, mpdu.data(), mpdu.size());
        FillStationTxDesc(frame.data(), (uint16_t)mpdu.size(), 40,
                          0, StationFrameKind::BroadcastMgmt, 0, 0x04);   // BMC=1, no ACK
        int n = 0;
        while (_beaconRun.load()) {
            bool ok = false;
            try { ok = dev->send_packet(frame.data(), frame.size()); } catch (...) {}
            if ((n++ % 20) == 0) SCANLOG("beacon: tx #%d ok=%d len=%zu", n, (int)ok, frame.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));   // ~10 beacons/s
        }
    });
}

void ApfpvStation::stopBeaconCal() {
    _beaconRun.store(false);
    if (_beaconThread.joinable()) _beaconThread.join();
}

// The gated establishment chain (arm -> auth -> assoc -> WPA2 -> DHCP -> stream).
// Returns true only on a held, streaming link. Used for both initial connect
// AND each supervisor reconnect attempt.
bool ApfpvStation::runConnectChain() {
    auto& dev = *reinterpret_cast<RtlUsbAdapter*>(_dev);
    auto& rm  = *reinterpret_cast<RadioManagementModule*>(_rm);
    auto sendFrame = [&dev](const std::vector<uint8_t>& f) {
        return dev.send_packet(const_cast<uint8_t*>(f.data()), f.size());
    };
    StationMode sta(dev, rm, sendFrame);
    MacAddr self{}, bssid{};
    // NOTE: our MAC is read from REG_MACID AFTER the device is brought up (below).
    // Reading it before Init returned EFUSE-unloaded garbage (e.g. ea:ea:ea:...).

    // Bring the PHY/RF up BEFORE any channel change (scanForSsid + arm call
    // set_channel_bwmode -> phy_SwChnl8812, which null-derefs if never Init'd),
    // AND wire the RX so discovery (beacons -> onScanFrame) and the arm handshake
    // (auth/assoc/deauth -> onMgmtFrame) actually receive. Previously this used a
    // no-op processor, so _scanResult and _gotAuthResp were never set -> the link
    // always failed at discovery. The real RxDeframe RX is wired after handshake.
    { std::lock_guard<std::mutex> lk(_scanMtx); _scanSeen.clear(); }   // diag: SSIDs heard
    // Run the device RX loop on its OWN thread. RtlJaguarDevice::Init is a BLOCKING
    // infinite read loop ("Listening air..."); calling it inline froze the connect
    // on the hint channel so scanForSsid never hopped (it only ever heard ch40).
    // The connect thread now drives channel hops + arm while this thread feeds RX.
    // One dispatch routes by phase: discovery/arm -> StationMode; streaming -> RxDeframe.
    if (_rtl) {
        auto* rtl = reinterpret_cast<RtlJaguarDevice*>(_rtl);
        StationMode* sp = &sta;
        auto dispatch = [sp, this](const Packet& pkt){
            _rxReady.store(true);
            if (_rxPhase.load() == 1) { RxDeframe* rx = _rx.get(); if (rx) rx->onPacket(pkt); return; }
            const uint8_t* f = pkt.Data.data(); size_t n = pkt.Data.size();
            if (n < 24) return;
            uint16_t fc = (uint16_t)(f[0] | (f[1] << 8));
            sp->onScanFrame(f, n);                  // beacons -> _scanResult
            std::string ss; ApInfo bi;
            if (ScanProbe::parseAnyBeacon(f, n, ss, bi) && !ss.empty()) {
                std::lock_guard<std::mutex> lk(_scanMtx);
                if (_scanSeen.insert(ss).second)
                    SCANLOG("discovery heard \"%s\" ch%d", ss.c_str(), bi.channel);
            }
            MacAddr a1{}, a2{};
            std::memcpy(a1.b.data(), f + 4,  6);
            std::memcpy(a2.b.data(), f + 10, 6);
            sp->onMgmtFrame(fc, a1, a2);            // auth/assoc/deauth -> flags
        };
        // stop any prior RX thread, then start a fresh one on the init channel
        rtl->should_stop = true;
        if (_rxThread.joinable()) _rxThread.join();
        rtl->should_stop = false;
        _rxPhase.store(0); _rxReady.store(false);
        uint8_t initCh = (uint8_t)(_params.channel > 0 ? _params.channel : 40);
        _rxThread = std::thread([rtl, dispatch, initCh]() {
            try { rtl->Init(dispatch, SelectedChannel{ .Channel=initCh, .ChannelOffset=0,
                                                       .ChannelWidth=CHANNEL_WIDTH_20 }); }
            catch (...) {}
        });
        // wait until the RX loop is live (device brought up) or a short timeout
        using namespace std::chrono;
        auto t0 = steady_clock::now();
        while (!_rxReady.load() && steady_clock::now() - t0 < seconds(4))
            std::this_thread::sleep_for(milliseconds(50));
        // TX POWER: the station auth/assoc TX uses the same chip/PA the beacon-cal
        // (startBeaconCal -> SetTxPower) and the verified-working monitor injection
        // (txdemo SetTxPower(40)) drive — but the connect path NEVER set it, so the
        // auth-req keyed the PA at unset/zero power = on-air silence. Set a solid
        // index now so the frame actually radiates. (Java applies the legal-EIRP
        // clamp via nativeStaSetTxPower; 40 here is the txdemo working value.)
        try { rtl->SetTxPower(40); SCANLOG("station TX power set to idx 40"); }
        catch (...) { SCANLOG("station SetTxPower threw"); }
    }

    // Our MAC, read NOW that the device is up (REG_MACID populated from EFUSE).
    // Used as addr2 in auth/assoc + for WPA2 PTK; a garbage MAC makes the AP
    // reply to a bogus address. 0x0610 = REG_MACID.
    for (int i = 0; i < 6; ++i) self.b[i] = dev.rtw_read8(0x0610 + i);
    bool bad = true; for (int i=0;i<6;i++){ if(self.b[i]!=0 && self.b[i]!=0xFF) bad=false; }
    if (bad) { self.b[0]=0x02; self.b[1]=0x11; self.b[2]=0x22;
               self.b[3]=0x33; self.b[4]=0x44; self.b[5]=0x55; }

    // DISCOVERY: scan beacons for the SSID -> BSSID + channel + negotiated RSN.
    // Removes the hardcoded-channel / empty-BSSID / fixed-cipher assumptions
    // (security + discovery parity with the kernel wpa_supplicant the VRX uses).
    ApInfo ap;
    if (_params.haveBssid) {
        // PHONE-ASSISTED: the phone already resolved the AP's BSSID + channel, so
        // skip the dongle sweep entirely and arm straight to it (CCMP/PSK default).
        for (int i = 0; i < 6; ++i) bssid.b[i] = _params.bssid[i];
        sta.setNegotiatedCipher(0x000FAC04, 0x000FAC04);
        SCANLOG("phone-assisted: BSSID %02x:%02x:%02x:%02x:%02x:%02x ch%d — skipping sweep",
                bssid.b[0],bssid.b[1],bssid.b[2],bssid.b[3],bssid.b[4],bssid.b[5], _params.channel);
    } else if (_params.scan) {
        set(State::Scanning);
        // Sweep beacons for the SSID -> BSSID + channel + negotiated RSN (now works:
        // the RX thread feeds frames while this thread hops channels).
        ap = sta.scanForSsid(_params.ssid.c_str(), _params.channel, /*ms*/2000);
        SCANLOG("scan: \"%s\" found=%d ch=%d", _params.ssid.c_str(), ap.found?1:0, ap.channel);
        if (!ap.found) { set(State::FailNoAp); return false; }
        bssid.b = ap.bssid;
        _params.channel = ap.channel ? ap.channel : _params.channel;
        sta.setNegotiatedCipher(ScanProbe::chooseCipher(ap.pairwise),
                                ap.rsnPresent ? ap.groupCipher : 0x000FAC04);
    }

    set(State::Arming);
    sta.setPhaseCb([this](int p){ set(p == 1 ? State::Authenticating : State::Associating); });
    auto r = sta.runProbe(self, bssid, _params.ssid.c_str(), /*hold*/3);
    SCANLOG("arm result: %d (0=GO 1=Deauth 2=NoAssocResp 3=TXFAIL_NoAuth 4=Error)", (int)r);
    switch (r) {
        case StationMode::Result::TXFAIL_NoAuthResp: set(State::FailTx);   return false;
        case StationMode::Result::NOGO_Deauthed:     set(State::FailNoAck); return false;
        case StationMode::Result::NOGO_NoAssocResp:  set(State::FailAuth);  return false;
        case StationMode::Result::Error:             set(State::FailNoAp);  return false;
        case StationMode::Result::GO_LinkHeld: break;
    }
    set(State::Handshaking);
    // ONE supplicant (member), used for handshake + RX decrypt + encrypted TX.
    Mac selfMac{}, bss{};
    std::copy(self.b.begin(), self.b.end(), selfMac.begin());
    std::copy(bssid.b.begin(), bssid.b.end(), bss.begin());
    _wpa = std::make_unique<Wpa2Supplicant>(MakeWpa2Crypto(), sendFrame);
    _wpa->begin(_params.passphrase, _params.ssid, selfMac, bss);

    // Real (encrypted) DHCP send path — usable once keys are installed.
    auto dhcpSend = [&](const std::vector<uint8_t>& ipDatagram) -> bool {
        if (!_wpa || !_wpa->ready()) return false;
        auto mpdu = _wpa->buildEncryptedData(ipDatagram.data(), ipDatagram.size());
        if (mpdu.empty()) return false;
        std::vector<uint8_t> frame(40 + mpdu.size(), 0);   // 40 = 8812 TX desc
        std::memcpy(frame.data()+40, mpdu.data(), mpdu.size());
        apfpv::FillStationTxDesc(frame.data(), (uint16_t)mpdu.size(), 40,
                                 0, apfpv::StationFrameKind::CcmpData, 0, 0x04);
        return sendFrame(frame);
    };
    _dhcp = std::make_unique<ApfpvDhcp>(self.b, dhcpSend);

    // Register the RX path NOW so EAPOL (handshake) and DHCP replies actually
    // reach the supplicant / DHCP machine before we wait on them.
    if (_params.lqFeedback) { _lq = std::make_unique<LqFeedback>(LqFeedback::Config{});
                              _lq->start("192.168.0.1", 12345); }
    _rx = std::make_unique<RxDeframe>(selfMac, bss, _wpa.get(), _lq.get(), _onRtp);
    _rx->setStation(this);
    { ApfpvDhcp* dh = _dhcp.get();
      _rx->setDhcpSink([dh](const uint8_t* b, size_t n){ dh->onBootpReply(b, n); }); }
    // Switch the ALREADY-RUNNING RX thread to the streaming path — do NOT re-Init
    // (that blocks). EAPOL (handshake), DHCP replies, and RTP now route through
    // RxDeframe via the dispatch. The device is already tuned to the AP's channel.
    _rxPhase.store(1);

    // Wait (bounded) for the 4-way handshake to install keys.
    {
        using namespace std::chrono;
        auto t0 = steady_clock::now();
        while (!_wpa->ready()) {
            if (steady_clock::now() - t0 > seconds(5)) { set(State::FailAuth); return false; }
            std::this_thread::sleep_for(milliseconds(20));
        }
    }

    // Keys installed -> DHCP (or static). For dynamic, wait briefly for the lease.
    set(State::Dhcp);
    if (_params.staticIp) _dhcp->claimStatic_192_168_0_10();
    else {
        _dhcp->start();
        using namespace std::chrono;
        auto t0 = steady_clock::now();
        while (!_dhcp->lease().valid) {
            if (steady_clock::now() - t0 > seconds(4)) break;
            std::this_thread::sleep_for(milliseconds(20));
        }
        if (!_dhcp->lease().valid) _dhcp->claimStatic_192_168_0_10();  // fallback
    }

    // mark link alive and go streaming
    _deauth.store(false);
    _lastRxMs.store(nowMs());
    set(State::Streaming);
    return true;
}

bool ApfpvStation::connect(const Params& p) {
    _params = p;
    bool ok = runConnectChain();
    // Start the persistent reconnect supervisor (the daemon), even if the first
    // attempt failed — it will keep trying, exactly like wpa_supplicant -B.
    if (p.autoReconnect && !_run.exchange(true)) {
        _supervisor = std::thread(&ApfpvStation::supervisorLoop, this);
    }
    return ok;
}

// The supervisor: the driver-level equivalent of the wpa_supplicant daemon.
// Watches for link loss and re-establishes, with exponential backoff capped low
// (recovery is fast: same channel, no rescan).
void ApfpvStation::supervisorLoop() {
    using namespace std::chrono;
    int backoff = _params.reconnectBackoffMs;
    while (_run.load()) {
        std::this_thread::sleep_for(milliseconds(100));
        State s = _state.load();

        if (s == State::Streaming) {
            backoff = _params.reconnectBackoffMs;   // reset backoff on healthy link
            bool lost = _deauth.load();
            // RX-silence watchdog: no data/beacon for rxTimeoutMs => link gone.
            if (!lost && (nowMs() - _lastRxMs.load()) > _params.rxTimeoutMs) lost = true;
            if (lost) {
                set(State::LinkLost);
                _deauth.store(false);
            }
            continue;
        }

        if (s == State::LinkLost || s == State::FailNoAp || s == State::FailNoAck ||
            s == State::FailAuth || s == State::FailTx || s == State::FailDhcp) {
            if (!_run.load()) break;
            set(State::Reconnecting);
            std::this_thread::sleep_for(milliseconds(backoff));
            if (!_run.load()) break;
            // A USB register read/write can throw (rtw_read -> ios_base::failure)
            // if the dongle hiccups or is accessed concurrently. Catch it here so
            // the supervisor THREAD degrades to a failed attempt instead of
            // terminating the whole app (std::terminate -> SIGABRT).
            bool ok = false;
            try { ok = runConnectChain(); }          // re-arm on known channel
            catch (const std::exception&) { ok = false; set(State::FailNoAp); }
            catch (...)                    { ok = false; set(State::FailNoAp); }
            if (!ok) {
                backoff = std::min(backoff * 2, _params.maxBackoffMs);  // exp backoff
            } else {
                backoff = _params.reconnectBackoffMs;
            }
        }
    }
}

void ApfpvStation::disconnect() {
    _run.store(false);
    if (_supervisor.joinable()) _supervisor.join();
    // Stop the RX read loop and join its thread.
    if (_rtl) reinterpret_cast<RtlJaguarDevice*>(_rtl)->should_stop = true;
    if (_rxThread.joinable()) _rxThread.join();
    set(State::Idle);
}

// All-SSID scan: tune the radio across the channel set, collect every beacon,
// and emit each NEW SSID. The RX collector runs on the device read thread; it
// captures only `this`, and _onScanAp is cleared (and the processor swapped to a
// no-op) before we return, so the still-running RX thread can't call a dead cb.
static inline bool isDfsChannel(int ch) {
    return (ch >= 52 && ch <= 64) || (ch >= 100 && ch <= 144);
}

void ApfpvStation::scanAll(int perChannelMs, bool includeDfs, const OnApFn& onAp) {
    if (!_rtl || !_rm) return;
    auto* rtl = reinterpret_cast<RtlJaguarDevice*>(_rtl);
    auto& rm  = *reinterpret_cast<RadioManagementModule*>(_rm);

    { std::lock_guard<std::mutex> lk(_scanMtx); _scanSeen.clear(); _onScanAp = onAp; }
    set(State::Scanning);

    // Diagnostics in MEMBERS so the stored collector (called from the device RX
    // thread) captures only `this` — capturing scanAll locals by ref dangles.
    _scanPkts.store(0); _scanBeacons.store(0);
    auto collector = [this](const Packet& pkt) {
        _scanPkts.fetch_add(1);
        std::string ssid; ApInfo info;
        if (!ScanProbe::parseAnyBeacon(pkt.Data.data(), pkt.Data.size(), ssid, info)) return;
        _scanBeacons.fetch_add(1);
        if (ssid.empty()) return;                       // skip hidden SSIDs
        info.rssi = (int)pkt.RxAtrib.rssi[0] - 110;     // approx dBm (gain byte)
        std::lock_guard<std::mutex> lk(_scanMtx);
        if (_onScanAp && _scanSeen.insert(ssid).second) _onScanAp(ssid, info);
    };
    SCANLOG("scanAll begin (perCh=%d ms)", perChannelMs);

    // hint first, then the full 5GHz set (UNII-1, UNII-2A/2C DFS, UNII-3) and all
    // 2.4GHz channels — APs (incl. phone hotspots/home routers) can sit anywhere,
    // e.g. DFS ch52-144. Passive RX-only listening on DFS is fine (no TX).
    // Trimmed for the ~per-channel cost of a real retune: UNII-1, UNII-2A DFS,
    // UNII-3 + the common 2.4 channels. (Dropped the big UNII-2C 100-144 block.)
    const int channels[] = {
        _params.channel,
        36, 40, 44, 48,            // UNII-1
        52, 56, 60, 64,            // UNII-2A (DFS)
        149, 153, 157, 161, 165,   // UNII-3
        1, 6, 11                   // 2.4GHz (common)
    };
    int last = 36; bool inited = false;
    for (int ch : channels) {
        if (ch <= 0) continue;
        if (!includeDfs && isDfsChannel(ch)) continue;   // skip DFS when disabled
        last = ch;
        SelectedChannel sc{ .Channel=(uint8_t)ch, .ChannelOffset=0,
                            .ChannelWidth=CHANNEL_WIDTH_20 };
        // Tuning is a USB register op (rtw_read/write) that can throw on a dongle
        // hiccup — catch per-channel so a single failure doesn't abort the app;
        // skip the bad channel and keep sweeping.
        try {
            if (!inited) { rtl->Init(collector, sc); inited = true; }     // bring-up + monitor RX
            else { rm.set_channel_bwmode((uint8_t)ch, 0, CHANNEL_WIDTH_20); } // actually retunes
        } catch (...) { continue; }
        std::this_thread::sleep_for(std::chrono::milliseconds(perChannelMs));
    }

    // Detach: no further emits, and replace the RX processor so the device's
    // read thread can't re-enter the collector after onAp's lifetime ends.
    { std::lock_guard<std::mutex> lk(_scanMtx); _onScanAp = nullptr; }
    SCANLOG("scanAll done: %d RX pkts, %d beacons, %zu SSIDs", _scanPkts.load(), _scanBeacons.load(), _scanSeen.size());
    try {
        rtl->Init([](const Packet&){}, SelectedChannel{ .Channel=(uint8_t)last,
                  .ChannelOffset=0, .ChannelWidth=CHANNEL_WIDTH_20 });
    } catch (...) {}
    set(State::Idle);
}

} // namespace apfpv
