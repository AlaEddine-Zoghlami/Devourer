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
    // General-IP sink (the VpnService TUN): the FULL decrypted IPv4 packet, so SSH/any
    // TCP+UDP traverses the dongle — not just RTP. The OS then routes it (RTP->5600, SSH->22).
    using OnIpFn = std::function<void(const uint8_t*, size_t)>;
    void setIpSink(OnIpFn fn) { _onIp = std::move(fn); }
    // ARP responder: answer "who has <ourIp>?" so peers (the VTX unicasting RTP video, SSH,
    // ...) keep a FRESH ARP entry for us — without it a STALE entry fails to re-validate and
    // the unicast stream stops after a frame or two. send() gets the encrypted ARP-reply MPDU.
    void setArp(uint32_t ourIp, std::function<void(const std::vector<uint8_t>&)> send) {
        _ourIp = ourIp; _arpSend = std::move(send);
    }
private:
    static int toDbm(uint8_t r);
    Mac _self, _bssid; Wpa2Supplicant* _wpa; LqFeedback* _lq; OnRtpFn _onRtp;
    OnDhcpFn _onDhcp; OnIpFn _onIp;
    uint32_t _ourIp = 0;
    std::function<void(const std::vector<uint8_t>&)> _arpSend;
    ApfpvStation* _station = nullptr;
    // Per-payload-type recent-RTP-seq window for dropping 802.11-retransmit duplicates
    // (reordered retransmits need a window, not just the previous seq) + debug counters.
    uint16_t _seqHist[128][128] = {};
    bool     _seqValid[128][128] = {};
    uint8_t  _seqPos[128] = {};
    int      _dbgRx = 0, _dbgDrop = 0;
    // RX-health instrumentation (logged to logcat tag "rxd-health"): decrypt failures + RTP-seq-gap
    // loss, to diagnose dongle jumps/rewinds (loss vs duplication vs decrypt failure).
    int      _dbgDecFail = 0, _dbgLoss = 0;
    uint16_t _lastSeq[128] = {};
    bool     _lastSeqV[128] = {};
};
}
