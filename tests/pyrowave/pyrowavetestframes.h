#pragma once

// Synthetic images and host-style PyroWave framing shared by the round trip
// tests. Include after the Vulkan pyrowave.h, whose encoder produces the
// bitstreams these frame.

#include "../../app/streaming/video/pyrowave/pyrowaveframing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace PyroWaveTest {

struct Planes {
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    std::vector<uint8_t> y, cb, cr;

    int chromaWidth() const { return chroma444 ? width : width / 2; }
    int chromaHeight() const { return chroma444 ? height : height / 2; }

    void allocate(int w, int h, bool c444)
    {
        width = w;
        height = h;
        chroma444 = c444;
        y.assign(size_t(w) * h, 0);
        cb.assign(size_t(chromaWidth()) * chromaHeight(), 0);
        cr.assign(size_t(chromaWidth()) * chromaHeight(), 0);
    }

    pyrowave_cpu_buffer buffer()
    {
        pyrowave_cpu_buffer buf = {};
        buf.data[0] = y.data();
        buf.data[1] = cb.data();
        buf.data[2] = cr.data();
        buf.row_stride_in_bytes[0] = size_t(width);
        buf.row_stride_in_bytes[1] = size_t(chromaWidth());
        buf.row_stride_in_bytes[2] = size_t(chromaWidth());
        buf.plane_size_in_bytes[0] = y.size();
        buf.plane_size_in_bytes[1] = cb.size();
        buf.plane_size_in_bytes[2] = cr.size();
        buf.width = width;
        buf.height = height;
        buf.format = chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        return buf;
    }
};

// Smooth gradients plus a few hard edges, varied per frame
inline void fillTestImage(Planes& planes, int frameIndex)
{
    for (int yy = 0; yy < planes.height; yy++) {
        for (int xx = 0; xx < planes.width; xx++) {
            int value = (xx * 3 + yy * 2 + frameIndex * 17) & 0xFF;
            if (((xx / 64) + (yy / 64) + frameIndex) % 7 == 0) {
                value = 235;
            }
            planes.y[size_t(yy) * planes.width + xx] = uint8_t(std::max(16, std::min(235, value)));
        }
    }
    for (int yy = 0; yy < planes.chromaHeight(); yy++) {
        for (int xx = 0; xx < planes.chromaWidth(); xx++) {
            const size_t index = size_t(yy) * planes.chromaWidth() + xx;
            planes.cb[index] = uint8_t(96 + ((xx + frameIndex * 5) % 64));
            planes.cr[index] = uint8_t(96 + ((yy * 2 + frameIndex * 3) % 64));
        }
    }
}

inline double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double sum = 0;
    for (size_t i = 0; i < a.size(); i++) {
        const double diff = double(a[i]) - double(b[i]);
        sum += diff * diff;
    }
    const double mse = sum / double(a.size());
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

inline void putU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; i++) {
        out.push_back(uint8_t(value >> (8 * i)));
    }
}

inline void putPadding(std::vector<uint8_t>& out, size_t bytes)
{
    putU32(out, 0xFFFFFFFFu);
    putU32(out, uint32_t((bytes - 8) / 4));
    out.resize(out.size() + (bytes - 8), 0);
}

// Record framing with the host's layout guarantees but strict record order
// otherwise: the sequence header, the coarsest level, then the rest. In each
// group, records too large to share a payload with a padding record come first
// (a payload less 4 bytes could never be placed alone), then the others padded
// so that none crosses a payload boundary and no 4-byte remainder (too small for
// a padding record) is left. criticalBytes receives the end of the coarsest
// level, whose payloads the host protects with parity. The host packs more
// tightly; the parser must accept both.
inline std::vector<uint8_t> recordFrame(const std::vector<uint8_t>& bitstream,
                                        const std::vector<pyrowave_packet>& packets,
                                        size_t shard, uint32_t coarseBlocks, size_t& criticalBytes)
{
    // Split the encoder's packets into the sequence header and block records
    struct Record {
        size_t offset;
        size_t size;
        bool coarse;
    };
    std::vector<Record> records;
    for (const auto& packet : packets) {
        for (size_t pos = packet.offset; pos < packet.offset + packet.size;) {
            uint32_t word0, word1;
            std::memcpy(&word0, bitstream.data() + pos, 4);
            std::memcpy(&word1, bitstream.data() + pos + 4, 4);
            const bool header = (word0 & 0x80000000u) != 0;
            const size_t size = header ? 8 : size_t((word0 >> 16) & 0xFFF) * 4;
            records.push_back({pos, size, header || (word1 >> 8) < coarseBlocks});
            pos += size;
        }
    }

    std::vector<uint8_t> out;
    // Bytes left in the current payload; the first also carries the frame header
    auto remaining = [&]() { return shard - (out.size() + 8) % shard; };
    auto place = [&](const Record& record) {
        out.insert(out.end(), bitstream.begin() + record.offset,
                   bitstream.begin() + record.offset + record.size);
    };

    place(records.front());
    for (bool coarse : {true, false}) {
        for (size_t i = 1; i < records.size(); i++) {
            if (records[i].coarse == coarse && records[i].size + 8 > shard) {
                if (records[i].size % shard == remaining() - 4) {
                    putPadding(out, 8);
                }
                place(records[i]);
            }
        }
        for (size_t i = 1; i < records.size(); i++) {
            if (records[i].coarse == coarse && records[i].size + 8 <= shard) {
                if (records[i].size > remaining() || remaining() - records[i].size == 4) {
                    putPadding(out, remaining());
                }
                place(records[i]);
            }
        }
        if (coarse) {
            // Padding is only ever placed before a record, so this is the coarse end
            criticalBytes = out.size();
        }
    }
    return out;
}

inline std::vector<uint8_t> lengthPrefixedFrame(const std::vector<uint8_t>& bitstream,
                                                const std::vector<pyrowave_packet>& packets)
{
    std::vector<uint8_t> out;
    putU32(out, uint32_t(packets.size()));
    for (const auto& packet : packets) {
        putU32(out, uint32_t(packet.size));
        out.insert(out.end(), bitstream.begin() + packet.offset,
                   bitstream.begin() + packet.offset + packet.size);
    }
    return out;
}

// Loses one RTP payload of a record-framed frame the way moonlight-common-c
// delivers it: zero-filled and marked lost, with the payloads that start with
// a record flagged as vibeshine does. Returns the frame's payload segments.
inline std::vector<PyroWaveFraming::Segment> losePayload(std::vector<uint8_t>& framed, size_t shard,
                                                         size_t lostPayload)
{
    std::vector<bool> recordStart(framed.size(), false);
    for (size_t pos = 0; pos + 8 <= framed.size();) {
        recordStart[pos] = true;
        uint32_t word0, word1;
        std::memcpy(&word0, framed.data() + pos, 4);
        std::memcpy(&word1, framed.data() + pos + 4, 4);
        pos += word0 == 0xFFFFFFFFu ? 8 + size_t(word1) * 4 :
               (word0 & 0x80000000u) ? 8 : size_t((word0 >> 16) & 0xFFF) * 4;
    }

    std::vector<PyroWaveFraming::Segment> segments;
    for (size_t offset = 0, index = 0; offset < framed.size(); index++) {
        const size_t size = std::min(framed.size() - offset, index == 0 ? shard - 8 : shard);
        segments.push_back({offset, size, index == lostPayload, bool(recordStart[offset])});
        if (index == lostPayload) {
            std::fill(framed.begin() + offset, framed.begin() + offset + size, uint8_t(0));
        }
        offset += size;
    }
    return segments;
}

// Payloads a record-framed frame spans, and how many hold the coarsest level
inline size_t payloadCount(size_t framedBytes, size_t shard)
{
    return (framedBytes + 8 + shard - 1) / shard;
}

}
