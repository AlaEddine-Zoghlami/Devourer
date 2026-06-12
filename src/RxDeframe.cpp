// ============================================================================
//  RxDeframe.cpp — 802.11 data -> UDP/RTP -> port 5600 + RSSI tap (impl)
//  Declarations in RxDeframe.h. Wire onPacket() as devourer's RX callback.
// ============================================================================
#include "RxDeframe.h"
#include "ApfpvStation.h"
#include <cstring>
#include <cstdio>
#include <chrono>
#include <android/log.h>

// --- throughput bottleneck diag: logs timing breakdown every 500 packets ---
#define RX_DIAG_INTERVAL 500
namespace apfpv {

RxDeframe::RxDeframe(const Mac& self, const Mac& bssid, Wpa2Supplicant* wpa,
                     LqFeedback* lq, OnRtpFn onRtp)
    : _self(self), _bssid(bssid), _wpa(wpa), _lq(lq), _onRtp(std::move(onRtp)) {}

int RxDeframe::toDbm(uint8_t r) {
    // RxAtrib.rssi[] is devourer's gain_trsw byte for the path: the RX
    // descriptor's {TRSW(bit7), gain[6:0]} field (FrameParser.cpp fills it from
    // driver_data.gain_trsw). It is a GAIN index, not dBm — so the placeholder
    // r/2-95 was wrong. The RTL8812 (Jaguar) phystatus path in the Realtek HAL
    // converts the OFDM RX gain to power as:  rx_pwr_dBm = gain_index - 110
    // (the phydm rx_pwr_all base; LNA/VGA gain index maps ~1:1 to dB, offset by
    // the front-end reference of -110 dBm at gain 0). We mask the TRSW bit and
    // apply that. Clamp to a sane RF window.
    int gain = r & 0x7F;                 // strip TRSW flag, keep gain[6:0]
    int dbm  = gain - 110;               // Jaguar OFDM gain-index -> dBm
    if (dbm < -100) dbm = -100;
    if (dbm > -10)  dbm = -10;
    return dbm;
}

void RxDeframe::onPacket(const Packet& pkt) {
    if (_station) _station->notifyRxAlive();   // any RX = link alive (supervisor)
    if (_lq) _lq->update(toDbm(pkt.RxAtrib.rssi[0]), toDbm(pkt.RxAtrib.rssi[1]));

    const uint8_t* f = pkt.Data.data();
    size_t len = pkt.Data.size();
    if (len < 24) return;

    uint16_t fc = f[0] | (f[1] << 8);
    // mgmt deauth/disassoc from our AP -> immediate link-loss signal
    if (((fc >> 2) & 0x3) == 0x0) {
        uint8_t sub = (fc >> 4) & 0xF;
        if (sub == 0xC || sub == 0xA) {
            if (_station) { // deauth/disassoc from AP -> immediate link-loss
                if (!_wpa || !_wpa->pmfActive() || _wpa->verifyProtectedMgmt(f, len))
                    _station->notifyDeauth();
            }
            return;
        }
        // 802.11 Action frame (subtype 0xD): could be ADDBA Request from the AP.
        // The AP MUST receive an ADDBA Response to start A-MPDU aggregation so we can
        // receive 30-64 frames per TXOP (needed for 65+ Mbps). Accept TID 0 (best-effort).
        if (sub == 0xD && _station && len >= 24 + 3) {
            const uint8_t* body = f + 24; // 3-addr mgmt header
            if (body[0] == 0x03 && body[1] == 0x00) { // BlockAck category, ADDBA Request
                _station->handleAddbaRequest(f, len);
            }
        }
        return;
    }
    if (((fc >> 2) & 0x3) != 0x2) return;          // data frames only
    if (!(fc & 0x0200)) return;                    // from-DS (AP->STA)
    // fprintf removed: blocks RX thread at 65Mbps — use rxd-diag instead
    // Accept unicast-to-us OR group-addressed (I/G bit in A1[0]): DHCP OFFER/ACK + ARP are
    // broadcast and FPV video may be multicast. Group frames decrypt with the GTK below.
    if (!(f[4] & 0x01) && std::memcmp(f + 4, _self.data(), 6) != 0) return;

    size_t hdrLen = 24;
    if ((fc & 0x0300) == 0x0300) hdrLen += 6;      // 4-addr
    if (fc & 0x0080) hdrLen += 2;                  // QoS-data (subtype>=8): 2B QoS Control
                                                   // (was 0x8000 = Order flag — wrong; EAPOL
                                                   // M1 is QoS-data fc=0x0288, so this was the
                                                   // header-offset bug that hid the handshake)
    if (len <= hdrLen) return;

    const uint8_t* body = f + hdrLen; size_t bodyLen = len - hdrLen;
    // Pre-allocated buffer per thread — avoids heap alloc per packet (5600/s at 65Mbps)
    static thread_local std::vector<uint8_t> plainBuf(2048);
    const uint8_t* llc; size_t llcLen;
    auto t0 = std::chrono::steady_clock::now();
    if (fc & 0x4000) {                             // Protected
        if (!_wpa || !_wpa->ready()) return;
        if (!_wpa->decryptData(f, len, plainBuf)) {
            _dbgDecFail++;
            return;
        }
        llc = plainBuf.data(); llcLen = plainBuf.size();
    } else { llc = body; llcLen = bodyLen; }
    auto t1 = std::chrono::steady_clock::now();   // after CCMP decrypt (or skip)

    if (llcLen < 8) return;
    static const uint8_t snap[6] = {0xaa,0xaa,0x03,0x00,0x00,0x00};
    if (std::memcmp(llc, snap, 6) != 0) return;
    uint16_t ethertype = (llc[6] << 8) | llc[7];
    // EAPOL (0x888E): the WPA2 4-way handshake. MUST route to the supplicant or
    // the handshake never completes and the link stalls at Handshaking.
    if (ethertype == 0x888E) {
        if (_wpa) _wpa->onEapolKey(llc + 8, llcLen - 8);
        return;
    }
    // A-MPDU reorder for QoS-data video frames (IPv4/UDP/5600). Pass-through
    // for in-order frames; reorders out-of-order A-MPDU subframes. Non-video
    // frames (ARP, DHCP, EAPOL, etc.) use the existing direct path below.
    if ((fc & 0x0088) == 0x0088 && ethertype == 0x0800 && llcLen >= 28) {
        // Quick check: is this likely a video RTP frame destined for port 5600?
        const uint8_t* ip = llc + 8;
        size_t ipl = llcLen - 8;
        if (ipl >= 20 && (ip[0] >> 4) == 4 && ip[9] == 17) {  // IPv4 + UDP
            size_t ihl = (ip[0] & 0x0f) * 4;
            if (ipl >= ihl + 8) {
                const uint8_t* u = ip + ihl;
                if (((u[2] << 8) | u[3]) == 5600) {  // RTP port
                    uint8_t tid = (hdrLen >= 26) ? (f[24] & 0x0f) : 0;
                    uint16_t seq = (uint16_t)((f[23] << 4) | (f[22] >> 4));
                    _dbgRx++;
                    // Health summary every 120 pkts (same interval as old path)
                    if ((_dbgRx % 120) == 0) {
                        uint8_t pt = ip[9]; // actually protocol field... let's use RTP pt
                        (void)pt;
                        __android_log_print(ANDROID_LOG_INFO, "rxd-health",
                            "rx=%d dropDup=%d decFail=%d lost=%d (reorder) rxrate=%u",
                            _dbgRx, _dbgDrop, _dbgDecFail, _dbgLoss,
                            (unsigned)pkt.RxAtrib.data_rate);
                    }
                    processReorder(tid, seq, llc, llcLen);
                    return;  // handled by reorder — skip duplicate processing
                }
            }
        }
    }

    // ARP responder: reply to "who has <ourIp>?" so peers keep a fresh entry for us and the
    // unicast RTP/SSH stream doesn't stall when their STALE entry needs re-validation.
    if (ethertype == 0x0806 && _ourIp && _arpSend && _wpa && llcLen >= 8 + 28) {
        const uint8_t* a = llc + 8;                          // ARP payload
        uint32_t tip = (a[24]<<24)|(a[25]<<16)|(a[26]<<8)|a[27];
        if (a[6]==0x00 && a[7]==0x01 && tip == _ourIp) {     // a request for OUR IP
            uint8_t r[28]; std::memcpy(r, a, 28);
            r[6]=0x00; r[7]=0x02;                            // oper = reply
            std::memcpy(r+8,  _self.data(), 6);              // sender HW = us
            std::memcpy(r+14, a+24, 4);                      // sender IP = our IP
            std::memcpy(r+18, a+8, 6);                       // target HW = requester
            std::memcpy(r+24, a+14, 4);                      // target IP = requester
            auto m = _wpa->buildEncryptedData(r, 28, 0x0806);
            if (!m.empty()) _arpSend(m);
        }
        return;
    }
    if (ethertype != 0x0800) return; // IPv4
    const uint8_t* ip = llc + 8; size_t ipLen = llcLen - 8;
    // De-dup 802.11 retransmissions of RTP BEFORE any sink: as a station we RX unicast RTP
    // and the AP retransmits each frame until ACKed (~6x). This must run ahead of the _onIp
    // (TUN) path below — otherwise the duplicates reach the decoder via the OS route. Retries
    // reuse the RTP seq, so drop a UDP/5600 packet whose (payload-type, seq) repeats the last.
    if (ipLen >= 20 && (ip[0] >> 4) == 4 && ip[9] == 17) {
        size_t ihl0 = (ip[0] & 0x0f) * 4;
        if (ipLen >= ihl0 + 8 + 4) {
            const uint8_t* u = ip + ihl0;
            if (((u[2] << 8) | u[3]) == 5600) {
                const uint8_t* r = u + 8;
                uint8_t  pt = r[1] & 0x7f;
                uint16_t sq = (uint16_t)((r[2] << 8) | r[3]);
                _dbgRx++;
                // Window de-dup: a retransmit reuses the RTP seq but can arrive reordered,
                // so check the last 128 seqs for this pt (not just the previous packet).
                bool dup = false;
                for (int i = 0; i < 128; i++) if (_seqValid[pt][i] && _seqHist[pt][i] == sq) { dup = true; break; }
                if (dup) { _dbgDrop++; return; }   // duplicate -> drop before _onIp + shortcut
                // RX-seq-gap loss: how many packets between this and the last UNIQUE one for this pt.
                if (_lastSeqV[pt]) {
                    int gap = (int)(uint16_t)(sq - _lastSeq[pt]);
                    if (gap > 1 && gap < 2000) _dbgLoss += (gap - 1);
                }
                _lastSeq[pt] = sq; _lastSeqV[pt] = true;
                // Health summary every 120 unique pkts: received / dup-dropped / decrypt-failed / lost.
                if ((_dbgRx % 120) == 0)
                    __android_log_print(ANDROID_LOG_INFO, "rxd-health",
                        "rx=%d dropDup=%d decFail=%d lost=%d (pt=%u seq=%u) rxrate=%u",
                        _dbgRx, _dbgDrop, _dbgDecFail, _dbgLoss, pt, sq,
                        (unsigned)pkt.RxAtrib.data_rate);
                        // rxrate: 0-3=CCK 1/2/5.5/11, 4-11=OFDM 6..54, 12-27=HT MCS0-15,
                        // 28+=VHT. Low (<12) => rate-control collapsed (ACK problem);
                        // high MCS but sparse rx => A-MPDU/Block-Ack aggregation missing.
                _seqHist[pt][_seqPos[pt]] = sq; _seqValid[pt][_seqPos[pt]] = true;
                _seqPos[pt] = (uint8_t)((_seqPos[pt] + 1) & 127);
            }
        }
    }
    // GENERAL-IP BRIDGE: hand the WHOLE IPv4 packet to the TUN sink so SSH / any TCP+UDP
    // traverses the dongle (it becomes a full L3 interface, not RTP-only). The OS then
    // routes it. The RTP->5600 + DHCP shortcuts below stay for the dongle-only demo path.
    if (_onIp) _onIp(ip, ipLen);
    if (ipLen < 20 || (ip[0] >> 4) != 4 || ip[9] != 17) return; // IPv4/UDP
    size_t ihl = (ip[0] & 0x0f) * 4;
    if (ipLen < ihl + 8) return;
    const uint8_t* udp = ip + ihl;
    uint16_t dport = (udp[2] << 8) | udp[3];
    uint16_t udlen = (udp[4] << 8) | udp[5];
    // DHCP: replies (OFFER/ACK) arrive on UDP port 68 (BOOTP client). Forward
    // the UDP payload (BOOTP message) to the DHCP state machine so DORA can
    // complete. Without this, the client never sees OFFER and DHCP stalls.
    if (dport == 68) {
        if (_onDhcp && udlen >= 8 && (size_t)udlen <= ipLen - ihl)
            _onDhcp(udp + 8, udlen - 8);
        return;
    }
    if (dport != 5600) return;   // video port
    uint16_t ulen = udlen;
    if (ulen < 8 || (size_t)ulen > ipLen - ihl) return;
    const uint8_t* rtp = udp + 8; size_t rtpLen = ulen - 8;
    auto t2 = std::chrono::steady_clock::now();   // after IP decode, before RTP forward
    if (rtpLen && _onRtp) _onRtp(rtp, rtpLen);   // (RTP de-dup happens earlier, ahead of _onIp)
    auto t3 = std::chrono::steady_clock::now();   // after RTP forward (socket send)

    // --- bottleneck diag: timing breakdown every RX_DIAG_INTERVAL packets ---
    {
        using namespace std::chrono;
        _diagPkts++;
        _diagTotalNs += duration_cast<nanoseconds>(t3 - t0).count();
        _diagDecNs   += duration_cast<nanoseconds>(t1 - t0).count();  // CCMP decrypt
        _diagFwdNs   += duration_cast<nanoseconds>(t3 - t2).count();  // RTP sendto()
        if ((_diagPkts % RX_DIAG_INTERVAL) == 0) {
            int64_t avgTotal = _diagTotalNs / _diagPkts;
            int64_t avgDec   = _diagDecNs / _diagPkts;
            int64_t avgIp    = (_diagTotalNs - _diagDecNs - _diagFwdNs) / _diagPkts; // IP decode
            int64_t avgFwd   = _diagFwdNs / _diagPkts;
            __android_log_print(ANDROID_LOG_WARN, "rxd-diag",
                "pkts=%d total=%lldns | CCMP=%lldns IP=%lldns fwd=%lldns",
                _diagPkts, (long long)avgTotal, (long long)avgDec,
                (long long)avgIp, (long long)avgFwd);
            _diagTotalNs = _diagDecNs = _diagFwdNs = _diagPkts = 0;
        }
    }
}

// A-MPDU per-TID reorder buffer (kernel: recv_indicatepkt_reorder).
// Delivers frames in-order within a sliding window of 64 seq numbers.
// QoS data frames from A-MPDU aggregates may arrive out-of-order; without
// this buffer, the RTP stream would see corrupted/unordered NALUs.
// 12-bit sequence space (802.11 QoS), modulo 4096.
bool RxDeframe::processReorder(uint8_t tid, uint16_t seq, const uint8_t* llc, size_t llcLen) {
    if (tid >= 16) return false;
    auto& rc = _reorder[tid];
    uint16_t sq = seq & 0xfff;  // 12-bit sequence number

    // First frame on this TID: initialize window at this seq.
    if (!rc.enable) {
        rc.enable = true;
        rc.indicate_seq = sq;
        rc.pending.clear();
    }

    uint16_t ind = rc.indicate_seq;

    // Already delivered or too old (outside window, below indicate_seq).
    // Sequence comparison with 12-bit wrap: (a - b) mod 4096.
    int16_t sn_diff = (int16_t)(sq - ind);
    if (sn_diff < 0) {
        // Too old — already passed this seq. Drop.
        return false;
    }

    // In-order: deliver immediately.
    if (sn_diff == 0) {
        // Deliver this frame
        if (llcLen >= 8 && _onRtp) {
            const uint8_t* ip = llc + 8;  // skip SNAP header
            size_t ipl = llcLen - 8;
            // Quick IP/UDP/RTP extraction (same as main onPacket path)
            if (ipl >= 20 && (ip[0] >> 4) == 4) {
                size_t ihl = (ip[0] & 0x0f) * 4;
                if (ipl >= ihl + 8 && ip[9] == 17) {  // UDP
                    const uint8_t* u = ip + ihl;
                    uint16_t dport = (u[2] << 8) | u[3];
                    if (dport == 5600) {
                        const uint8_t* rtp = u + 8;
                        size_t rtpLen = ((u[4] << 8) | u[5]) - 8;  // UDP len minus 8B header = RTP len
                        if (rtpLen > 0 && rtpLen <= ipl - ihl - 8)
                            _onRtp(rtp, rtpLen);
                    }
                }
            }
        }

        // Advance window to next expected seq
        rc.indicate_seq = (ind + 1) & 0xfff;

        // Drain pending queue: deliver any consecutive buffered frames
        while (!rc.pending.empty()) {
            auto it = rc.pending.begin();
            uint16_t next_ind = rc.indicate_seq;
            if (it->first == next_ind) {
                // Deliver queued frame (IP/UDP/RTP extraction similar to above)
                auto& buf = it->second;
                if (buf.size() >= 8) {
                    const uint8_t* ip = buf.data() + 8;
                    size_t ipl = buf.size() - 8;
                    if (ipl >= 20 && (ip[0] >> 4) == 4) {
                        size_t ihl = (ip[0] & 0x0f) * 4;
                        if (ipl >= ihl + 8 && ip[9] == 17) {
                            const uint8_t* u = ip + ihl;
                            uint16_t dport = (u[2] << 8) | u[3];
                            if (dport == 5600) {
                                const uint8_t* rtp = u + 8;
                                size_t rtpLen = (u[4] << 8) | u[5];
                                if (rtpLen >= 8 && (size_t)(rtpLen) <= ipl - ihl && _onRtp)
                                    _onRtp(rtp, rtpLen);
                            }
                        }
                    }
                }
                rc.indicate_seq = (next_ind + 1) & 0xfff;
                rc.pending.erase(it);
            } else {
                break;  // gap in pending queue — wait for the missing frame
            }
        }
        return true;
    }

    // Out-of-order but within window: buffer for later delivery.
    if (sn_diff < (int16_t)rc.wsize_b) {
        // Check if already queued (duplicate)
        if (rc.pending.count(sq) > 0) return false;
        rc.pending[sq] = std::vector<uint8_t>(llc, llc + llcLen);
        return false;  // queued, not delivered yet
    }

    // Outside window — too far ahead. Advance window and deliver.
    // (This handles the case where many frames were dropped and we need to skip ahead.)
    rc.indicate_seq = (sq + 1) & 0xfff;
    rc.pending.clear();  // discard stale pending frames
    if (llcLen >= 8 && _onRtp) {
        const uint8_t* ip = llc + 8;
        size_t ipl = llcLen - 8;
        if (ipl >= 20 && (ip[0] >> 4) == 4) {
            size_t ihl = (ip[0] & 0x0f) * 4;
            if (ipl >= ihl + 8 && ip[9] == 17) {
                const uint8_t* u = ip + ihl;
                if (((u[2] << 8) | u[3]) == 5600) {
                    const uint8_t* rtp = u + 8;
                    size_t rtpLen = (u[4] << 8) | u[5];
                    if (rtpLen >= 8 && (size_t)(rtpLen) <= ipl - ihl)
                        _onRtp(rtp, rtpLen);
                }
            }
        }
    }
    return true;
}

} // namespace apfpv
