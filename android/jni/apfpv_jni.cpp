// ============================================================================
//  apfpv_jni.cpp — JNI bindings: ApfpvStaLink.java  <->  native ApfpvStation
//  This is the REAL connective tissue: PixelPilot's Java calls these; they
//  drive the forked devourer station stack. Mirrors the existing WfbNgLink JNI
//  registration pattern (libusb fd via wrap_sys_device -> RtlUsbAdapter).
//  Goes in PixelPilot app/wfbngrtl8812/src/main/cpp/ alongside the wfb JNI.
// ============================================================================
#include <jni.h>
#include <libusb.h>
#include <memory>
#include <string>
#include <android/log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>

#include "ApfpvStation.h"
#include "WiFiDriver.h"
#include "RtlUsbAdapter.h"
#include "RtlJaguarDevice.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "apfpv-jni", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "apfpv-jni", __VA_ARGS__)

using namespace apfpv;

// One station per app instance (APFPV uses a single link).
struct StaCtx {
    libusb_context*               usb = nullptr;
    libusb_device_handle*         handle = nullptr;
    std::unique_ptr<WiFiDriver>   driver;
    std::unique_ptr<RtlJaguarDevice> rtl;   // owns adapter + radio manager
    std::unique_ptr<ApfpvStation> station;
    JavaVM*                       jvm = nullptr;
    jobject                       jlink = nullptr;   // global ref to ApfpvStaLink
    int                           rtpSock = -1;     // UDP -> 127.0.0.1:5600
    sockaddr_in                   rtpDst{};
};

// Forward state/rssi to Java (onNativeState / onNativeRssi callbacks).
static void postState(StaCtx* ctx, ApfpvStation::State s) {
    if (!ctx->jvm || !ctx->jlink) return;
    JNIEnv* env = nullptr;
    if (ctx->jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jclass cls = env->GetObjectClass(ctx->jlink);
    jmethodID m = env->GetMethodID(cls, "onNativeState", "(I)V");
    if (m) env->CallVoidMethod(ctx->jlink, m, (jint)s);
    ctx->jvm->DetachCurrentThread();
}

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_openipc_wfbngrtl8812_ApfpvStaLink_nativeStaInitialize(JNIEnv* env, jclass, jobject /*context*/) {
    auto* ctx = new StaCtx();
    env->GetJavaVM(&ctx->jvm);
    libusb_init(&ctx->usb);
    LOGI("ApfpvStation ctx initialized");
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT void JNICALL
Java_com_openipc_wfbngrtl8812_ApfpvStaLink_nativeStaConnect(
        JNIEnv* env, jclass, jlong inst, jobject jlink, jint fd,
        jint channel, jint bandwidth, jstring jssid, jstring jpass) {
    auto* ctx = reinterpret_cast<StaCtx*>(inst);
    if (!ctx) return;
    ctx->jlink = env->NewGlobalRef(jlink);

    // Unrooted USB-host path (same as the wfb monitor path): adopt the fd.
    libusb_set_option(ctx->usb, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, nullptr);
    if (libusb_wrap_sys_device(ctx->usb, (intptr_t)fd, &ctx->handle) < 0) {
        LOGE("libusb_wrap_sys_device failed"); postState(ctx, ApfpvStation::State::FailNoAp); return;
    }

    const char* ssid = env->GetStringUTFChars(jssid, nullptr);
    const char* pass = env->GetStringUTFChars(jpass, nullptr);

    // RTP delivery socket: native RX hands recovered RTP here; we sendto
    // 127.0.0.1:5600 so PixelPilot's existing VideoPlayer (binds INADDR_ANY:5600)
    // renders it with ZERO decoder changes — same as the wfb path's output.
    if (ctx->rtpSock < 0) {
        ctx->rtpSock = ::socket(AF_INET, SOCK_DGRAM, 0);
        std::memset(&ctx->rtpDst, 0, sizeof(ctx->rtpDst));
        ctx->rtpDst.sin_family = AF_INET;
        ctx->rtpDst.sin_port = htons(5600);
        ::inet_pton(AF_INET, "127.0.0.1", &ctx->rtpDst.sin_addr);
    }
    StaCtx* c = ctx;   // capture for the lambda
    auto onRtp = [c](const uint8_t* rtp, size_t len) {
        if (c->rtpSock >= 0 && rtp && len)
            ::sendto(c->rtpSock, rtp, len, 0, (sockaddr*)&c->rtpDst, sizeof(c->rtpDst));
    };
    auto onState = [ctx](ApfpvStation::State s){ postState(ctx, s); };

    // Build the real driver objects from the adopted libusb handle. WiFiDriver
    // owns the RtlUsbAdapter + RadioManagementModule; ApfpvStation drives them.
    // NOTE: WiFiDriver's public accessors for adapter/radio must be used here —
    // passing nullptr (as the earlier scaffold did) would crash. The exact
    // accessor names depend on the devourer build; wire them when compiling:
    //   ctx->station = std::make_unique<ApfpvStation>(
    //       ctx->driver->adapter(), ctx->driver->radioManager(), onRtp, onState);
    // Real devourer factory path: WiFiDriver.CreateRtlDevice(handle) builds the
    // RtlJaguarDevice (which owns the RtlUsbAdapter + RadioManagementModule).
    ctx->driver = std::make_unique<WiFiDriver>(nullptr /*Logger_t*/);
    ctx->rtl = ctx->driver->CreateRtlDevice(ctx->handle);
    ctx->station = std::make_unique<ApfpvStation>(
        &ctx->rtl->adapter(), &ctx->rtl->radioManager(), onRtp, onState);
    ctx->station->setDevice(ctx->rtl.get());   // RX path: device -> RxDeframe

    ApfpvStation::Params p;
    p.channel = channel; p.bandwidth = bandwidth;
    p.ssid = ssid; p.passphrase = pass; p.lqFeedback = true;
    ctx->station->connect(p);     // runs the gated chain; states -> Java

    env->ReleaseStringUTFChars(jssid, ssid);
    env->ReleaseStringUTFChars(jpass, pass);
}

JNIEXPORT void JNICALL
Java_com_openipc_wfbngrtl8812_ApfpvStaLink_nativeStaDisconnect(JNIEnv*, jclass, jlong inst, jint /*fd*/) {
    auto* ctx = reinterpret_cast<StaCtx*>(inst);
    if (ctx && ctx->station) ctx->station->disconnect();
}

JNIEXPORT jint JNICALL
Java_com_openipc_wfbngrtl8812_ApfpvStaLink_nativeStaGetState(JNIEnv*, jclass, jlong inst) {
    auto* ctx = reinterpret_cast<StaCtx*>(inst);
    return (ctx && ctx->station) ? (jint)ctx->station->state() : 0;
}

JNIEXPORT jint JNICALL
Java_com_openipc_wfbngrtl8812_ApfpvStaLink_nativeStaGetRssi(JNIEnv*, jclass, jlong inst) {
    auto* ctx = reinterpret_cast<StaCtx*>(inst);
    return (ctx && ctx->station) ? (jint)ctx->station->rssiDbm() : -99;
}

JNIEXPORT void JNICALL
Java_com_openipc_wfbngrtl8812_ApfpvStaLink_nativeStaSetLqFeedback(JNIEnv*, jclass, jlong /*inst*/, jboolean /*en*/) {
    // toggles ApfpvStation's LqFeedback; stored in Params at connect for now.
}

JNIEXPORT void JNICALL
Java_com_openipc_wfbngrtl8812_ApfpvStaLink_nativeStaSetTxPower(JNIEnv*, jclass, jlong inst, jint power) {
    // GS dongle TX power index (0-63) for the station-mode dongle. Same chip and
    // same scale as the WFB-ng path; routes to the RtlJaguarDevice's SetTxPower.
    // (Phone-Wi-Fi mode never reaches here — it has no dongle and no StaCtx.)
    auto* ctx = reinterpret_cast<StaCtx*>(inst);
    if (ctx && ctx->rtl) {
        ctx->rtl->SetTxPower(static_cast<uint8_t>(power));
    }
}

JNIEXPORT jint JNICALL
Java_com_openipc_wfbngrtl8812_ApfpvStaLink_nativeStaGetTxPower(JNIEnv*, jclass, jlong inst) {
    // Reads the dongle's REAL reference TX power index (0-63) from the driver:
    // EFUSE-calibrated value when the dongle is connected, else the fallback.
    // Returns -1 if no device yet (caller should keep its own default then).
    auto* ctx = reinterpret_cast<StaCtx*>(inst);
    if (ctx && ctx->rtl) {
        return static_cast<jint>(ctx->rtl->GetTxPower());
    }
    return -1;
}

} // extern "C"
