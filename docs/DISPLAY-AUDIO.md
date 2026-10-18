# Display audio (HDMI/DisplayPort)

Status: the audio path works on the RTX 5060 test machine (macOS 15.8.1, DP and HDMI): the display
appears under Sound → Output, plays, and hot-plug/port changes keep working. The system volume slider is
still disabled (see NMHDAVol below).

## Why NVIDIA display audio needs three pieces

macOS splits the job between three drivers:

| Layer | What it must do |
| --- | --- |
| GPU kernel driver (`NVRM`/NVKMS) | enable the audio stream on the active head and write the ELD into the GPU's HDA codec |
| Framebuffer (`NVRMFB`) | publish the display↔codec association the audio controller reads: `hda-gfx`, `connector-type`, `port-number`, `av-signal-type`, `audio-codec-info`, `IODisplayEDID` |
| AppleHDA | `AppleHDAController` binds the card's HDA function, `AppleHDACodecGeneric` parses the codec and `AppleHDAHDMI_DPDriver` creates `AppleHDAEngineOutputDP` |

Two of them were missing on newer cards:

1. `AppleHDAController` only probes HDA functions listed in its built-in 48-byte device table (Fermi and
   Kepler ids only), so `10DE:22EB` got no driver at all. **NMHDA** (`kexts/NMHDA`) rewrites a Kepler
   table entry to the card's id and re-matches the device.
2. Even with the controller bound, AppleHDA creates no engine until the framebuffer publishes the
   display↔codec association above. **NMHDALink** (`kexts/NMHDALink`) publishes it.

## NMHDALink

A small kext that has no Lilu dependency and touches no hardware. On the `IOFramebuffer` it reads the
sink EDID with `getDDCBlock` and publishes:

```
connector-type    0x800 (HDMI) / 0x400 (DP), decided by the EDID's HDMI VSDB
port-number       0
av-signal-type    8 while a sink with a CEA audio block is online, 0 otherwise
audio-codec-info  0x00040100 (codec 0, byte 1 = 1, pin widget 4)
IODisplayEDID     the full EDID
```

It re-evaluates on display connect and clears `av-signal-type` on disconnect.

`audio-codec-info` is the decisive one: without it `AppleHDAController::findValidDisplayCodec()` looks
the PCI id up in a second table that has no NVIDIA entries and never tells `AppleHDAHDMI_DPDriver` about
the display.

The same properties belong in `nvrm-fb.cpp`'s display-audio block; this kext exists because NVRMFB has no
build path in this tree.

## NMHDAVol (experimental)

AppleHDA's display engine has a selector but no `IOAudioLevelControl`, so CoreAudio greys out the volume
slider (real Macs behave the same). NMHDAVol routes `IOAudioEngine::clipOutputSamples`, scales only the
frames of an `AppleHDAEngineOutputDP`, and attaches a level control to that engine.

Two constraints shaped it:

- `IOAudioFamily` lives in `SystemKernelExtensions`, which OpenCore does not link against while injecting
  into the prelinked kernel, so the plugin must not reference IOAudioFamily symbols. It gets the engine
  with `metaCast("IOAudioEngine")` and allocates the control with `OSMetaClass::allocClassWithName`, and
  every call after that is a virtual dispatch.
- The volume is a percentage in `sysctl debug.nmhdavol`.

## Install

Both kexts go into `EFI/OC/Kexts` and are listed in `Kernel -> Add` (NMHDAVol after Lilu):

```
NMHDA.kext       Enabled=true    (table patch; already part of this repository)
NMHDALink.kext   Enabled=true
NMHDAVol.kext    Enabled=true    (only if you want the experimental volume patch)
```

## Verify

```sh
ioreg -w0 -n NVRMFramebuffer -r -d 0 | grep -E 'audio-codec-info|av-signal-type|connector-type|port-number'
ioreg -w0 -c AppleHDAEngineOutputDP | grep -c AppleHDAEngineOutputDP   # >= 1
system_profiler SPAudioDataType | grep -A4 'Transport: HDMI'
```

## Known limits

- The system volume slider stays greyed out without NMHDAVol; Apple's design leaves display volume to
  the sink.
- `audio-codec-info` hardcodes pin widget 4, which is what NVIDIA GPU codecs use; a card with a different
  pin NID needs that byte changed (take it from a Linux `codec dump`).
