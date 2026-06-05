#pragma once
#include <cstdint>
#include <array>
#include <functional>
#include "FrameParser.h"
#include "Wpa2Supplicant.h"
#include "LqFeedback.h"
namespace apfpv {
using Mac = std::array<uint8_t,6>;
class ApfpvStation;  // fwd: liveness/deauth notifications for the reconnect supervisor
class RxDeframe {
public:
    using OnRtpFn = std::function<void(const uint8_t*, size_t)>;
    RxDeframe(const Mac& self, const Mac& bssid, Wpa2Supplicant* wpa, LqFeedback* lq, OnRtpFn onRtp);
    void onPacket(const Packet& pkt);
    void setStation(ApfpvStation* st) { _station = st; }
    // DHCP reply hook: received UDP -> port 68 (BOOTP client) forwarded here so
    // the DHCP state machine can advance OFFER->REQUEST->ACK->Bound.
    using OnDhcpFn = std::function<void(const uint8_t*, size_t)>;
    void setDhcpSink(OnDhcpFn fn) { _onDhcp = std::move(fn); }
private:
    static int toDbm(uint8_t r);
    Mac _self, _bssid; Wpa2Supplicant* _wpa; LqFeedback* _lq; OnRtpFn _onRtp;
    OnDhcpFn _onDhcp;
    ApfpvStation* _station = nullptr;
};
}
