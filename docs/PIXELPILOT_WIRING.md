# PixelPilot fork — wiring APFPV station mode into the app

These are the **exact, minimal** edits to make PixelPilot drive the
station-capable devourer. New files live in this fork under `android/` and
`android/pixelpilot/`; copy them into the PixelPilot tree at the paths shown,
then apply the small VideoActivity edits.

## New files to add
| Fork path | PixelPilot destination |
|-----------|------------------------|
| `android/ApfpvStaLink.java` | `app/wfbngrtl8812/src/main/java/com/openipc/wfbngrtl8812/ApfpvStaLink.java` |
| `android/pixelpilot/ApfpvLinkManager.java` | `app/src/main/java/com/openipc/pixelpilot/ApfpvLinkManager.java` |

## Native side (already in devourer fork)
Add the new STA sources to `app/wfbngrtl8812/src/main/cpp/CMakeLists.txt`
(StationMode, StationTxDesc, Dot11Frames, Wpa2Supplicant, Wpa2Crypto) and
implement the `nativeSta*` JNI bindings declared in `ApfpvStaLink.java`
(mirror the existing `WfbNgLink` JNI .cpp registration).

## VideoActivity.java edits (against the real file)

**1. Field — beside the existing `wfbLink` (line ~113):**
```java
private WfbNgLink wfbLink;            // existing
private ApfpvStaLink staLink;        // ADD
private boolean apfpvMode = false;   // ADD — the mode flag (persist in prefs)
```

**2. Instantiate — beside `wfbLink = new WfbNgLink(this)` (line ~307):**
```java
wfbLink = new WfbNgLink(this);                 // existing
staLink = new ApfpvStaLink(this);              // ADD
```

**3. Manager selection — wherever WfbLinkManager is created/used:**
```java
// BEFORE: always WfbLinkManager(this, binding, wfbLink)
if (apfpvMode) {
    apfpvManager = new ApfpvLinkManager(this, binding, staLink);
    apfpvManager.setChannel(40);               // legal 5.2 GHz default
    apfpvManager.setBandwidth(20);
    apfpvManager.setCredentials(ssidPref, pskPref);
    // USB attach/detach + permission flow is identical; route to apfpvManager
} else {
    wfbLinkManager = new WfbLinkManager(this, binding, wfbLink);  // existing path
}
```

**4. Mode toggle UI** — add a switch in settings (and persist):
```java
// SharedPreferences "link_mode": "wfb" | "apfpv"
// On change: stop current manager, set apfpvMode, re-run adapter start.
```

**5. Credential UI** — add SSID + passphrase text fields in the settings
sheet (only visible when link_mode == apfpv). Defaults: OpenIPC / 12345678.

## What you do NOT touch
- `VideoPlayer` / `videonative` — RTP on port 5600 path is reused verbatim.
- The decoder, OSD render, DVR — unchanged.
- USB permission broadcast receiver — same `ACTION_USB_PERMISSION` flow;
  only the *manager* it hands the device to differs.

## Result
A single toggle switches PixelPilot between:
- **WFB-ng** (existing, monitor mode, dongle) — unchanged.
- **APFPV station** (new) — dongle associates to greg's AP via the forked
  devourer, receives RTP, sends dongle-RSSI LQ feedback to the VTX.

Gated, as always, on the ACK-survival probe passing on hardware.
