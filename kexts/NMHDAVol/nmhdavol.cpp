// NMHDAVol: software volume for AppleHDA's display-audio engine.
//
// AppleHDAHDMI_DPDriver creates AppleHDAEngineOutputDP with a selector but no IOAudioLevelControl, so CoreAudio
// greys the volume slider out. The engine does not implement clipOutputSamples either (the symbol only exists in
// IOAudioFamily), so the base conversion is the one place where every output sample passes through.
//
// This plugin routes IOAudioEngine::clipOutputSamples, scales only the frames that belong to an
// AppleHDAEngineOutputDP, and attaches a level control to that engine so CoreAudio shows a slider. The control's
// value is a percentage in debug.nmhdavol.

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/audio/IOAudioControl.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioTypes.h>
#include <IOKit/audio/IOAudioDefines.h>
#include <mach/kmod.h>
#include <sys/sysctl.h>
#include <stdarg.h>

static char gReport[512];
SYSCTL_STRING(_debug, OID_AUTO, nmhdavol, CTLFLAG_RD | CTLFLAG_KERN, gReport, sizeof(gReport), "NMHDAVol");
static void report(const char *fmt, ...) {
    char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    SYSLOG("nmhdavol", "%s", line);
    size_t used = strlen(gReport), need = strlen(line) + 2;
    if (used + need > sizeof(gReport)) { gReport[0] = 0; used = 0; }
    snprintf(gReport + used, sizeof(gReport) - used, "%s%s", used ? " | " : "", line);
}

static volatile int gVolume = 75;          // percent
static volatile UInt32 gClips, gScaled;
static volatile UInt32 gFmtBits, gFmtFlags, gFmtCh;
static IOAudioLevelControl *gControl;

static IOReturn volumeChanged(OSObject *, IOAudioControl *, SInt32, SInt32 newValue) {
    gVolume = newValue;
    report("volume %d", (int)newValue);
    return kIOReturnSuccess;
}

static bool isDisplayEngine(IOAudioEngine *engine) {
    const char *cn = engine && engine->getMetaClass() ? engine->getMetaClass()->getClassName() : nullptr;
    return cn && strstr(cn, "AppleHDAEngineOutputDP");
}

// The format lives in IOAudioStreamFormat::fFlags: the leading character of the sample-format constant ('l'pcm,
// 'i'eee) in the top byte, the numeric representation below it.
static void scaleFrames(void *sampleBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames,
                        const IOAudioStreamFormat *fmt, int volume) {
    if (!sampleBuf || !fmt || volume >= 100) return;
    UInt32 ch = fmt->fNumChannels, bits = fmt->fBitDepth;
    if (!ch || !bits) return;
    UInt32 frameBytes = ch * (bits / 8);
    UInt8 *p = (UInt8 *)sampleBuf + (UInt64)firstSampleFrame * frameBytes;
    UInt32 n = numSampleFrames * ch;
    float g = (float)volume / 100.0f;

    UInt32 kind = fmt->fSampleFormat;
    if (kind == kIOAudioStreamSampleFormatIEEEFloat) {
        if (bits == 32) { float *s = (float *)p; for (UInt32 i = 0; i < n; i++) s[i] *= g; }
    } else if (kind == kIOAudioStreamSampleFormatLinearPCM) {
        if (bits == 16) { int16_t *s = (int16_t *)p; for (UInt32 i = 0; i < n; i++) s[i] = (int16_t)(s[i] * g); }
        else if (bits == 32) { int32_t *s = (int32_t *)p; for (UInt32 i = 0; i < n; i++) s[i] = (int32_t)(s[i] * g); }
    }
    gScaled++;
}

static const char *kAudioPath[] {
    "/System/Library/Extensions/IOAudioFamily.kext/Contents/MacOS/IOAudioFamily"
};
static KernelPatcher::KextInfo kAudioFamily { "com.apple.iokit.IOAudioFamily", kAudioPath, arrsize(kAudioPath),
    {}, {}, KernelPatcher::KextInfo::Unloaded };
static const char *kHdaPath[] {
    "/System/Library/Extensions/AppleHDA.kext/Contents/MacOS/AppleHDA"
};
static KernelPatcher::KextInfo kAppleHDA { "com.apple.driver.AppleHDA", kHdaPath, arrsize(kHdaPath),
    {}, {}, KernelPatcher::KextInfo::Unloaded };
// Lilu keeps this array (it writes loadIndex into it), so it must outlive pluginStart.
static KernelPatcher::KextInfo kPatchedKexts[] { kAudioFamily, kAppleHDA };

static mach_vm_address_t orgClip;
static IOReturn clipOutputSamples(IOAudioEngine *self, const void *mixBuf, void *sampleBuf, UInt32 firstSampleFrame,
                                  UInt32 numSampleFrames, const IOAudioStreamFormat *streamFormat,
                                  IOAudioStream *audioStream) {
    auto r = FunctionCast(clipOutputSamples, orgClip)(self, mixBuf, sampleBuf, firstSampleFrame, numSampleFrames,
                                                      streamFormat, audioStream);
    if (r == kIOReturnSuccess && isDisplayEngine(self)) {
        gClips++;
        if (streamFormat && gFmtFlags == 0) {
            gFmtBits = streamFormat->fBitDepth; gFmtFlags = streamFormat->fSampleFormat;
            gFmtCh = streamFormat->fNumChannels;
            report("format bits %u flags 0x%08x ch %u", (unsigned)gFmtBits, (unsigned)gFmtFlags, (unsigned)gFmtCh);
        }
        scaleFrames(sampleBuf, firstSampleFrame, numSampleFrames, streamFormat, gVolume);
    }
    return r;
}

static void attachVolumeControl(IOAudioEngine *engine) {
    if (!engine || !isDisplayEngine(engine) || gControl) return;
    OSObject *alloc = OSMetaClass::allocClassWithName("IOAudioLevelControl");
    IOAudioLevelControl *c = alloc ? static_cast<IOAudioLevelControl *>(alloc) : nullptr;
    if (!c || !c->init(gVolume, 0, 100, 0, 0, kIOAudioControlChannelIDAll, "Display", 0, 0,
                       kIOAudioControlUsageOutput)) {
        if (c) c->release();
        report("level control allocation failed");
        return;
    }
    c->setValueChangeHandler(volumeChanged, nullptr);
    if (engine->addDefaultAudioControl(c) == kIOReturnSuccess) {
        gControl = c;
        report("level control attached to %s", engine->getName());
    } else {
        report("addDefaultAudioControl refused");
        c->release();
    }
}

// The engine publishes its controls to the HAL before coreaudiod opens it; a control added afterwards never
// reaches CoreAudio. IOAudioEngine::start is a base-class symbol that exists in the kernel collection, while
// AppleHDA's own methods have no symbols at all, so the control is attached here.
static mach_vm_address_t orgEngineStart;
static bool engineStart(IOAudioEngine *self, IOService *provider) {
    bool r = FunctionCast(engineStart, orgEngineStart)(self, provider);
    if (r) attachVolumeControl(self);
    return r;
}

static bool onEngineMatched(void *, void *, IOService *service, IONotifier *) {
    // IOAudioFamily lives in SystemKernelExtensions, which OpenCore does not link against while injecting into the
    // prelinked kernel — so this plugin may not reference its symbols. The class is cast by name and allocated
    // through OSMetaClass (kernel core); every call below is a virtual dispatch.
    OSMetaClassBase *mcb = service ? service->metaCast("IOAudioEngine") : nullptr;
    IOService *svc = mcb ? static_cast<IOService *>(mcb) : nullptr;
    attachVolumeControl(svc ? static_cast<IOAudioEngine *>(svc) : nullptr);
    return true;
}

static void onKextLoaded(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t, size_t);

static void pluginStart() {
    sysctl_register_oid(&sysctl__debug_nmhdavol);
    OSDictionary *m = IOService::serviceMatching("AppleHDAEngineOutputDP");
    if (m) {
        IOService::addMatchingNotification(gIOMatchedNotification, m, onEngineMatched, nullptr, nullptr, 0);
        m->release();
    }
    lilu.onKextLoadForce(kPatchedKexts, arrsize(kPatchedKexts), onKextLoaded);
    report("waiting for AppleHDA");
}

// One callback for both kexts: whichever of the two symbols lives in the kext under this index routes here, and
// the other lookup simply fails. Comparing loadIndex is not reliable across Lilu versions, so it is not done.
static void onKextLoaded(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t, size_t) {
    KernelPatcher::RouteRequest clip {
        "__ZN13IOAudioEngine17clipOutputSamplesEPKvPvjjPK20_IOAudioStreamFormatP13IOAudioStream",
        clipOutputSamples, orgClip };
    if (patcher.routeMultiple(index, &clip, 1)) report("clipOutputSamples routed (kext %lu)", (unsigned long)index);
    patcher.clearError();
    KernelPatcher::RouteRequest start {
        "__ZN13IOAudioEngine5startEP9IOService", engineStart, orgEngineStart };
    if (patcher.routeMultiple(index, &start, 1)) report("IOAudioEngine::start routed (kext %lu)", (unsigned long)index);
    patcher.clearError();
}

static const char *bootargOff[] { "-nmhdavoloff" };
static const char *bootargDebug[] { "-nmhdavoldbg" };
static const char *bootargBeta[] { "-nmhdavolbeta" };

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff, arrsize(bootargOff),
    bootargDebug, arrsize(bootargDebug),
    bootargBeta, arrsize(bootargBeta),
    KernelVersion::Sequoia, KernelVersion::Sequoia,
    pluginStart
};

extern "C" kern_return_t NMHDAVol_kern_start(kmod_info_t *, void *);
extern "C" kern_return_t NMHDAVol_kern_stop(kmod_info_t *, void *);
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMHDAVol, "1.0.0", NMHDAVol_kern_start, NMHDAVol_kern_stop) }
