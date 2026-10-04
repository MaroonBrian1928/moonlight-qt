#pragma once

// Same-device alternative to IPyroWaveSurfacePool for a Vulkan renderer. The
// decoder wraps the renderer's VkDevice and writes straight into the
// renderer's plane images. A surface belongs to the decoder except while the
// renderer samples it: the renderer waits on the decode semaphore before its
// reads and signals the surface's hold semaphore once they retire, and the
// decoder waits for that hold value before overwriting the surface.

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <cstdint>

class IPyroWaveVulkanSurfaces {
public:
    virtual ~IPyroWaveVulkanSurfaces() = default;

    // Describes the renderer's device for pyrowave_create_device(). It stays
    // valid until the renderer is destroyed.
    virtual const pyrowave_device_create_info* pyroWaveDeviceInfo() = 0;

    virtual int pyroWaveSurfaceCount() const = 0;

    // Plane views of a surface (GENERAL layout), its hold semaphore, and the
    // hold value that must be reached before the decoder first writes it.
    virtual void pyroWaveSurface(int index, pyrowave_gpu_buffers* planes,
                                 VkSemaphore* holdSemaphore, uint64_t* holdValue) = 0;

    // Timeline semaphore the decoder signals when a surface is written.
    virtual VkSemaphore pyroWaveDecodeSemaphore() = 0;
};
