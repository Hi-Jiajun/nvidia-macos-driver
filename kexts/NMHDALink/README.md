# NMHDALink

Publishes the display↔codec association that `AppleHDAController` reads from the framebuffer. Without it,
AppleHDA can see the codec and even start `AppleHDAHDMI_DPDriver`, yet never create
`AppleHDAEngineOutputDP`, so no display audio device appears.

Per external display it publishes on the `IOFramebuffer`:

| Property | Value |
| --- | --- |
| `connector-type` | `0x00000800` HDMI, `0x00000400` DisplayPort (from the EDID's HDMI VSDB) |
| `port-number` | `0` |
| `av-signal-type` | `8` while a sink whose CEA block has an audio block is online, `0` otherwise |
| `audio-codec-info` | `0x00040100` (codec 0, byte 1 = 1, pin widget 4) |
| `IODisplayEDID` | the sink's full EDID |

The EDID is read through `getDDCBlock`; an internal panel (`kIOConnectionBuiltIn`) is skipped. Connect and
disconnect events re-evaluate the set.

## Build

```sh
bash build.sh <MacKernelSDK checkout> <out dir>
```

The result is `<out dir>/NMHDALink.kext`; there is no Lilu dependency.

## Install

Copy `NMHDALink.kext` to `EFI/OC/Kexts` and add it to `Kernel -> Add` (any position after Lilu). It requires
`NMHDA.kext` as well when the card's HDA function id is not in `AppleHDAController`'s built-in table.

## Verify

```sh
ioreg -w0 -n NVRMFramebuffer -r -d 0 | grep -E 'audio-codec-info|av-signal-type|connector-type|port-number'
ioreg -w0 -c AppleHDAEngineOutputDP | grep -c AppleHDAEngineOutputDP   # >= 1
system_profiler SPAudioDataType | grep -A4 'Transport: HDMI'
```
