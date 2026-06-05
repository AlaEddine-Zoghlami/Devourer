#pragma once
#include <cstdint>
namespace apfpv {
class LqFeedback {
public:
    enum class Mode { FixedTimer, FrameDriven };
    struct Config { Mode mode = Mode::FixedTimer; int send_interval_ms = 33;
        int min_interval_ms = 20; int keepalive_ms = 0; double smoothing_alpha = 0.3; };
    static int rssiPct(int dbm);
    LqFeedback() ; explicit LqFeedback(Config cfg);
    ~LqFeedback() { stop(); }   // join thread + close socket on destroy (reconnect-safe)
    bool start(const char* airIp = "192.168.0.1", uint16_t port = 12345);
    void stop();
    void update(int rssiA_dbm, int rssiB_dbm = INT32_MIN);
};
}
