// NMHDALink: publish the display-audio link that AppleHDAController reads from the framebuffer.
//
// AppleHDA only creates AppleHDAEngineOutputDP when the controller can associate a display with a display codec.
// On Apple's own GPUs that association is published by the framebuffer driver: connector-type, port-number,
// av-signal-type (8 = external digital audio while a sink with audio is online) and audio-codec-info (codec
// address, 1, pin widget NID). NVRMFB publishes hda-gfx and its own nvda-eld but none of those four, which is
// why the codec and AppleHDAHDMI_DPDriver come up while no engine is ever created.
//
// This kext publishes them on the IOFramebuffer, reading the sink's EDID through getDDCBlock. It is a companion
// to NVRMFB rather than a patch of it (the display driver has no build path in this tree); the same properties
// belong in nvrm-fb.cpp's display-audio block once NVRMFB is rebuilt.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>

class NMHDALink : public IOService {
    OSDeclareDefaultStructors(NMHDALink)
public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(NMHDALink, IOService)

static IORegistryEntry *framebufferAbove(IOService *svc) {
    IORegistryEntry *e = svc->getParentEntry(gIOServicePlane);
    while (e && !e->metaCast("IOFramebuffer")) e = e->getParentEntry(gIOServicePlane);
    return e;
}

// NVRMFB reports kIOConnectionBuiltIn for a laptop panel; an eDP/LVDS display has no audio sink to describe.
static bool isInternalPanel(IOService *svc) {
    IOFramebuffer *fb = OSDynamicCast(IOFramebuffer, svc);
    if (!fb) return false;
    uintptr_t flags = 0;
    if (fb->getAttributeForConnection(0, kConnectionFlags, &flags) != kIOReturnSuccess) return false;
    return (flags & kIOConnectionBuiltIn) != 0;
}

// Read the whole EDID (base block + CEA extensions) the same way IODisplayConnect does.
static UInt32 readEdid(IOService *svc, UInt8 *out, UInt32 max) {
    IOFramebuffer *fb = OSDynamicCast(IOFramebuffer, svc);
    if (!fb) return 0;
    IOByteCount len = 0;
    if (fb->getDDCBlock(0, 0, kIODDCBlockTypeEDID, 0, out, &len) != kIOReturnSuccess || len < 128) return 0;
    UInt32 total = 128, ext = out[126];
    for (UInt32 i = 1; i <= ext && total + 128 <= max; i++) {
        len = 0;
        if (fb->getDDCBlock(0, i, kIODDCBlockTypeEDID, 0, out + total, &len) != kIOReturnSuccess || len < 128) break;
        total += 128;
    }
    return total;
}

// CEA-861: is there an audio data block, and is this an HDMI sink (HDMI Licensing VSDB OUI 00-0C-03)?
static void edidFacts(const UInt8 *e, UInt32 n, bool *hasAudio, bool *isHdmi) {
    *hasAudio = false;
    *isHdmi = false;
    if (n < 128) return;
    UInt32 exts = e[126];
    for (UInt32 x = 1; x <= exts; x++) {
        UInt32 base = x * 128;
        if (base + 128 > n) break;
        const UInt8 *cea = e + base;
        if (cea[0] != 0x02 || cea[1] < 3) continue;
        UInt32 dtd = cea[2], p = 4;
        if (dtd < 4 || dtd > 128) continue;
        while (p < dtd) {
            UInt32 tag = cea[p] >> 5, len = cea[p] & 0x1F;
            if (p + 1 + len > dtd) break;
            const UInt8 *body = cea + p + 1;
            if (tag == 1 && len >= 3) *hasAudio = true;
            if (tag == 3 && len >= 3 && body[0] == 0x03 && body[1] == 0x0C && body[2] == 0x00) *isHdmi = true;
            p += 1 + len;
        }
    }
}

static void publish(IORegistryEntry *fb, const UInt8 *edid, UInt32 edidLen, bool online) {
    if (!fb) return;
    bool hasAudio = false, isHdmi = false;
    if (edid && edidLen >= 128) edidFacts(edid, edidLen, &hasAudio, &isHdmi);

    UInt8 conn[4] = {0x00, (UInt8)(isHdmi ? 0x08 : 0x04), 0x00, 0x00};   // 0x800 HDMI / 0x400 DP
    fb->setProperty("connector-type", conn, sizeof conn);
    UInt8 port[4] = {0, 0, 0, 0};
    fb->setProperty("port-number", port, sizeof port);
    UInt8 codec[4] = {0x00, 0x01, 0x04, 0x00};                          // codec 0, 1, pin widget 4
    fb->setProperty("audio-codec-info", codec, sizeof codec);
    UInt8 sig[4] = {(UInt8)((online && hasAudio) ? 0x08 : 0x00), 0, 0, 0};
    fb->setProperty("av-signal-type", sig, sizeof sig);
    if (edid && edidLen >= 128)
        if (OSData *d = OSData::withBytes(edid, edidLen)) { fb->setProperty("IODisplayEDID", d); d->release(); }
}

static bool onFramebuffer(void *, void *, IOService *svc, IONotifier *) {
    if (isInternalPanel(svc)) {
        UInt8 zero[4] = {0, 0, 0, 0};
        svc->setProperty("av-signal-type", zero, sizeof zero);
        IOLog("nmhdalink: %s is an internal panel, no display audio published\n", svc->getName());
        return true;
    }
    UInt8 edid[512];
    UInt32 n = readEdid(svc, edid, sizeof edid);
    publish(svc, n ? edid : nullptr, n, n != 0);
    IOLog("nmhdalink: %s edid %u B\n", svc->getName(), (unsigned)n);
    return true;
}

static bool onDisplay(void *, void *, IOService *svc, IONotifier *) {
    if (!svc) return true;
    OSData *edid = OSDynamicCast(OSData, svc->getProperty("IODisplayEDID"));
    IORegistryEntry *fb = framebufferAbove(svc);
    if (!fb) return true;
    if (isInternalPanel(OSDynamicCast(IOService, fb))) {
        UInt8 zero[4] = {0, 0, 0, 0};
        fb->setProperty("av-signal-type", zero, sizeof zero);
        return true;
    }
    if (edid && edid->getLength() >= 128)
        publish(fb, (const UInt8 *)edid->getBytesNoCopy(), edid->getLength(), true);
    else
        publish(fb, nullptr, 0, true);     // keep the statics, let av-signal-type follow the EDID we have
    return true;
}

static bool onDisplayGone(void *, void *, IOService *svc, IONotifier *) {
    if (!svc) return true;
    IORegistryEntry *fb = framebufferAbove(svc);
    if (fb) { UInt8 zero[4] = {0, 0, 0, 0}; fb->setProperty("av-signal-type", zero, sizeof zero); }
    return true;
}

bool NMHDALink::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    OSDictionary *fbMatch = IOService::serviceMatching("IOFramebuffer");
    OSDictionary *dpyMatch = IOService::serviceMatching("AppleDisplay");
    if (fbMatch) IOService::addMatchingNotification(gIOMatchedNotification, fbMatch, onFramebuffer, this, nullptr, 0);
    if (dpyMatch) {
        IOService::addMatchingNotification(gIOMatchedNotification, dpyMatch, onDisplay, this, nullptr, 0);
        IOService::addMatchingNotification(gIOTerminatedNotification, dpyMatch, onDisplayGone, this, nullptr, 0);
    }
    IOLog("nmhdalink: started\n");
    return true;
}

extern "C" kern_return_t NMHDALink_kern_start(kmod_info_t *, void *) { return KERN_SUCCESS; }
extern "C" kern_return_t NMHDALink_kern_stop(kmod_info_t *, void *) { return KERN_SUCCESS; }
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMHDALink, "1.0.0", NMHDALink_kern_start, NMHDALink_kern_stop) }
