#pragma once

#include "pyrowaveframing.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// The negotiated stream a PyroWave decoder is created for
struct PyroWaveStreamConfig {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    bool tenBit = false;
};

// What FFmpegVideoDecoder needs from a PyroWave decoder, whichever GPU API it
// decodes with (PyroWaveDecoder: Vulkan; PyroWaveMetalDecoder: native Metal).
class IPyroWaveFrameDecoder
{
public:
    virtual ~IPyroWaveFrameDecoder() = default;

    // Parses and decodes one frame. On success, frame receives the decoded
    // output and its format/size metadata; the GPU work may still be in flight.
    // Returns false if the frame was dropped. packets maps the frame's RTP
    // packets and which of them were lost (empty for a frame that arrived
    // whole); what survived is decoded when the coarsest wavelet level did,
    // which the host announces as the first criticalPackets packets (0 if it
    // did not).
    virtual bool decode(const uint8_t* data, size_t size,
                        const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                        AVFrame* frame) = 0;

    // Why the last call failed, for logging.
    virtual const std::string& lastError() const = 0;

    // Whether the last decoded frame was missing records
    virtual bool lastFramePartial() const = 0;
};
