#pragma once

// Contract between the native Metal PyroWave decoder and a Metal renderer. The
// decoder allocates its plane textures on the renderer's MTLDevice and commits
// each decode to the renderer's command queue. One queue runs its command
// buffers in commit order and Metal tracks hazards on the (tracked) plane
// textures, so a render committed after a decode samples finished planes, and
// a decode into planes a committed render still reads waits for that read.
// Neither side signals events or waits for the other's frames. (The Metal port
// itself waits for its oldest decode when four are still running on the GPU,
// which only back-pressures a decoder far ahead of the GPU.)

#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
}

class IPyroWaveMetalTarget {
public:
    virtual ~IPyroWaveMetalTarget() = default;

    // id<MTLDevice> and id<MTLCommandQueue>, valid until the renderer is
    // destroyed. Plain pointers so this header stays free of Objective-C.
    virtual void* pyroWaveMetalDevice() = 0;
    virtual void* pyroWaveMetalCommandQueue() = 0;
};

// Owned by each Metal PyroWave AVFrame through frame->buf[0]. The planes stay
// valid while the frame holds this reference, even after the decoder is gone.
// Freeing the last reference returns the surface to the decoder; a renderer
// must commit its reads before then.
struct PyroWaveMetalFrameRef {
    static constexpr uint32_t k_Magic = 0x5059524D; // "PYRM"

    uint32_t magic = k_Magic;
    int surface = -1;
    // id<MTLTexture> for Y, Cb and Cr. 10-bit streams use R16Unorm planes
    // holding full-range 16-bit samples, not 10 bits in the low bits.
    void* planes[3] = {};

    static PyroWaveMetalFrameRef* fromFrame(const AVFrame* frame)
    {
        if (frame == nullptr || frame->buf[0] == nullptr ||
                frame->buf[0]->size != sizeof(PyroWaveMetalFrameRef)) {
            return nullptr;
        }
        auto ref = reinterpret_cast<PyroWaveMetalFrameRef*>(frame->buf[0]->data);
        return ref->magic == k_Magic ? ref : nullptr;
    }
};
