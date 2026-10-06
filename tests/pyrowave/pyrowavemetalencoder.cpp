#include "pyrowavemetalencoder.h"

#include "pyrowave_metal_names.h"
#include <metal/pyrowave_metal.h>

namespace PyroWaveTest {

struct MetalEncoder::Impl {
    pyrowave_device device = nullptr;
    pyrowave_encoder encoder = nullptr;
    int width = 0;
    int height = 0;
    bool chroma444 = false;

    ~Impl()
    {
        if (encoder != nullptr) {
            pyrowave_encoder_destroy(encoder);
        }
        if (device != nullptr) {
            pyrowave_device_destroy(device);
        }
    }
};

MetalEncoder::MetalEncoder() = default;

MetalEncoder::~MetalEncoder() = default;

bool MetalEncoder::create(int width, int height, bool chroma444)
{
    auto impl = std::make_unique<Impl>();
    impl->width = width;
    impl->height = height;
    impl->chroma444 = chroma444;
    if (pyrowave_create_default_device(&impl->device) != PYROWAVE_SUCCESS) {
        return false;
    }

    pyrowave_encoder_create_info info = {};
    info.device = impl->device;
    info.width = width;
    info.height = height;
    info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    if (pyrowave_encoder_create(&info, &impl->encoder) != PYROWAVE_SUCCESS) {
        return false;
    }

    m_Impl = std::move(impl);
    return true;
}

bool MetalEncoder::encode(const uint8_t* y, const uint8_t* cb, const uint8_t* cr, size_t budget)
{
    const Impl& impl = *m_Impl;
    const size_t chromaWidth = impl.chroma444 ? impl.width : impl.width / 2;
    const size_t chromaHeight = impl.chroma444 ? impl.height : impl.height / 2;

    pyrowave_cpu_buffer buffer = {};
    buffer.data[0] = const_cast<uint8_t*>(y);
    buffer.data[1] = const_cast<uint8_t*>(cb);
    buffer.data[2] = const_cast<uint8_t*>(cr);
    buffer.row_stride_in_bytes[0] = size_t(impl.width);
    buffer.row_stride_in_bytes[1] = chromaWidth;
    buffer.row_stride_in_bytes[2] = chromaWidth;
    buffer.plane_size_in_bytes[0] = size_t(impl.width) * impl.height;
    buffer.plane_size_in_bytes[1] = chromaWidth * chromaHeight;
    buffer.plane_size_in_bytes[2] = chromaWidth * chromaHeight;
    buffer.width = impl.width;
    buffer.height = impl.height;
    buffer.format = impl.chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

    pyrowave_rate_control rate = { budget };
    return pyrowave_encoder_encode_cpu_synchronous(impl.encoder, &buffer, &rate) == PYROWAVE_SUCCESS;
}

bool MetalEncoder::packetize(size_t boundary, size_t padding, std::vector<uint8_t>& bitstream,
                             std::vector<EncodedPacket>& packets)
{
    const Impl& impl = *m_Impl;

    size_t count = 0;
    pyrowave_result result = padding != 0 ?
        pyrowave_encoder_compute_num_packets_with_padding(impl.encoder, boundary, padding, &count) :
        pyrowave_encoder_compute_num_packets(impl.encoder, boundary, &count);
    if (result != PYROWAVE_SUCCESS) {
        return false;
    }

    std::vector<pyrowave_packet> raw(count);
    size_t written = 0;
    result = padding != 0 ?
        pyrowave_encoder_packetize_with_padding(impl.encoder, raw.data(), boundary, padding, &written,
                                                bitstream.data(), bitstream.size()) :
        pyrowave_encoder_packetize(impl.encoder, raw.data(), boundary, &written,
                                   bitstream.data(), bitstream.size());
    if (result != PYROWAVE_SUCCESS) {
        return false;
    }

    packets.clear();
    for (size_t i = 0; i < written; i++) {
        packets.push_back({ raw[i].offset, raw[i].size });
    }
    return true;
}

}
