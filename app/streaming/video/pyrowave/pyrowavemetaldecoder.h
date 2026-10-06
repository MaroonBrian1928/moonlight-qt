#pragma once

#include "pyrowaveframedecoder.h"

#include <memory>
#include <string>

class IPyroWaveMetalTarget;

// Decodes PyroWave frames with the native Metal port into a pool of plane
// textures on a Metal renderer's device (IPyroWaveMetalTarget). Each frame
// carries a PyroWaveMetalFrameRef. Not thread safe: decode() must be called
// from one thread. Frames it produces may be freed from any thread.
class PyroWaveMetalDecoder : public IPyroWaveFrameDecoder
{
public:
    using Config = PyroWaveStreamConfig;

    // Whether the target's device can run the Metal decoder (Apple7 GPU
    // family; Intel and AMD Macs are not supported).
    static bool isSupported(IPyroWaveMetalTarget* target);

    PyroWaveMetalDecoder();
    ~PyroWaveMetalDecoder() override;

    PyroWaveMetalDecoder(const PyroWaveMetalDecoder&) = delete;
    PyroWaveMetalDecoder& operator=(const PyroWaveMetalDecoder&) = delete;

    bool initialize(const Config& config, IPyroWaveMetalTarget* target);

    // The decode is committed to the target's queue, not waited for, unless
    // four earlier decodes are still running on the GPU: the port then waits
    // for the oldest before reusing its upload buffers.
    bool decode(const uint8_t* data, size_t size,
                const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                AVFrame* frame) override;

    const std::string& lastError() const override { return m_LastError; }

    bool lastFramePartial() const override { return m_LastFramePartial; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    std::string m_LastError;
    bool m_LastFramePartial = false;
};
