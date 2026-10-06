#pragma once

#include "pyrowaveframedecoder.h"
#include "pyrowavesurfaces.h"

class IPyroWaveVulkanSurfaces;

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

// Decodes PyroWave frames into surfaces owned by the renderer: on a private
// Vulkan device into shared OS handles (IPyroWaveSurfacePool), or on the
// renderer's own Vulkan device (IPyroWaveVulkanSurfaces). Without either, it
// decodes into system memory as 8-bit YUV420P/YUV444P frames that any renderer
// can upload. Not thread safe: decode() must be called from one thread. Frames it
// produces may be freed from any thread.
class PyroWaveDecoder : public IPyroWaveFrameDecoder
{
public:
    using Config = PyroWaveStreamConfig;

    PyroWaveDecoder();
    ~PyroWaveDecoder() override;

    PyroWaveDecoder(const PyroWaveDecoder&) = delete;
    PyroWaveDecoder& operator=(const PyroWaveDecoder&) = delete;

    // Pass at most one of pool and vulkanSurfaces. With neither, output goes to
    // system memory, which waits for each decode on the CPU and is 8-bit only.
    bool initialize(const Config& config, IPyroWaveSurfacePool* pool,
                    IPyroWaveVulkanSurfaces* vulkanSurfaces = nullptr);

    // With a surface pool, the GPU work is only submitted: the frame's
    // PyroWaveFrameRef carries the decode fence value to wait for.
    bool decode(const uint8_t* data, size_t size,
                const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                AVFrame* frame) override;

    const std::string& lastError() const override { return m_LastError; }

    bool lastFramePartial() const override { return m_LastFramePartial; }

    // Framing seen in the most recent successfully parsed frame.
    PyroWaveFraming::Framing lastFraming() const { return m_LastFraming; }

private:
    bool decodeToMemory(AVFrame* frame);
    bool decodeToSurface(AVFrame* frame);

    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    std::string m_LastError;
    PyroWaveFraming::Framing m_LastFraming = PyroWaveFraming::Framing::Records;
    bool m_LastFramePartial = false;
};
