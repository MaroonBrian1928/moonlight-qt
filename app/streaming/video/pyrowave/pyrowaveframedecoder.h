#pragma once

#include "pyrowaveframing.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// The negotiated stream a native Metal PyroWave decoder is created for.
// PyroWaveDecoder (Vulkan) has its own Config with the renderer's surfaces.
struct PyroWaveStreamConfig {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    bool tenBit = false;
};

// CPU wall times, not GPU execution times. Submission can wait for older work.
struct PyroWaveDecodeDiagnostics {
    uint64_t phaseUs[5] = {}; // parse, push/validate, acquire, submit, release
    uint64_t contextWaitUs = 0; // portion of submit inside Granite::next_frame_context
    uint64_t payloadBytes = 0;
    uint32_t receivedBlocks = 0;
    uint32_t announcedBlocks = 0;
    uint32_t paddingBytes = 0;
    bool partial = false;
};

// What FFmpegVideoDecoder needs from a PyroWave decoder, whichever GPU API it
// decodes with (PyroWaveDecoder: Vulkan, the default everywhere;
// PyroWaveMetalDecoder: the opt-in native Metal port on Apple Silicon).
class IPyroWaveFrameDecoder
{
public:
    using DecodeDiagnostics = PyroWaveDecodeDiagnostics;

    virtual ~IPyroWaveFrameDecoder() = default;

    // Whether decode() returns once the GPU work is submitted rather than
    // complete (shared surfaces), so the renderer must order its reads after it.
    virtual bool hasAsynchronousOutput() const = 0;

    // Parses and decodes one frame. On success, frame receives the decoded
    // output and its format/size metadata. Returns false if the frame was
    // dropped. packets maps the frame's RTP packets and which of them were
    // lost (empty for a frame that arrived whole); what survived is decoded
    // when the coarsest wavelet level did, which the host announces as the
    // first criticalPackets packets (0 if it did not). diagnostics, when not
    // null, receives per-phase CPU timings.
    virtual bool decode(const uint8_t* data, size_t size,
                        const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                        AVFrame* frame, DecodeDiagnostics* diagnostics = nullptr) = 0;

    // Why the last call failed, for logging.
    virtual const std::string& lastError() const = 0;

    // Whether the last decoded frame was missing records
    virtual bool lastFramePartial() const = 0;
};
