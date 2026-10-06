#include "pyrowavemetaldecoder.h"
#include "pyrowavemetaltarget.h"

// The Metal port links beside the Vulkan library under renamed symbols
#include "pyrowave_metal_names.h"
#include <metal/pyrowave_metal.h>

#include <Limelight.h>
#include <SDL.h>

#import <Metal/Metal.h>

#include <array>
#include <deque>
#include <mutex>

namespace {

// Matches the Vulkan surface pools: enough for the pacer's outstanding frames
// and the VRR worker's queue, with the decoder writing one more.
constexpr int k_SurfaceCount = 10;

// The plane textures, shared by the decoder and every frame that references
// them so a frame outliving the decoder keeps its planes.
struct SurfacePool {
    std::mutex lock;
    std::deque<int> free;
    std::vector<std::array<id<MTLTexture>, 3>> planes;

    ~SurfacePool()
    {
        for (auto& surface : planes) {
            for (id<MTLTexture> plane : surface) {
                [plane release];
            }
        }
    }

    void push(int surface)
    {
        std::lock_guard<std::mutex> guard(lock);
        free.push_back(surface);
    }

    bool pop(int& surface)
    {
        std::lock_guard<std::mutex> guard(lock);
        if (free.empty()) {
            return false;
        }
        surface = free.front();
        free.pop_front();
        return true;
    }
};

struct FrameOwner {
    std::shared_ptr<SurfacePool> pool;
};

// Every render that sampled the surface was committed to the shared queue
// before the frame was freed, so the next decode into it is ordered after
// those reads: the surface is free at once.
void freeFrameRef(void* opaque, uint8_t* data)
{
    auto owner = static_cast<FrameOwner*>(opaque);
    auto ref = reinterpret_cast<PyroWaveMetalFrameRef*>(data);

    owner->pool->push(ref->surface);

    delete ref;
    delete owner;
}

void logMessage(void*, const char* message)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PyroWave (Metal): %s", message);
}

}

struct PyroWaveMetalDecoder::Impl {
    Config config;
    PyroWaveFraming::StreamGeometry geometry {};

    pyrowave_device device = nullptr;
    pyrowave_decoder decoder = nullptr;
    id<MTLCommandQueue> queue = nil;
    std::shared_ptr<SurfacePool> pool = std::make_shared<SurfacePool>();

    PyroWaveFraming::Frame parsed;

    ~Impl()
    {
        // Committed decodes retain what they use, so nothing waits here
        if (decoder != nullptr) {
            pyrowave_decoder_destroy(decoder);
        }
        if (device != nullptr) {
            pyrowave_device_destroy(device);
        }
        [queue release];
    }

    bool createSurfaces(id<MTLDevice> mtlDevice)
    {
        const MTLPixelFormat format = config.tenBit ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
        const NSUInteger chromaWidth = config.chroma444 ? config.width : config.width / 2;
        const NSUInteger chromaHeight = config.chroma444 ? config.height : config.height / 2;

        for (int i = 0; i < k_SurfaceCount; i++) {
            std::array<id<MTLTexture>, 3> surface {};
            for (int plane = 0; plane < 3; plane++) {
                MTLTextureDescriptor* desc =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                       width:plane == 0 ? config.width : chromaWidth
                                                                      height:plane == 0 ? config.height : chromaHeight
                                                                   mipmapped:NO];
                desc.storageMode = MTLStorageModePrivate;
                desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                surface[plane] = [mtlDevice newTextureWithDescriptor:desc];
                if (surface[plane] == nil) {
                    for (int j = 0; j < plane; j++) {
                        [surface[j] release];
                    }
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "PyroWave: allocating Metal surface %d plane %d failed", i, plane);
                    return false;
                }
            }
            pool->planes.push_back(surface);
            pool->free.push_back(i);
        }
        return true;
    }
};

bool PyroWaveMetalDecoder::isSupported(IPyroWaveMetalTarget* target)
{
    return target != nullptr && target->pyroWaveMetalDevice() != nullptr &&
           pyrowave_device_is_supported(target->pyroWaveMetalDevice());
}

PyroWaveMetalDecoder::PyroWaveMetalDecoder() = default;

PyroWaveMetalDecoder::~PyroWaveMetalDecoder() = default;

bool PyroWaveMetalDecoder::initialize(const Config& config, IPyroWaveMetalTarget* target)
{ @autoreleasepool {
    SDL_assert(!m_Impl);

    if (config.width <= 0 || config.height <= 0 || target == nullptr) {
        return false;
    }
    if (!config.chroma444 && ((config.width | config.height) & 1)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: 4:2:0 streams need an even width and height (%dx%d)",
                     config.width, config.height);
        return false;
    }

    auto mtlDevice = static_cast<id<MTLDevice>>(target->pyroWaveMetalDevice());
    auto queue = static_cast<id<MTLCommandQueue>>(target->pyroWaveMetalCommandQueue());
    if (mtlDevice == nil || queue == nil) {
        return false;
    }
    if (!pyrowave_device_is_supported(mtlDevice)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: %s cannot run the native Metal decoder",
                    mtlDevice.name.UTF8String);
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->geometry = { config.width, config.height, config.chroma444 };
    impl->queue = [queue retain];

    pyrowave_device_create_info deviceInfo = {};
    deviceInfo.mtl_device = mtlDevice;
    deviceInfo.message_callback = logMessage;
    // Compiles the decode pipelines from their embedded source
    pyrowave_result result = pyrowave_device_create(&deviceInfo, &impl->device);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: creating the Metal device failed: %s",
                     pyrowave_result_to_string(result));
        return false;
    }

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = impl->device;
    decoderInfo.width = config.width;
    decoderInfo.height = config.height;
    decoderInfo.chroma = config.chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    result = pyrowave_decoder_create(&decoderInfo, &impl->decoder);
    if (result != PYROWAVE_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: Metal decoder creation failed: %s",
                     pyrowave_result_to_string(result));
        return false;
    }

    if (!impl->createSurfaces(mtlDevice)) {
        return false;
    }

    uint32_t major = 0, minor = 0, patch = 0;
    pyrowave_get_api_version(&major, &minor, &patch);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave decoder ready: %dx%d %s %d-bit, %d surfaces, native Metal on %s, API %u.%u.%u, bitstream %s",
                config.width, config.height,
                config.chroma444 ? "4:4:4" : "4:2:0",
                config.tenBit ? 10 : 8,
                k_SurfaceCount,
                mtlDevice.name.UTF8String,
                major, minor, patch, PYROWAVE_BITSTREAM_ID);

    m_Impl = std::move(impl);
    return true;
}}

bool PyroWaveMetalDecoder::decode(const uint8_t* data, size_t size,
                                  const std::vector<PyroWaveFraming::Segment>& packets, size_t criticalPackets,
                                  AVFrame* frame)
{ @autoreleasepool {
    Impl& impl = *m_Impl;

    m_LastFramePartial = false;
    if (!PyroWaveFraming::parse(data, size, packets, criticalPackets, impl.geometry, impl.parsed, m_LastError)) {
        return false;
    }

    // Every frame is independent. Clearing first keeps the 3-bit sequence
    // counter from treating a frame after a long drop as stale.
    pyrowave_decoder_clear(impl.decoder);
    for (const auto& span : impl.parsed.spans) {
        const pyrowave_result result = pyrowave_decoder_push_packet(impl.decoder, data + span.offset, span.size);
        if (result != PYROWAVE_SUCCESS) {
            m_LastError = std::string("decoder rejected a packet: ") + pyrowave_result_to_string(result);
            return false;
        }
    }

    if (!impl.parsed.partial) {
        if (!pyrowave_decoder_decode_is_ready(impl.decoder, false)) {
            m_LastError = "frame is incomplete";
            return false;
        }
    }
    else {
        // As in PyroWaveDecoder: a partial frame decodes if its coarsest
        // wavelet level is intact (checked by the parser) and more than 90% of
        // its blocks arrived; missing detail decodes as blur.
        if (!impl.parsed.coarseLevelIntact) {
            m_LastError = "part of the coarsest wavelet level was lost";
            return false;
        }
        if (!pyrowave_decoder_decode_is_ready_with_sideband(impl.decoder, true, 0, 0.9f, nullptr, 0)) {
            m_LastError = "too little of the frame arrived (" + std::to_string(impl.parsed.blockRecords) +
                          " of " + std::to_string(impl.parsed.announcedBlocks) + " blocks)";
            return false;
        }
    }
    m_LastFramePartial = impl.parsed.partial;

    int surface;
    if (!impl.pool->pop(surface)) {
        m_LastError = "no free output surface";
        return false;
    }

    id<MTLCommandBuffer> commandBuffer = [impl.queue commandBuffer];
    if (commandBuffer == nil) {
        impl.pool->push(surface);
        m_LastError = "no Metal command buffer";
        return false;
    }
    commandBuffer.label = @"PyroWave decode";

    pyrowave_gpu_buffers buffers = {};
    for (int plane = 0; plane < 3; plane++) {
        buffers.planes[plane] = impl.pool->planes[surface][plane];
    }
    const pyrowave_result result = pyrowave_decoder_decode_gpu_buffer(impl.decoder, commandBuffer, &buffers);
    if (result != PYROWAVE_SUCCESS) {
        // Nothing was committed
        impl.pool->push(surface);
        m_LastError = std::string("decode encoding failed: ") + pyrowave_result_to_string(result);
        return false;
    }
    [commandBuffer commit];

    frame->width = impl.config.width;
    frame->height = impl.config.height;
    if (impl.config.tenBit) {
        frame->format = impl.config.chroma444 ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV420P10;
    }
    else {
        frame->format = impl.config.chroma444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_YUV420P;
    }
    // Hosts average each 2x2 quad for 4:2:0 chroma
    frame->chroma_location = AVCHROMA_LOC_CENTER;
    frame->flags |= AV_FRAME_FLAG_KEY;

    auto ref = new PyroWaveMetalFrameRef();
    ref->surface = surface;
    for (int plane = 0; plane < 3; plane++) {
        ref->planes[plane] = buffers.planes[plane];
    }
    auto owner = new FrameOwner { impl.pool };

    frame->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(ref), sizeof(*ref),
                                     freeFrameRef, owner, 0);
    if (frame->buf[0] == nullptr) {
        // Later decodes into this surface queue behind the one just committed
        impl.pool->push(surface);
        delete ref;
        delete owner;
        m_LastError = "out of memory";
        return false;
    }

    return true;
}}
