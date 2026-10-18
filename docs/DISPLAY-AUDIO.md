# Display audio (HDMI/DisplayPort)

macOS plays sound through the monitor over the same cable as the picture, but the audio side is a separate PCI function
on the card and a separate driver stack in macOS. On an NVIDIA card two of those pieces were missing, and this change
supplies the second one. With both, the sink appears under Sound → Output and plays.

## The four layers

| Layer | What it must do |
| --- | --- |
| `NVRM`/NVKMS (GPU kernel driver) | enable the audio stream on the active head and write the sink's ELD into the GPU's HDA codec |
| `NVRMFB` (framebuffer) | publish the display-codec association `AppleHDAController` reads, and the sink's EDID |
| `AppleHDA` | `AppleHDAController` binds the card's HDA function, `AppleHDACodecGeneric` parses the codec, `AppleHDAHDMI_DPDriver` creates `AppleHDAEngineOutputDP` |
| `NMHDA` (`kexts/NMHDA`) | rewrite an `AppleHDAController` device-table entry so the card's HDA function binds at all |

`NVRM` and `NMHDA` already did their part. The framebuffer published `nvda-eld` and `hda-gfx`, and still no
`AppleHDAEngineOutputDP` was created: the controller had no display to associate with its codec.

## The fix

`NVRMFB` now also publishes the properties `AppleHDAController` reads before it will associate a display with the
codec (`publishDisplayAudioLink` in `kexts/NVRMFB/fb/nvrm-fb.cpp`, called from the framebuffer's start and from the
display-swap path):

| Property | Value |
| --- | --- |
| `connector-type` | `0x00000800` HDMI, `0x00000400` DisplayPort, decided by the EDID's HDMI VSDB |
| `port-number` | `0` |
| `av-signal-type` | `8` when the sink's CEA block lists at least one short audio descriptor, `0` otherwise |
| `audio-codec-info` | `0x00040100`: codec 0, byte 1 = 1, pin widget 4 |
| `IODisplayEDID` | the sink's EDID, as `getDDCBlock` hands it to macOS |

All five are four-byte data properties, little-endian, the way Apple's own framebuffers publish them.

`audio-codec-info` is decisive. Without it, `AppleHDAController::findValidDisplayCodec()` looks its own PCI id up in a
second table that has no NVIDIA entries, never tells `AppleHDAHDMI_DPDriver` about the display, and no engine is
created. `av-signal-type` is what routes audio: the controller dispatches on it and `8` is the external-digital audio
path, while an internal eDP panel's `0x10` falls through to the default. `connector-type` and `port-number` pick the
display head the audio belongs to.

A sink with no audio descriptors, an internal panel and an EDID that could not be read publish no association; the
panel and the unreadable EDID also clear `av-signal-type` so nothing stale routes audio to a head that cannot play it.
The other properties are left as they were, the same way `nvda-eld` is kept when a display goes away.

## One scan decides both

The ELD and these properties answer the same two questions about the same bytes: does the sink list audio formats, and
is it an HDMI sink? `nvrm_cea_scan()` in `kexts/NVRMFB/nvrm_eld.h` answers them once, and `nvrm_build_eld()` and
`publishDisplayAudioLink()` both read its result. SADs in the ELD therefore imply `av-signal-type` 8, and an empty ELD
implies 0; the two cannot drift apart, and the bounds checks on a truncated or malformed EDID live in one place.

`tools/test_display_audio_link.py` lifts `publishDisplayAudioLink()` out of the source and runs it against a mock
registry entry that records every property written: HDMI sink, native DisplayPort sink, silent sink, a malformed audio
block, audio that only appears in the second extension block, internal panel, unreadable EDID and an absent extension
block. Three one-line regressions of that function — always HDMI, always audio, and dropping the internal check — must
fail the same suite. `tools/test_eld.py` covers the shared scan and the ELD it feeds.

## Evidence

Before: `AppleHDAEngineOutputDP = 0` and no HDMI/DP device in `system_profiler SPAudioDataType`, with the HDA function
bound, the codec enumerated and `AppleHDAHDMI_DPDriver` started.

After: `AppleHDAEngineOutputDP` is created, the sink appears under Sound → Output, `audio-selector` is written back on
the framebuffer, and audio plays. Booted on the test machine with the out-of-tree kext that had published the same
values on the `IOFramebuffer` removed, so the properties below are NVRMFB's own:

```
$ ioreg -w0 -n NVRMFramebuffer -r -d 0 | grep -E 'audio-codec-info|av-signal-type|connector-type|port-number'
  |   "av-signal-type" = <08000000>
  |   "audio-codec-info" = <00010400>
  |   "connector-type" = <00040000>
  |   "port-number" = <00000000>
$ ioreg -w0 -c AppleHDAEngineOutputDP | grep -c AppleHDAEngineOutputDP
3
$ system_profiler SPAudioDataType | grep -A3 'Transport: HDMI'
          Transport: HDMI
          Output Source: Mi Monitor
```

The table is the DisplayPort reading: `0x00000400` because that sink's EDID carries no HDMI VSDB. Unplugging it and
plugging the same monitor into the card's HDMI port republished the association as `0x00000800` without a restart,
with `av-signal-type` still 8 and the engine still playing. The out-of-tree kext also showed that the properties
alone are enough, with no NVKMS change behind them.

## Verify

```sh
python3 tools/test_display_audio_link.py     # the property values, and the three negative controls
python3 tools/test_eld.py                    # the shared CEA scan and the ELD

ioreg -w0 -n NVRMFramebuffer -r -d 0 | grep -E 'audio-codec-info|av-signal-type|connector-type|port-number'
ioreg -w0 -c AppleHDAEngineOutputDP | grep -c AppleHDAEngineOutputDP   # >= 1
system_profiler SPAudioDataType | grep -A4 'Transport: HDMI'
```

## Known limits

- `audio-codec-info` hardcodes pin widget 4, which is what NVIDIA GPU codecs use. A codec with a different pin NID
  needs that byte changed.
- Every display head publishes `port-number` 0 and pin widget 4, which is right for the one sink this was tested with.
  Two audio-capable monitors on the same card have not been tried; the controller may associate both with the same
  converter and create one engine.
- The system volume slider stays greyed out on display outputs. Apple's driver creates the engine without an
  `IOAudioLevelControl`; real Macs behave the same and the volume belongs to the sink. Adding a control from a kext
  panics `AppleHDAController` while it activates the engine, so the display's own volume is the way to change it.
- Tested on one card, an RTX 5060 under macOS 15.8.1, over both DisplayPort and HDMI, including switching the monitor
  between the two ports without a restart. macOS 26 is untested.
- Sample formats are whatever the sink advertises in its SADs; nothing here negotiates or converts them.
