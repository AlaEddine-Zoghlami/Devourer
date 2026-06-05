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

namespace apfpv {

void ApfpvStation::set(State s) { _state.store(s); if (_onState) _onState(s); }

ApfpvStation::ApfpvStation(void* dev, void* rm, OnRtpFn onRtp, OnStateFn onState)
    : _dev(dev), _rm(rm), _onRtp(std::move(onRtp)), _onState(std::move(onState)) {}

ApfpvStation::~ApfpvStation() { disconnect(); }

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
    // Derive our own MAC from the adapter (REG_MACID, populated from EFUSE by
    // devourer init). Without this, WPA2 PTK derivation + frame addr2 are zero
    // and the AP rejects us. 0x0610 = REG_MACID.
    for (int i = 0; i < 6; ++i) self.b[i] = dev.rtw_read8(0x0610 + i);
    // If unprogrammed (all 0/FF), synthesize a locally-administered MAC.
    bool bad = true; for (int i=0;i<6;i++){ if(self.b[i]!=0 && self.b[i]!=0xFF) bad=false; }
    if (bad) { self.b[0]=0x02; self.b[1]=0x11; self.b[2]=0x22;
               self.b[3]=0x33; self.b[4]=0x44; self.b[5]=0x55; }

    // DISCOVERY: scan beacons for the SSID -> BSSID + channel + negotiated RSN.
    // Removes the hardcoded-channel / empty-BSSID / fixed-cipher assumptions
    // (security + discovery parity with the kernel wpa_supplicant the VRX uses).
    ApInfo ap;
    if (_params.scan) {
        set(State::Scanning);
        // StationMode collects beacons via onMgmtFrame->ScanProbe; we ask it to
        // sweep the channel hint first, then the 5GHz UNII-1 set (36..48) the
        // EMAX VTX uses. Found AP fixes channel + cipher for association.
        ap = sta.scanForSsid(_params.ssid.c_str(), _params.channel, /*ms*/2000);
        if (!ap.found) { set(State::FailNoAp); return false; }
        bssid.b = ap.bssid;
        _params.channel = ap.channel ? ap.channel : _params.channel;
        sta.setNegotiatedCipher(ScanProbe::chooseCipher(ap.pairwise),
                                ap.rsnPresent ? ap.groupCipher : 0x000FAC04);
    }

    set(State::Arming);
    auto r = sta.runProbe(self, bssid, _params.ssid.c_str(), /*hold*/3);
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
    if (_rtl) {
        auto* rtl = reinterpret_cast<RtlJaguarDevice*>(_rtl);
        RxDeframe* rx = _rx.get();
        rtl->Init([rx](const Packet& pkt){ rx->onPacket(pkt); },
                  SelectedChannel{ .Channel=(uint8_t)_params.channel, .ChannelOffset=0,
                                   .ChannelWidth=CHANNEL_WIDTH_20 });
    }

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
            bool ok = runConnectChain();            // re-arm on known channel
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
    set(State::Idle);
}

// All-SSID scan: tune the radio across the channel set, collect every beacon,
// and emit each NEW SSID. The RX collector runs on the device read thread; it
// captures only `this`, and _onScanAp is cleared (and the processor swapped to a
// no-op) before we return, so the still-running RX thread can't call a dead cb.
void ApfpvStation::scanAll(int perChannelMs, const OnApFn& onAp) {
    if (!_rtl || !_rm) return;
    auto* rtl = reinterpret_cast<RtlJaguarDevice*>(_rtl);
    auto& rm  = *reinterpret_cast<RadioManagementModule*>(_rm);

    { std::lock_guard<std::mutex> lk(_scanMtx); _scanSeen.clear(); _onScanAp = onAp; }
    set(State::Scanning);

    auto collector = [this](const Packet& pkt) {
        std::string ssid; ApInfo info;
        if (!ScanProbe::parseAnyBeacon(pkt.Data.data(), pkt.Data.size(), ssid, info)) return;
        if (ssid.empty()) return;                       // skip hidden SSIDs
        info.rssi = (int)pkt.RxAtrib.rssi[0] - 110;     // approx dBm (gain byte)
        std::lock_guard<std::mutex> lk(_scanMtx);
        if (_onScanAp && _scanSeen.insert(ssid).second) _onScanAp(ssid, info);
    };

    // hint first, then 5GHz UNII-1 (APFPV/EMAX), upper-5GHz UNII-3, then 2.4.
    const int channels[] = { _params.channel, 36, 40, 44, 48,
                             149, 153, 157, 161, 1, 6, 11 };
    int last = 36; bool inited = false;
    for (int ch : channels) {
        if (ch <= 0) continue;
        last = ch;
        SelectedChannel sc{ .Channel=(uint8_t)ch, .ChannelOffset=0,
                            .ChannelWidth=CHANNEL_WIDTH_20 };
        if (!inited) { rtl->Init(collector, sc); inited = true; }  // starts RX
        else         { rm.set_channel_bwmode((uint8_t)ch, 0, CHANNEL_WIDTH_20); }
        std::this_thread::sleep_for(std::chrono::milliseconds(perChannelMs));
    }

    // Detach: no further emits, and replace the RX processor so the device's
    // read thread can't re-enter the collector after onAp's lifetime ends.
    { std::lock_guard<std::mutex> lk(_scanMtx); _onScanAp = nullptr; }
    rtl->Init([](const Packet&){}, SelectedChannel{ .Channel=(uint8_t)last,
              .ChannelOffset=0, .ChannelWidth=CHANNEL_WIDTH_20 });
    set(State::Idle);
}

} // namespace apfpv
