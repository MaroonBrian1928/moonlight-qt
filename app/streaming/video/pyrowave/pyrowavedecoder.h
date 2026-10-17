#pragma once

#include "pyrowaveframedecoder.h"
#include "pyrowavesurfaces.h"

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// Decodes PyroWave frames on Vulkan. Windows shares GPU surfaces with the
// renderer. Linux and macOS decode on the VkDevice supplied by the renderer's
// IPyroWaveVulkanPool into planes it lends. macOS exports those MoltenVK images
// to the native Metal presenter. Diagnostics can instead read planes to the CPU.
// Not thread safe: decode() must be called from one thread. Frames it produces
// may be freed from any thread. The default PyroWave decoder on every platform;
// see PyroWaveMetalDecoder for the opt-in native Metal port.
class PyroWaveDecoder : public IPyroWaveFrameDecoder
{
public:
    struct Config {
        int width = 0;
        int height = 0;
        bool chroma444 = false;
        bool tenBit = false;
        // Negotiated with a host that supports independent LZ4 detail groups.
        bool compression = false;
#ifndef _WIN32
        // Optional; without it frames are read back into system memory
        IPyroWaveVulkanPool* vulkanPool = nullptr;
        // Production Metal playback requires GPU sharing. Readback remains
        // available to standalone diagnostics which do not own a presenter.
        bool requireSharedOutput = false;
#endif
    };

    PyroWaveDecoder();
    ~PyroWaveDecoder() override;

    PyroWaveDecoder(const PyroWaveDecoder&) = delete;
    PyroWaveDecoder& operator=(const PyroWaveDecoder&) = delete;

    bool initialize(const Config& config, IPyroWaveSurfacePool* pool);

    // Shared surfaces return after submission; CPU readback returns complete.
    bool hasAsynchronousOutput() const override;

    // Parses and decodes one frame. With shared surfaces the GPU work is only
    // submitted and the renderer orders its reads after it; the readback path
    // waits and returns a planar AVFrame in system memory. Returns false if
    // the frame was dropped.
    // packets maps the frame's RTP packets and which of them were lost (empty
    // for a frame that arrived whole); what survived is decoded when the
    // coarsest wavelet level did, which the host announces as the first
    // criticalPackets packets (0 if it did not). Compressed detail groups
    // are independent; unrepaired detail loss remains a partial frame.
    bool decode(const uint8_t* data, size_t size,
                const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                AVFrame* frame, DecodeDiagnostics* diagnostics = nullptr) override;

    // Why the last call failed, for logging.
    const std::string& lastError() const override { return m_LastError; }

    // Whether the last decoded frame was missing records
    bool lastFramePartial() const override { return m_LastFramePartial; }

    // Framing seen in the most recent successfully parsed frame.
    PyroWaveFraming::Framing lastFraming() const { return m_LastFraming; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    std::string m_LastError;
    PyroWaveFraming::Framing m_LastFraming = PyroWaveFraming::Framing::Records;
    bool m_LastFramePartial = false;
};
