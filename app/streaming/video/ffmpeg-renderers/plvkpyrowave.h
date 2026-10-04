#pragma once

#include "streaming/video/pyrowave/pyrowavesurfaces.h"
#include "streaming/video/pyrowave/pyrowavevulkansurfaces.h"

#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include <array>
#include <memory>
#include <vector>

// libplacebo side of the PyroWave same-device surfaces: plane textures on the
// renderer's pl_vulkan device that the decoder writes in place. Between
// renders every plane is held (pl_vulkan_hold_ex) for the decoder; map()
// releases a frame's planes to libplacebo behind the decode semaphore and
// unmap() holds them again, signalling the surface's hold semaphore.
class PlVkPyroWaveSurfaces : public IPyroWaveVulkanSurfaces
{
public:
    // Enough for the pacer's outstanding frames, the VRR worker's extra
    // queued frame, and one decode plus one render in flight.
    static constexpr int k_SurfaceCount = 10;

    // Features to request at device creation: everything the device
    // supports, which covers PyroWave's compute requirements.
    // The returned chain lives as long as this object.
    const VkPhysicalDeviceFeatures2* queryDeviceFeatures(pl_vk_inst instance, VkPhysicalDevice device);

    bool initialize(pl_vk_inst instance, pl_vulkan vulkan,
                    int width, int height, bool chroma444, bool tenBit);
    ~PlVkPyroWaveSurfaces();

    // Builds the libplacebo frame for a decoded surface. Each successful map
    // must be paired with unmap() once the reads are recorded.
    bool map(const AVFrame* frame, const PyroWaveFrameRef* ref, pl_frame* mappedFrame);
    void unmap(PyroWaveFrameRef* ref);

    // CPU wait for the frame's decode, in microseconds.
    uint64_t waitForDecode(const PyroWaveFrameRef* ref);

    // IPyroWaveVulkanSurfaces
    const pyrowave_device_create_info* pyroWaveDeviceInfo() override { return &m_DeviceInfo; }
    int pyroWaveSurfaceCount() const override { return (int)m_Surfaces.size(); }
    void pyroWaveSurface(int index, pyrowave_gpu_buffers* planes,
                         VkSemaphore* holdSemaphore, uint64_t* holdValue) override;
    VkSemaphore pyroWaveDecodeSemaphore() override { return m_DecodeSemaphore; }

private:
    bool hold(int surface, uint64_t* value);

    struct Surface {
        std::array<pl_tex, 3> planes {};
        pyrowave_gpu_buffers views {};
        VkSemaphore holdSemaphore = VK_NULL_HANDLE;
        uint64_t holdValue = 0;
    };

    pl_vulkan m_Vulkan = nullptr;
    std::vector<Surface> m_Surfaces;
    VkSemaphore m_DecodeSemaphore = VK_NULL_HANDLE;
    PFN_vkWaitSemaphores m_WaitSemaphores = nullptr;
    bool m_TenBit = false;

    VkPhysicalDeviceFeatures2 m_Features {};
    VkPhysicalDeviceVulkan11Features m_Features11 {};
    VkPhysicalDeviceVulkan12Features m_Features12 {};
    VkPhysicalDeviceVulkan13Features m_Features13 {};

    // pyrowave_create_device() keeps pointers into these
    VkApplicationInfo m_AppInfo {};
    VkInstanceCreateInfo m_InstanceInfo {};
    float m_QueuePriority = 1.0f;
    VkDeviceQueueCreateInfo m_QueueInfo {};
    VkDeviceCreateInfo m_VkDeviceInfo {};
    pyrowave_device_create_queue_info m_Queue {};
    pyrowave_device_create_info m_DeviceInfo {};
};
