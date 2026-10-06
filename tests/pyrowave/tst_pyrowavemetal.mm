// Decodes host-framed PyroWave frames with the client's native Metal decoder
// (PyroWaveMetalDecoder) and checks its planes against the source image and,
// when a Vulkan device is present (MoltenVK through GRANITE_VULKAN_LIBRARY),
// against the Vulkan decoder the client otherwise uses, fed the same bytes:
// whole frames, length-prefixed framing, single lost payloads, 10-bit output
// scale, and reuse of a surface a queued read still samples. The frames come
// from the vendored Metal encoder, since PyroWave's Vulkan encoder does not run
// on MoltenVK. Needs an Apple7+ GPU; exits 0 with a notice otherwise.

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include "pyrowavetestframes.h"
#include "pyrowavemetalencoder.h"
#include "../../app/streaming/video/pyrowave/pyrowavemetaldecoder.h"
#include "../../app/streaming/video/pyrowave/pyrowavemetaltarget.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

class TestTarget : public IPyroWaveMetalTarget {
public:
    TestTarget()
    {
        m_Device = MTLCreateSystemDefaultDevice();
        m_Queue = [m_Device newCommandQueue];
    }

    ~TestTarget() override
    {
        [m_Queue release];
        [m_Device release];
    }

    void* pyroWaveMetalDevice() override { return m_Device; }
    void* pyroWaveMetalCommandQueue() override { return m_Queue; }

    id<MTLDevice> device() const { return m_Device; }
    id<MTLCommandQueue> queue() const { return m_Queue; }

private:
    id<MTLDevice> m_Device;
    id<MTLCommandQueue> m_Queue;
};

// Decoded planes read back from a frame's textures: 8-bit samples, plus the raw
// 16-bit luma of a 10-bit stream
struct Output {
    Planes planes;
    std::vector<uint16_t> y16;
};

// A blit of a frame's planes into shared buffers, committed behind whatever the
// queue already holds (as a render would be) and waited for by finish()
class Readback {
public:
    Readback(TestTarget& target, const AVFrame* frame, bool tenBit)
        : m_TenBit(tenBit)
    { @autoreleasepool {
        auto ref = PyroWaveMetalFrameRef::fromFrame(frame);
        expect(ref != nullptr, "decoded frame carries a PyroWaveMetalFrameRef");
        m_Planes.allocate(frame->width, frame->height,
                          frame->format == AV_PIX_FMT_YUV444P || frame->format == AV_PIX_FMT_YUV444P10);
        if (ref == nullptr) {
            return;
        }

        const NSUInteger bpp = tenBit ? 2 : 1;
        m_CommandBuffer = [[target.queue() commandBuffer] retain];
        id<MTLBlitCommandEncoder> blit = [m_CommandBuffer blitCommandEncoder];
        for (int plane = 0; plane < 3; plane++) {
            auto texture = (id<MTLTexture>)ref->planes[plane];
            m_Widths[plane] = texture.width;
            m_Heights[plane] = texture.height;
            m_Buffers[plane] = [target.device() newBufferWithLength:texture.width * texture.height * bpp
                                                            options:MTLResourceStorageModeShared];
            [blit copyFromTexture:texture
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                         toBuffer:m_Buffers[plane]
                destinationOffset:0
           destinationBytesPerRow:texture.width * bpp
         destinationBytesPerImage:texture.width * texture.height * bpp];
        }
        [blit endEncoding];
        [m_CommandBuffer commit];
    }}

    ~Readback()
    {
        [m_CommandBuffer release];
        for (id<MTLBuffer> buffer : m_Buffers) {
            [buffer release];
        }
    }

    Output finish()
    {
        Output output;
        output.planes = m_Planes;
        if (m_CommandBuffer == nil) {
            return output;
        }

        [m_CommandBuffer waitUntilCompleted];
        expect(m_CommandBuffer.status == MTLCommandBufferStatusCompleted, "readback completed");

        std::vector<uint8_t>* dst[3] = { &output.planes.y, &output.planes.cb, &output.planes.cr };
        for (int plane = 0; plane < 3; plane++) {
            const size_t count = m_Widths[plane] * m_Heights[plane];
            expect(dst[plane]->size() == count, "plane size matches the frame");
            if (dst[plane]->size() != count) {
                continue;
            }
            if (!m_TenBit) {
                std::memcpy(dst[plane]->data(), m_Buffers[plane].contents, count);
                continue;
            }
            auto samples = static_cast<const uint16_t*>(m_Buffers[plane].contents);
            for (size_t i = 0; i < count; i++) {
                (*dst[plane])[i] = uint8_t(std::lround(samples[i] / 257.0));
            }
            if (plane == 0) {
                output.y16.assign(samples, samples + count);
            }
        }
        return output;
    }

private:
    bool m_TenBit;
    Planes m_Planes;
    id<MTLCommandBuffer> m_CommandBuffer = nil;
    id<MTLBuffer> m_Buffers[3] = {};
    NSUInteger m_Widths[3] = {};
    NSUInteger m_Heights[3] = {};
};

struct Difference {
    int max = 0;
    double mean = 0;
};

Difference compare(const Planes& a, const Planes& b)
{
    Difference result;
    double sum = 0;
    size_t count = 0;
    for (auto plane : { &Planes::y, &Planes::cb, &Planes::cr }) {
        const auto& pa = a.*plane;
        const auto& pb = b.*plane;
        for (size_t i = 0; i < std::min(pa.size(), pb.size()); i++) {
            const int diff = std::abs(int(pa[i]) - int(pb[i]));
            result.max = std::max(result.max, diff);
            sum += diff;
        }
        count += std::min(pa.size(), pb.size());
    }
    result.mean = count ? sum / double(count) : 0;
    return result;
}

bool identical(const Planes& a, const Planes& b)
{
    return a.y == b.y && a.cb == b.cb && a.cr == b.cr;
}

// The client's decode of one host-framed frame. segments empty: arrived whole.
AVFrame* metalDecode(PyroWaveMetalDecoder& decoder, const std::vector<uint8_t>& framed,
                     const std::vector<PyroWaveFraming::Segment>& segments, size_t criticalPayloads,
                     const std::string& name)
{
    AVFrame* frame = av_frame_alloc();
    if (!decoder.decode(framed.data(), framed.size(), segments, criticalPayloads, frame)) {
        expect(false, name + ": Metal decode failed: " + decoder.lastError());
        av_frame_free(&frame);
        return nullptr;
    }
    return frame;
}

// PyroWaveDecoder's acceptance rules with the Vulkan decoder's CPU output, as the
// reference for the same bytes. Returns false if the frame would be dropped.
bool vulkanDecode(pyrowave_decoder decoder, const std::vector<uint8_t>& framed,
                  const std::vector<PyroWaveFraming::Segment>& segments, size_t criticalPayloads,
                  const PyroWaveFraming::StreamGeometry& geometry, Planes& output, const std::string& name)
{
    PyroWaveFraming::Frame frame;
    std::string error;
    if (!PyroWaveFraming::parse(framed.data(), framed.size(), segments, criticalPayloads, geometry, frame, error)) {
        expect(false, name + ": parse failed: " + error);
        return false;
    }

    pyrowave_decoder_clear(decoder);
    for (const auto& span : frame.spans) {
        if (pyrowave_decoder_push_packet(decoder, framed.data() + span.offset, span.size) != PYROWAVE_SUCCESS) {
            expect(false, name + ": Vulkan push_packet rejected a span");
            return false;
        }
    }
    const bool ready = frame.partial ?
        frame.coarseLevelIntact && pyrowave_decoder_decode_is_ready_with_sideband(decoder, true, 0, 0.9f, nullptr, 0) :
        pyrowave_decoder_decode_is_ready(decoder, false);
    if (!ready) {
        return false;
    }

    auto buffer = output.buffer();
    if (pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) != PYROWAVE_SUCCESS) {
        expect(false, name + ": Vulkan decode failed");
        return false;
    }
    return true;
}

// Checks one Metal output against the source and, if given, the Vulkan reference
void checkOutput(const Output& output, const Planes& source, const Planes* reference, bool tenBit,
                 double minimumPsnr, const std::string& name)
{
    const double quality = psnr(source.y, output.planes.y);
    expect(quality > minimumPsnr, name + ": luma PSNR " + std::to_string(quality));

    if (tenBit) {
        // Full-scale 16-bit samples: 10 bits in the low bits would stay below 1024
        const uint16_t peak = output.y16.empty() ? 0 : *std::max_element(output.y16.begin(), output.y16.end());
        expect(peak > 1023 * 32, name + ": 10-bit planes hold full-scale 16-bit samples (peak " +
                                     std::to_string(peak) + ")");

        // And finer than 8 bits: 8-bit results widened to 16 bits (byte * 257)
        // would pass the comparisons below, which are 8-bit because the Vulkan
        // decoder's CPU output is
        const size_t fine = std::count_if(output.y16.begin(), output.y16.end(),
                                          [](uint16_t sample) { return sample % 257 != 0; });
        expect(fine * 2 > output.y16.size(),
               name + ": 10-bit planes keep precision beyond 8 bits (" + std::to_string(fine) + " of " +
               std::to_string(output.y16.size()) + " samples between 8-bit steps)");
    }

    if (reference != nullptr) {
        // The Metal port keeps FP32 lifting math and FP16 storage, as the
        // Vulkan decoder does at PYROWAVE_PRECISION=1: within rounding of it.
        // At 8 bits only: the Vulkan reference decodes to 8-bit CPU planes.
        const Difference diff = compare(output.planes, *reference);
        expect(diff.max <= 2 && diff.mean < 0.25,
               name + ": matches the Vulkan decoder (max " + std::to_string(diff.max) +
               ", mean " + std::to_string(diff.mean) + ")");
    }
}

std::vector<pyrowave_packet> toPackets(const std::vector<EncodedPacket>& encoded)
{
    std::vector<pyrowave_packet> packets;
    for (const auto& packet : encoded) {
        packets.push_back({ packet.offset, packet.size });
    }
    return packets;
}

// Reusing a surface a committed read still samples: holds every surface, queues
// a read of one, frees it and decodes another frame into it at once. The queued
// read must still see the first frame.
void checkSurfaceReuse(TestTarget& target, PyroWaveMetalDecoder& decoder,
                       const std::vector<uint8_t>& first, const Output& firstOutput,
                       const std::vector<uint8_t>& second, const Output& secondOutput,
                       bool tenBit, const std::string& name)
{
    std::vector<AVFrame*> held;
    for (int i = 0; i < 64; i++) {
        AVFrame* frame = av_frame_alloc();
        if (!decoder.decode(first.data(), first.size(), {}, 0, frame)) {
            av_frame_free(&frame);
            break;
        }
        held.push_back(frame);
    }
    expect(held.size() >= 2 && held.size() < 64,
           name + ": the surface pool is bounded (" + std::to_string(held.size()) + " surfaces)");
    expect(decoder.lastError() == "no free output surface", name + ": an exhausted pool drops the frame");
    if (held.empty()) {
        return;
    }

    const int reused = PyroWaveMetalFrameRef::fromFrame(held.front())->surface;
    Readback queuedRead(target, held.front(), tenBit);
    av_frame_free(&held.front());
    held.erase(held.begin());

    AVFrame* frame = metalDecode(decoder, second, {}, 0, name + " reuse");
    if (frame != nullptr) {
        expect(PyroWaveMetalFrameRef::fromFrame(frame)->surface == reused,
               name + ": the next decode reuses the freed surface");
        Readback secondRead(target, frame, tenBit);
        expect(identical(queuedRead.finish().planes, firstOutput.planes),
               name + ": a read queued before the reuse still sees its frame");
        expect(identical(secondRead.finish().planes, secondOutput.planes),
               name + ": the reused surface holds the new frame");
        av_frame_free(&frame);
    }

    for (AVFrame* heldFrame : held) {
        av_frame_free(&heldFrame);
    }
}

void runCase(TestTarget& target, pyrowave_device vulkanDevice, int width, int height, bool chroma444,
             bool tenBit, size_t budget, bool checkReuse)
{ @autoreleasepool {
    const std::string name = std::to_string(width) + "x" + std::to_string(height) +
                             (chroma444 ? " 4:4:4" : " 4:2:0") + (tenBit ? " 10-bit" : " 8-bit");
    const PyroWaveFraming::StreamGeometry geometry {width, height, chroma444};

    MetalEncoder encoder;
    if (!encoder.create(width, height, chroma444)) {
        expect(false, name + ": Metal encoder creation");
        return;
    }

    PyroWaveMetalDecoder decoder;
    PyroWaveMetalDecoder::Config config;
    config.width = width;
    config.height = height;
    config.chroma444 = chroma444;
    config.tenBit = tenBit;
    if (!decoder.initialize(config, &target)) {
        expect(false, name + ": Metal decoder initialization");
        return;
    }

    pyrowave_decoder vulkanDecoder = nullptr;
    if (vulkanDevice != nullptr) {
        pyrowave_decoder_create_info info = {};
        info.device = vulkanDevice;
        info.width = width;
        info.height = height;
        info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
        expect(pyrowave_decoder_create(&info, &vulkanDecoder) == PYROWAVE_SUCCESS, name + ": Vulkan decoder creation");
    }

    Planes source, reference;
    source.allocate(width, height, chroma444);
    reference.allocate(width, height, chroma444);

    const size_t shard = 1392 - 16;
    std::vector<std::vector<uint8_t>> decodedRecords;
    std::vector<Output> decodedOutputs;

    // Frames 1-4 are encoded but never decoded: the decoder must still accept
    // frame 5 even though its 3-bit sequence counter skipped ahead.
    for (int frameIndex = 0; frameIndex < 6; frameIndex++) {
        fillTestImage(source, frameIndex);
        if (!encoder.encode(source.y.data(), source.cb.data(), source.cr.data(), budget)) {
            expect(false, name + ": encode");
            break;
        }
        if (frameIndex != 0 && frameIndex != 5) {
            continue;
        }
        const std::string frameName = name + " frame " + std::to_string(frameIndex);

        std::vector<uint8_t> bitstream(budget + 1024 * 1024);
        std::vector<EncodedPacket> encoded;
        if (!encoder.packetize(shard, 8, bitstream, encoded)) {
            expect(false, frameName + ": packetize");
            break;
        }
        size_t criticalBytes = 0;
        const auto records = recordFrame(bitstream, toPackets(encoded), shard,
                                         PyroWaveFraming::coarseBlockCount(geometry), criticalBytes);

        // Whole, record framed
        AVFrame* frame = metalDecode(decoder, records, {}, 0, frameName + " records");
        if (frame == nullptr) {
            continue;
        }
        const Output whole = Readback(target, frame, tenBit).finish();
        av_frame_free(&frame);
        const bool haveReference = vulkanDecoder != nullptr &&
            vulkanDecode(vulkanDecoder, records, {}, 0, geometry, reference, frameName + " records");
        // Both decoders must accept the frame, or the comparison would be skipped
        expect(vulkanDecoder == nullptr || haveReference, frameName + ": the Vulkan decoder accepts it too");
        checkOutput(whole, source, haveReference ? &reference : nullptr, tenBit, 30.0, frameName + " records");
        decodedRecords.push_back(records);
        decodedOutputs.push_back(whole);

        // Whole, length-prefixed: the same blocks decode to the same planes
        std::vector<EncodedPacket> compat;
        if (encoder.packetize(1024, 0, bitstream, compat)) {
            const auto prefixed = lengthPrefixedFrame(bitstream, toPackets(compat));
            frame = metalDecode(decoder, prefixed, {}, 0, frameName + " length-prefixed");
            if (frame != nullptr) {
                expect(identical(Readback(target, frame, tenBit).finish().planes, whole.planes),
                       frameName + ": length-prefixed framing decodes as record framing does");
                av_frame_free(&frame);
            }
        }

        // One lost payload at a time outside the critical (parity-protected) ones
        const size_t payloads = payloadCount(records.size(), shard);
        const size_t criticalPayloads = payloadCount(criticalBytes, shard);
        int salvaged = 0, tried = 0;
        double worst = 99;
        for (size_t lost = criticalPayloads; lost < payloads;
             lost += std::max<size_t>(1, (payloads - criticalPayloads) / 12)) {
            const std::string lossName = frameName + " lost payload " + std::to_string(lost) +
                                         " of " + std::to_string(payloads);
            auto lossy = records;
            const auto segments = losePayload(lossy, shard, lost);
            tried++;

            frame = av_frame_alloc();
            if (!decoder.decode(lossy.data(), lossy.size(), segments, criticalPayloads, frame)) {
                expect(false, lossName + ": Metal decoder dropped it: " + decoder.lastError());
                av_frame_free(&frame);
                continue;
            }
            expect(decoder.lastFramePartial(), lossName + ": reported as partial");
            const Output partial = Readback(target, frame, tenBit).finish();
            av_frame_free(&frame);
            salvaged++;
            worst = std::min(worst, psnr(source.y, partial.planes.y));

            const bool lossReference = vulkanDecoder != nullptr &&
                vulkanDecode(vulkanDecoder, lossy, segments, criticalPayloads, geometry, reference, lossName);
            expect(vulkanDecoder == nullptr || lossReference, lossName + ": the Vulkan decoder salvages it too");
            checkOutput(partial, source, lossReference ? &reference : nullptr, tenBit, 20.0, lossName);
        }
        expect(salvaged == tried, frameName + ": every loss outside the critical payloads decodes");

        std::printf("%s: %zu payloads (%zu critical), whole %.1f dB, %d of %d single losses salvaged "
                    "(worst %.1f dB)%s\n",
                    frameName.c_str(), payloads, criticalPayloads, psnr(source.y, whole.planes.y),
                    salvaged, tried, worst, haveReference ? ", matches Vulkan" : "");
    }

    if (checkReuse && decodedRecords.size() == 2) {
        checkSurfaceReuse(target, decoder, decodedRecords[0], decodedOutputs[0],
                          decodedRecords[1], decodedOutputs[1], tenBit, name);
    }

    if (vulkanDecoder != nullptr) {
        pyrowave_decoder_destroy(vulkanDecoder);
    }
}}

}

int main()
{ @autoreleasepool {
    TestTarget target;
    if (target.device() == nil || !PyroWaveMetalDecoder::isSupported(&target)) {
        std::printf("No Metal GPU that runs PyroWave (Apple7 family); skipping\n");
        return 0;
    }
    std::printf("Metal device: %s\n", target.device().name.UTF8String);

    pyrowave_device vulkanDevice = nullptr;
    if (pyrowave_create_default_device(&vulkanDevice) != PYROWAVE_SUCCESS) {
        vulkanDevice = nullptr;
        std::printf("No Vulkan device: checking against the source only "
                    "(set GRANITE_VULKAN_LIBRARY to a MoltenVK dylib to compare decoders)\n");
    }

    // Budgets around 1.6 bits per pixel
    runCase(target, vulkanDevice, 1280, 720, false, false, 180 * 1024, true);
    runCase(target, vulkanDevice, 1920, 1080, false, false, 400 * 1024, false);
    runCase(target, vulkanDevice, 1920, 1080, true, false, 650 * 1024, false);
    runCase(target, vulkanDevice, 1920, 1080, false, true, 400 * 1024, true);
    // The MacBook Pro 14" native size streamed in the field
    runCase(target, vulkanDevice, 3024, 1890, false, true, 1100 * 1024, false);

    if (vulkanDevice != nullptr) {
        pyrowave_device_destroy(vulkanDevice);
    }

    if (g_Failures == 0) {
        std::printf("PyroWave Metal decoder: all checks passed\n");
    }
    return g_Failures ? 1 : 0;
}}
