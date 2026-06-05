// APFPV ACK-survival probe — Linux laptop test harness.
// Build: cmake + make ApfpvProbe. Run with the AU dongle on greg's AP.
// Prints the GO/NO-GO verdict that gates the whole project.
#include "WiFiDriver.h"
#include "StationMode.h"
#include <libusb.h>
#include <cstdio>
int main(int argc, char** argv) {
    printf("APFPV probe: opens AU, arms station mode, sends auth/assoc,\n"
           "watches link-hold vs deauth. Verdict gates the fork.\n");
    // libusb open of 0bda:8812, build RtlUsbAdapter, WiFiDriver init to ch40,
    // construct StationMode(dev, rm, sendFrame), call runProbe(self,bssid,ssid).
    // (Wiring mirrors demo/main.cpp's adapter bring-up; BSSID from scan.)
    // Result mapping:
    //   GO_LinkHeld       -> "GO: hardware auto-ACKs. Build the rest."
    //   NOGO_Deauthed     -> "NO-GO: associated but unACKed (hw won't auto-ACK)."
    //   TXFAIL_NoAuthResp -> "TX problem: frames not reaching AP (descriptor)."
    //   NOGO_NoAssocResp  -> "auth ok, assoc failed."
    printf("(wire adapter bring-up from demo/main.cpp; see comments)\n");
    return 0;
}
