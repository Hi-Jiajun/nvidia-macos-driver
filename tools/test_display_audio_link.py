#!/usr/bin/env python3
"""The properties AppleHDA reads off the framebuffer must follow the sink's EDID.

AppleHDAController will not associate a display with the GPU's codec without audio-codec-info, and it routes audio
only for av-signal-type 8, so the values NVRMFB publishes here decide whether HDMI/DisplayPort audio exists at all.
publishDisplayAudioLink() is lifted out of nvrm-fb.cpp and compiled against a mock registry entry that records every
property written. The cases cover an HDMI sink, a native DisplayPort sink (no HDMI VSDB), a silent sink, an internal
panel and an unreadable EDID, and audio that only appears in the second extension block.

Negative controls run the same suite against three one-line regressions of that region -- always claiming HDMI,
always claiming audio, and forgetting the internal panel -- and require each to fail."""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'kexts' / 'NVRMFB' / 'fb' / 'nvrm-fb.cpp').read_text()

start = source.index('static void publishDisplayAudioLink(')
region = source[start:source.index('\n}\n', start) + 3]
assert 'nvrm_cea_scan(edid, n, &cea)' in region, \
    'the display-audio link no longer reads the sink through nvrm_cea_scan(): it can now disagree with the ELD'
assert '"audio-codec-info"' in region and '"av-signal-type"' in region

fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

template <typename... T> static void fbLog(const char *, T &&...) {}
#define FBLOG(...) fbLog(__VA_ARGS__)

struct OSData {
    std::vector<uint8_t> bytes;
    static OSData *withBytes(const void *p, unsigned n)
    {
        OSData *d = new OSData();
        d->bytes.assign((const uint8_t *)p, (const uint8_t *)p + n);
        return d;
    }
    void release() { delete this; }
};

struct IORegistryEntry {
    std::map<std::string, std::vector<uint8_t>> data;
    bool setProperty(const char *k, const void *v, unsigned n)
    {
        data[k].assign((const uint8_t *)v, (const uint8_t *)v + n);
        return true;
    }
    bool setProperty(const char *k, OSData *d) { data[k] = d->bytes; return true; }
    bool setProperty(const char *k, const char *) { data[k] = std::vector<uint8_t>(1, 1); return true; }
    bool has(const char *k) const { return data.find(k) != data.end(); }
    // A 4-byte property is published little-endian: <00 08 00 00> is 0x800.
    unsigned u32(const char *k) const
    {
        const std::vector<uint8_t> &v = data.at(k);
        assert(v.size() == 4);
        return v[0] | (v[1] << 8) | (v[2] << 16) | ((unsigned)v[3] << 24);
    }
    std::vector<uint8_t> bytes(const char *k) const { return data.at(k); }
};

#include "nvrm_eld.h"

REGION

static void base_block(uint8_t *e, size_t blocks, uint8_t ext_count)
{
    memset(e, 0, 128 * blocks);
    e[0] = 0x00; for (int i = 1; i <= 6; i++) e[i] = 0xFF; e[7] = 0x00;
    e[8] = 0x10; e[9] = 0xAC; e[10] = 0x34; e[11] = 0x12;   // manufacturer / product
    e[126] = ext_count;
}

// A CEA-861 rev-3 extension with the requested number of SADs and, optionally, the HDMI Licensing VSDB.
static void cea_block(uint8_t *c, int sads, bool hdmi_vsdb)
{
    memset(c, 0, 128);
    c[0] = 0x02; c[1] = 0x03;
    size_t p = 4;
    if (sads > 0) {
        c[p++] = (uint8_t)((NVRM_CEA_TAG_AUDIO << 5) | (size_t)(3 * sads));
        for (int i = 0; i < sads; i++) { c[p++] = 0x09; c[p++] = 0x07; c[p++] = 0x07; }   // LPCM 2ch
    }
    if (hdmi_vsdb) {
        c[p++] = (uint8_t)((NVRM_CEA_TAG_VENDOR << 5) | 5);
        c[p++] = 0x03; c[p++] = 0x0C; c[p++] = 0x00; c[p++] = 0x10; c[p++] = 0x00;
    }
    c[2] = (uint8_t)p;
}

static void expect_sink(IORegistryEntry &fb, unsigned connector, unsigned signal, const uint8_t *edid, size_t n)
{
    assert(fb.u32("connector-type") == connector);
    assert(fb.u32("port-number") == 0);
    assert(fb.u32("audio-codec-info") == 0x00040100u);      // codec 0, byte 1 = 1, pin widget 4
    assert(fb.u32("av-signal-type") == signal);
    assert(fb.bytes("IODisplayEDID") == std::vector<uint8_t>(edid, edid + n));
}

static void expect_no_sink(IORegistryEntry &fb)
{
    assert(fb.u32("av-signal-type") == 0);
    assert(!fb.has("audio-codec-info") && !fb.has("connector-type") && !fb.has("IODisplayEDID"));
}

int main()
{
    // 1. An HDMI sink that announces audio: HDMI connector, av-signal-type 8, the EDID itself published.
    {
        uint8_t edid[256]; base_block(edid, 2, 1); cea_block(edid + 128, 2, true);
        IORegistryEntry fb;
        publishDisplayAudioLink(&fb, edid, sizeof edid, false);
        expect_sink(fb, 0x00000800u, 8, edid, sizeof edid);
        std::printf("case: HDMI sink with audio\n");
    }

    // 2. A native DisplayPort sink: audio but no HDMI VSDB, so connector-type must stay DisplayPort.
    //    Negative control "always HDMI" fails here.
    {
        uint8_t edid[256]; base_block(edid, 2, 1); cea_block(edid + 128, 1, false);
        IORegistryEntry fb;
        publishDisplayAudioLink(&fb, edid, sizeof edid, false);
        expect_sink(fb, 0x00000400u, 8, edid, sizeof edid);
        std::printf("case: DisplayPort sink with audio\n");
    }

    // 3. An HDMI link to a sink that lists no formats (a DVI monitor through an adapter, a monitor with audio off):
    //    the association is published -- the controller may still route video -- but av-signal-type stays 0, because
    //    no AppleHDAEngineOutputDP can play anything. Negative control "always audio" fails here.
    {
        uint8_t edid[256]; base_block(edid, 2, 1); cea_block(edid + 128, 0, true);
        IORegistryEntry fb;
        publishDisplayAudioLink(&fb, edid, sizeof edid, false);
        expect_sink(fb, 0x00000800u, 0, edid, sizeof edid);
        std::printf("case: silent HDMI sink\n");
    }

    // 4. An audio data block too short to hold one SAD is not a format the sink can play, so av-signal-type stays 0
    //    and agrees with the ELD, which finds no SAD there either.
    {
        uint8_t edid[256]; base_block(edid, 2, 1); cea_block(edid + 128, 0, true);
        uint8_t *c = edid + 128; c[4] = (uint8_t)((NVRM_CEA_TAG_AUDIO << 5) | 2); c[5] = 0x09; c[6] = 0x07;
        c[2] = 7;                                            // data blocks end after the short audio block
        IORegistryEntry fb;
        publishDisplayAudioLink(&fb, edid, sizeof edid, false);
        assert(fb.u32("av-signal-type") == 0);
        std::printf("case: malformed audio block\n");
    }

    // 5. Audio in the second extension block only: the scan has to follow every block the EDID declares.
    {
        uint8_t edid[384]; base_block(edid, 3, 2);
        cea_block(edid + 128, 0, false);                      // no audio, no VSDB
        cea_block(edid + 256, 2, true);                       // audio + HDMI
        IORegistryEntry fb;
        publishDisplayAudioLink(&fb, edid, sizeof edid, false);
        expect_sink(fb, 0x00000800u, 8, edid, sizeof edid);
        std::printf("case: audio in the second extension block\n");
    }

    // 6. An internal panel gets no association at all. Negative control "internal ignored" fails here: it would
    //    publish a DisplayPort association for the eDP panel and let the controller tie the panel to the codec.
    {
        uint8_t edid[256]; base_block(edid, 2, 1); cea_block(edid + 128, 2, true);
        IORegistryEntry fb;
        publishDisplayAudioLink(&fb, edid, sizeof edid, true);
        expect_no_sink(fb);
        std::printf("case: internal panel\n");
    }

    // 7. An EDID that could not be read (or is shorter than one block) must not describe a sink either.
    {
        uint8_t short_edid[64]; memset(short_edid, 0, sizeof short_edid);
        IORegistryEntry a, b;
        publishDisplayAudioLink(&a, short_edid, sizeof short_edid, false);
        publishDisplayAudioLink(&b, nullptr, 0, false);
        expect_no_sink(a);
        expect_no_sink(b);
        std::printf("case: unreadable EDID\n");
    }

    // 8. An EDID whose single extension is not in the buffer still publishes the association, with no audio.
    {
        uint8_t edid[128]; base_block(edid, 1, 1);            // claims one extension, buffer holds only the base
        IORegistryEntry fb;
        publishDisplayAudioLink(&fb, edid, sizeof edid, false);
        expect_sink(fb, 0x00000400u, 0, edid, sizeof edid);
        std::printf("case: absent extension block\n");
    }

    // 9. A null framebuffer is not a crash (the call is made from two start paths).
    publishDisplayAudioLink(nullptr, nullptr, 0, false);

    std::puts("PASS: connector-type follows the HDMI VSDB, av-signal-type follows the sink's SADs, internal and "
              "unreadable displays get no association");
    return 0;
}
'''


def run(code, expect_ok):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        (path / 'test.cpp').write_text(code)
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-I', str(root / 'kexts' / 'NVRMFB'),
                        str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
        rc = subprocess.run([str(path / 'test')]).returncode
    if expect_ok:
        assert rc == 0, 'publishDisplayAudioLink must publish the properties these cases expect'
    else:
        assert rc != 0, 'the regression must FAIL the suite (negative control for the whole suite)'


real = fixture.replace('REGION', region)
run(real, expect_ok=True)

controls = {
    'connector-type always HDMI': ('cea.is_hdmi ? 0x08 : 0x04', '0x08'),
    'av-signal-type always 8': ('cea.has_audio ? 0x08 : 0x00', '0x08'),
    'internal panel association': ('if (internal || !edid || n < 128)', 'if (!edid || n < 128)'),
}
for name, (old, new) in controls.items():
    assert old in region, 'could not build the negative control for %s' % name
    run(real.replace(old, new), expect_ok=False)

print('PASS: display-audio link publishes the association from the sink\'s EDID; '
      + ', '.join(controls) + ' each fail the suite')
