// Video encoder in the browser. MP4: Mediabunny encodes H.264 with WebCodecs and
// writes the file. GIF: gifenc. Both are loaded from a CDN when an export starts.
// The finished file is handed to the page as a download. JS calls finish later
// (promises), so their state lives in Module.asciiEnc and C++ polls it.
#include "encoder.h"

#include <emscripten.h>

EM_JS_DEPS(encoder, "$stringToUTF8,$lengthBytesUTF8");

EM_JS(void, jsEncPrepare, (int gif), {
    const e = Module.asciiEnc = { error: '', ready: false, done: false, pending: 0 };
    if (!gif && typeof VideoEncoder === 'undefined') {
        e.error = 'This browser cannot encode video (no WebCodecs). Try GIF, or another browser.';
        return;
    }
    const url = gif ? 'https://cdn.jsdelivr.net/npm/gifenc@1.0.3/+esm'
                    : 'https://cdn.jsdelivr.net/npm/mediabunny@1.61.3/+esm';
    import(url).then(lib => { e.lib = lib; e.ready = true; },
                     () => { e.error = 'Could not load the encoder from ' + url + '.'; });
});

// Frames are padded to an even size for H.264 (black edge on the right/bottom)
EM_JS(int, jsEncOpen, (int gif, const char* name, int w, int h, double fps), {
    const e = Module.asciiEnc;
    if (!e || !e.ready) return 0;
    e.name = UTF8ToString(name);
    e.gif = !!gif;
    e.fps = fps;
    e.w = gif ? w : w + (w & 1);
    e.h = gif ? h : h + (h & 1);
    e.buf = new Uint8Array(e.w * e.h * 4);
    e.n = 0;
    const fail = err => { if (!e.error) e.error = 'Encoding failed: ' + (err && err.message || err); };
    e.fail = fail;
    try {
        if (gif) {
            e.out = e.lib.GIFEncoder();
        } else {
            const mb = e.lib;
            e.target = new mb.BufferTarget();
            e.out = new mb.Output({ format: new mb.Mp4OutputFormat({ fastStart: 'in-memory' }), target: e.target });
            e.src = new mb.VideoSampleSource({ codec: 'avc', bitrate: mb.QUALITY_HIGH });
            e.out.addVideoTrack(e.src, { frameRate: fps });
            e.chain = e.out.start().catch(fail);
        }
    } catch (err) {
        fail(err);
        return 0;
    }
    return 1;
});

EM_JS(int, jsEncBusy, (), {
    const e = Module.asciiEnc;
    return e && !e.error && (!e.ready || e.pending > 0) ? 1 : 0;
});

EM_JS(int, jsEncWrite, (const unsigned char* px, int w, int h), {
    const e = Module.asciiEnc;
    if (!e || e.error) return 0;
    // Flip: glReadPixels gives the bottom row first
    const rowBytes = w * 4;
    for (let y = 0; y < h; y++) {
        const src = px + (h - 1 - y) * rowBytes;
        e.buf.set(HEAPU8.subarray(src, src + rowBytes), y * e.w * 4);
    }
    try {
        if (e.gif) {
            const palette = e.lib.quantize(e.buf, 256);
            const index = e.lib.applyPalette(e.buf, palette);
            e.out.writeFrame(index, e.w, e.h, { palette, delay: 1000 / e.fps, repeat: 0 });
        } else {
            // encoderBusy holds the next frame back until this one is added, so e.buf can be reused
            const sample = new e.lib.VideoSample(e.buf, { format: 'RGBX', codedWidth: e.w, codedHeight: e.h,
                timestamp: e.n / e.fps, duration: 1 / e.fps });
            e.pending++;
            e.chain = e.chain.then(() => e.src.add(sample))
                .catch(e.fail)
                .finally(() => { sample.close(); e.pending--; });
        }
    } catch (err) {
        e.fail(err);
        return 0;
    }
    e.n++;
    return 1;
});

EM_JS(void, jsEncClose, (), {
    const e = Module.asciiEnc;
    if (!e || e.error) return;
    const download = (bytes, type) => {
        const a = document.createElement('a');
        a.href = URL.createObjectURL(new Blob([bytes], { type }));
        a.download = e.name;
        a.click();
        setTimeout(() => URL.revokeObjectURL(a.href), 1000);
        e.done = true;
    };
    if (e.gif) {
        e.out.finish();
        download(e.out.bytes(), 'image/gif');
    } else {
        e.chain.then(() => e.out.finalize())
            .then(() => { if (!e.error) download(e.target.buffer, 'video/mp4'); })
            .catch(e.fail);
    }
});

EM_JS(int, jsEncPoll, (), {
    const e = Module.asciiEnc;
    if (!e) return -1;
    return e.error ? -1 : e.done ? 1 : 0;
});

EM_JS(void, jsEncError, (char* out, int size), {
    const e = Module.asciiEnc;
    stringToUTF8(e ? e.error : '', out, size);
});

EM_JS(void, jsEncAbort, (), {
    const e = Module.asciiEnc;
    Module.asciiEnc = null;
    if (!e) return;
    e.error = e.error || 'cancelled'; // stops the chained writes
    if (e.out && e.out.cancel) e.out.cancel().catch(() => {});
});

void encoderPrepare(VideoFormat format)
{
    jsEncPrepare(format == V_GIF);
}

static int g_w = 0, g_h = 0;

bool encoderOpen(VideoFormat format, const std::string& path, int w, int h, const std::string&, double fps)
{
    g_w = w;
    g_h = h;
    return jsEncOpen(format == V_GIF, path.c_str(), w, h, fps);
}

bool encoderBusy()
{
    return jsEncBusy();
}

bool encoderWrite(const std::vector<unsigned char>& rgba)
{
    return jsEncWrite(rgba.data(), g_w, g_h);
}

void encoderClose()
{
    jsEncClose();
}

int encoderPoll()
{
    return jsEncPoll();
}

std::string encoderError()
{
    char text[512];
    jsEncError(text, sizeof text);
    return text;
}

void encoderAbort()
{
    jsEncAbort();
}
