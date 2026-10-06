// Encodes synthetic frames with the vendored PyroWave encoder, frames them the
// way hosts do (record framing aligned to RTP payloads, and the length-prefixed
// compatibility framing), and decodes them through the client's framing parser.
// Needs a Vulkan GPU; exits 0 with a notice when none is present.

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include "pyrowavetestframes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_Failures = 0;

void expect(bool condition, const std::string& description)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
        ++g_Failures;
    }
}

using namespace PyroWaveTest;

bool decodeFramed(pyrowave_decoder decoder, const std::vector<uint8_t>& framed,
                  const PyroWaveFraming::StreamGeometry& geometry,
                  PyroWaveFraming::Framing expectedFraming, Planes& output, const std::string& name)
{
    PyroWaveFraming::Frame frame;
    std::string error;
    if (!PyroWaveFraming::parse(framed.data(), framed.size(), geometry, frame, error)) {
        expect(false, name + ": parse failed: " + error);
        return false;
    }
    expect(frame.framing == expectedFraming, name + ": framing detected");

    pyrowave_decoder_clear(decoder);
    for (const auto& span : frame.spans) {
        if (pyrowave_decoder_push_packet(decoder, framed.data() + span.offset, span.size) != PYROWAVE_SUCCESS) {
            expect(false, name + ": push_packet rejected a span");
            return false;
        }
    }
    if (!pyrowave_decoder_decode_is_ready(decoder, false)) {
        expect(false, name + ": frame not complete after pushing every span");
        return false;
    }

    auto buffer = output.buffer();
    if (pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) != PYROWAVE_SUCCESS) {
        expect(false, name + ": decode failed");
        return false;
    }
    return true;
}

// Loses one RTP payload of a record-framed frame the way moonlight-common-c
// delivers it (zero-filled, marked lost), with the payloads that start with a
// record flagged and the critical payloads announced as vibeshine does, and
// decodes what the parser salvages. Returns whether the decoder accepted the
// partial frame; its luma PSNR goes to quality.
bool decodeWithLostPayload(pyrowave_decoder decoder, std::vector<uint8_t> framed, size_t shard,
                           size_t lostPayload, size_t criticalPayloads,
                           const PyroWaveFraming::StreamGeometry& geometry,
                           const Planes& source, Planes& output, double& quality, const std::string& name)
{
    const auto segments = losePayload(framed, shard, lostPayload);

    PyroWaveFraming::Frame frame;
    std::string error;
    if (!PyroWaveFraming::parse(framed.data(), framed.size(), segments, criticalPayloads, geometry, frame, error)) {
        expect(false, name + ": parse failed: " + error);
        return false;
    }
    expect(frame.partial, name + ": the frame is partial");
    expect(frame.blockRecords < frame.announcedBlocks, name + ": records were lost");

    pyrowave_decoder_clear(decoder);
    for (const auto& span : frame.spans) {
        if (pyrowave_decoder_push_packet(decoder, framed.data() + span.offset, span.size) != PYROWAVE_SUCCESS) {
            expect(false, name + ": push_packet rejected a span");
            return false;
        }
    }
    // As PyroWaveDecoder does: the parser vouches for the coarsest level
    if (!frame.coarseLevelIntact ||
            !pyrowave_decoder_decode_is_ready_with_sideband(decoder, true, 0, 0.9f, nullptr, 0)) {
        return false;
    }

    auto buffer = output.buffer();
    if (pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) != PYROWAVE_SUCCESS) {
        expect(false, name + ": decode failed");
        return false;
    }
    quality = psnr(source.y, output.y);
    return true;
}

void runCase(pyrowave_device device, int width, int height, bool chroma444, size_t budget)
{
    const std::string name = std::to_string(width) + "x" + std::to_string(height) +
                             (chroma444 ? " 4:4:4" : " 4:2:0");
    const PyroWaveFraming::StreamGeometry geometry {width, height, chroma444};

    pyrowave_encoder_create_info encoderInfo = {};
    encoderInfo.device = device;
    encoderInfo.width = width;
    encoderInfo.height = height;
    encoderInfo.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_encoder encoder = nullptr;
    if (pyrowave_encoder_create(&encoderInfo, &encoder) != PYROWAVE_SUCCESS) {
        expect(false, name + ": encoder creation");
        return;
    }

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = device;
    decoderInfo.width = width;
    decoderInfo.height = height;
    decoderInfo.chroma = encoderInfo.chroma;
    pyrowave_decoder decoder = nullptr;
    if (pyrowave_decoder_create(&decoderInfo, &decoder) != PYROWAVE_SUCCESS) {
        expect(false, name + ": decoder creation");
        pyrowave_encoder_destroy(encoder);
        return;
    }

    Planes source, decoded;
    source.allocate(width, height, chroma444);
    decoded.allocate(width, height, chroma444);

    const size_t shard = 1392 - 16;
    size_t recordBytes = 0, paddingBytes = 0;

    // Frames 1-4 are encoded but never decoded: the decoder must still accept
    // frame 5 even though its 3-bit sequence counter skipped ahead.
    for (int frameIndex = 0; frameIndex < 6; frameIndex++) {
        fillTestImage(source, frameIndex);
        auto input = source.buffer();
        pyrowave_rate_control rate = { budget };
        if (pyrowave_encoder_encode_cpu_synchronous(encoder, &input, &rate) != PYROWAVE_SUCCESS) {
            expect(false, name + ": encode");
            break;
        }
        if (frameIndex != 0 && frameIndex != 5) {
            continue;
        }

        size_t packetCount = 0;
        pyrowave_encoder_compute_num_packets_with_padding(encoder, shard, 8, &packetCount);
        std::vector<pyrowave_packet> packets(packetCount);
        std::vector<uint8_t> bitstream(budget + 1024 * 1024);
        size_t written = 0;
        if (pyrowave_encoder_packetize_with_padding(encoder, packets.data(), shard, 8, &written,
                                                    bitstream.data(), bitstream.size()) != PYROWAVE_SUCCESS) {
            expect(false, name + ": packetize");
            break;
        }
        packets.resize(written);

        size_t criticalBytes = 0;
        const auto records = recordFrame(bitstream, packets, shard,
                                         PyroWaveFraming::coarseBlockCount(geometry), criticalBytes);
        recordBytes += records.size();
        if (decodeFramed(decoder, records, geometry, PyroWaveFraming::Framing::Records, decoded, name + " records")) {
            const double quality = psnr(source.y, decoded.y);
            expect(quality > 30.0, name + ": record-framed luma PSNR " + std::to_string(quality));
            PyroWaveFraming::Frame frame;
            std::string error;
            PyroWaveFraming::parse(records.data(), records.size(), geometry, frame, error);
            paddingBytes += frame.paddingBytes;

            // Lose one payload at a time across the frame. The payloads holding the
            // coarsest level carry parity (moonlight-common-c recovers them), so only
            // the others can reach the parser with a hole; each only blurs its area.
            const size_t payloads = (records.size() + 8 + shard - 1) / shard;
            const size_t criticalPayloads = (criticalBytes + 8 + shard - 1) / shard;
            int salvaged = 0, tried = 0;
            double worstQuality = 99;
            for (size_t lost = criticalPayloads; lost < payloads;
                 lost += std::max<size_t>(1, (payloads - criticalPayloads) / 24)) {
                const std::string lossName = name + " lost payload " + std::to_string(lost) +
                                             " of " + std::to_string(payloads);
                double lossyQuality = 0;
                tried++;
                if (decodeWithLostPayload(decoder, records, shard, lost, criticalPayloads, geometry, source,
                                          decoded, lossyQuality, lossName)) {
                    salvaged++;
                    worstQuality = std::min(worstQuality, lossyQuality);
                    // Losing the next-coarsest level right after the critical payloads
                    // blurs a large area for the frame (about 22 dB at 4:4:4 here)
                    expect(lossyQuality > 20.0 && lossyQuality <= quality + 0.01,
                           lossName + ": luma PSNR " + std::to_string(lossyQuality) +
                           " against " + std::to_string(quality) + " whole");
                }
            }
            std::printf("%s frame %d: %zu of %zu payloads critical (%.1f%% of the frame); %d of %d "
                        "single-payload losses elsewhere decoded, luma PSNR %.1f dB whole, %.1f dB worst salvaged\n",
                        name.c_str(), frameIndex, criticalPayloads, payloads,
                        100.0 * double(criticalBytes) / double(records.size()), salvaged, tried, quality, worstQuality);
            expect(salvaged == tried, name + ": every loss outside the critical payloads decodes");
        }

        // Re-packetize with the compatibility boundary for length-prefixed framing
        pyrowave_encoder_compute_num_packets(encoder, 1024, &packetCount);
        packets.resize(packetCount);
        pyrowave_encoder_packetize(encoder, packets.data(), 1024, &written, bitstream.data(), bitstream.size());
        packets.resize(written);
        const auto prefixed = lengthPrefixedFrame(bitstream, packets);
        if (decodeFramed(decoder, prefixed, geometry, PyroWaveFraming::Framing::LengthPrefixed, decoded,
                         name + " length-prefixed")) {
            const double quality = psnr(source.y, decoded.y);
            expect(quality > 30.0, name + ": length-prefixed luma PSNR " + std::to_string(quality));
        }
    }

    std::printf("%s: record framing %zu bytes, %.1f%% padding (strict order)\n",
                name.c_str(), recordBytes, recordBytes ? 100.0 * double(paddingBytes) / double(recordBytes) : 0.0);

    pyrowave_decoder_destroy(decoder);
    pyrowave_encoder_destroy(encoder);
}

}

int main()
{
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) {
        std::printf("No Vulkan device for PyroWave; skipping the round trip\n");
        return 0;
    }

    // Budgets around 1.6 bits per pixel
    runCase(device, 1280, 720, false, 180 * 1024);
    runCase(device, 1920, 1080, false, 400 * 1024);
    runCase(device, 1920, 1080, true, 650 * 1024);

    pyrowave_device_destroy(device);

    if (g_Failures == 0) {
        std::printf("PyroWave round trip: all checks passed\n");
    }
    return g_Failures ? 1 : 0;
}
