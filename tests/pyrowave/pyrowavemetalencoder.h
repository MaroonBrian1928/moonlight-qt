#pragma once

// The vendored Metal encoder behind an interface free of PyroWave types, so a
// test can use it beside the Vulkan pyrowave.h (the two APIs share names).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace PyroWaveTest {

struct EncodedPacket {
    size_t offset;
    size_t size;
};

class MetalEncoder {
public:
    MetalEncoder();
    ~MetalEncoder();

    bool create(int width, int height, bool chroma444);

    // 8-bit planes, tightly packed
    bool encode(const uint8_t* y, const uint8_t* cb, const uint8_t* cr, size_t budget);

    // Packetizes the last encode. With padding (as hosts do for record framing),
    // packets break at boundary less the padding size.
    bool packetize(size_t boundary, size_t padding, std::vector<uint8_t>& bitstream,
                   std::vector<EncodedPacket>& packets);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

}
