#pragma once
#include <cstdint>
#include <array>
#include <map>
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
    // ARP responder: answer "who has <ourIp>?"
    void setArp(uint32_t ourIp, std::function<void(const std::vector<uint8_t>&)> send) {
        _ourIp = ourIp; _arpSend = std::move(send);
    }
    // Software Block-Ack: send a raw 802.11 control frame (the BA response) through
    // the TX path. Called from onPacket when A-MPDU subframes need a BA response.
    void setBaSend(std::function<void(const uint8_t*, size_t)> send) { _baSend = std::move(send); }
private:
    static int toDbm(uint8_t r);
    Mac _self, _bssid; Wpa2Supplicant* _wpa; LqFeedback* _lq; OnRtpFn _onRtp;
    OnDhcpFn _onDhcp; OnIpFn _onIp;
    uint32_t _ourIp = 0;
    std::function<void(const std::vector<uint8_t>&)> _arpSend;
    std::function<void(const uint8_t*, size_t)> _baSend;  // software Block-Ack TX
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
    // Bottleneck diag: cumulative time (ns) spent in onPacket phases per interval
    int64_t  _diagTotalNs = 0, _diagDecNs = 0, _diagFwdNs = 0;
    int      _diagPkts = 0;
    // A-MPDU RX reorder buffer (kernel: recv_indicatepkt_reorder).
    // Per-TID sliding window for in-order delivery of A-MPDU subframes.
    // Without this, out-of-order subframes are dropped -> video corruption.
    struct ReorderCtl {
        bool     enable = false;
        uint16_t indicate_seq = 0xffff;  // next expected seq (mod 4096)
        uint8_t  wsize_b = 64;            // window size in frames
        std::map<uint16_t, std::vector<uint8_t>> pending; // seq -> plaintext frame
    };
    ReorderCtl _reorder[16];  // one per TID (0-15)
    // Software Block-Ack session per TID (mac80211 does this in software).
    // Tracks received sequence numbers and generates BA response frames.
    struct BaSession {
        bool     active = false;
        uint16_t startSeq = 0;      // first seq in the BA window
        uint64_t bitmap = 0;        // 64 bits, bit N = received seq (startSeq+N)
        int      count = 0;         // frame count since last BA sent
    };
    BaSession _baSessions[16];
    // Build and send a compressed Block-Ack control frame for TID `tid`.
    void sendSoftwareBA(uint8_t tid);
    // Process a decrypted QoS-data frame through the reorder buffer for TID `tid`.
    // Delivers frames in-order via onRtpFn. Returns true if delivered, false if queued/dropped.
    bool processReorder(uint8_t tid, uint16_t seq, const uint8_t* llc, size_t llcLen);
};
}
