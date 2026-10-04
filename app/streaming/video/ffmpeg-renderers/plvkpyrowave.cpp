#include "plvkpyrowave.h"

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>

#include <Limelight.h>
#include <SDL.h>

#include <cstring>

namespace {

constexpr uint64_t k_DecodeWaitTimeoutNs = 100000000;

}

const VkPhysicalDeviceFeatures2* PlVkPyroWaveSurfaces::queryDeviceFeatures(pl_vk_inst instance, VkPhysicalDevice device)
{
    auto getProperties = (PFN_vkGetPhysicalDeviceProperties)
        instance->get_proc_addr(instance->instance, "vkGetPhysicalDeviceProperties");
    auto getFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2)
        instance->get_proc_addr(instance->instance, "vkGetPhysicalDeviceFeatures2");
    if (getProperties == nullptr || getFeatures2 == nullptr) {
        return nullptr;
    }

    VkPhysicalDeviceProperties properties;
    getProperties(device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        return nullptr;
    }

    m_Features13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    m_Features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &m_Features13 };
    m_Features11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &m_Features12 };
    m_Features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &m_Features11 };
    getFeatures2(device, &m_Features);

    // Robust access only costs performance here
    m_Features.features.robustBufferAccess = VK_FALSE;
    m_Features13.robustImageAccess = VK_FALSE;
    return &m_Features;
}

bool PlVkPyroWaveSurfaces::initialize(pl_vk_inst instance, pl_vulkan vulkan,
                                      int width, int height, bool chroma444, bool tenBit)
{
    m_Vulkan = vulkan;
    m_TenBit = tenBit;

    auto getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        vulkan->get_proc_addr(vulkan->instance, "vkGetDeviceProcAddr");
    auto getDeviceQueue = getDeviceProcAddr ?
        (PFN_vkGetDeviceQueue)getDeviceProcAddr(vulkan->device, "vkGetDeviceQueue") : nullptr;
    m_WaitSemaphores = getDeviceProcAddr ?
        (PFN_vkWaitSemaphores)getDeviceProcAddr(vulkan->device, "vkWaitSemaphores") : nullptr;
    if (getDeviceQueue == nullptr || m_WaitSemaphores == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: missing Vulkan device functions");
        return false;
    }

    const int depth = tenBit ? 16 : 8;
    pl_fmt format = pl_find_fmt(vulkan->gpu, PL_FMT_UNORM, 1, depth, depth,
                                (pl_fmt_caps)(PL_FMT_CAP_SAMPLEABLE | PL_FMT_CAP_STORABLE | PL_FMT_CAP_RENDERABLE));
    if (format == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: no %d-bit plane format that the decoder can write", depth);
        return false;
    }

    pl_vulkan_sem_params semParams = {};
    semParams.type = VK_SEMAPHORE_TYPE_TIMELINE;
    semParams.debug_tag = PL_DEBUG_TAG;
    m_DecodeSemaphore = pl_vulkan_sem_create(vulkan->gpu, &semParams);
    if (m_DecodeSemaphore == VK_NULL_HANDLE) {
        return false;
    }

    const int chromaWidth = chroma444 ? width : width / 2;
    const int chromaHeight = chroma444 ? height : height / 2;

    m_Surfaces.resize(k_SurfaceCount);
    for (int i = 0; i < k_SurfaceCount; i++) {
        Surface& surface = m_Surfaces[i];
        for (int plane = 0; plane < 3; plane++) {
            pl_tex_params texParams = {};
            texParams.w = plane == 0 ? width : chromaWidth;
            texParams.h = plane == 0 ? height : chromaHeight;
            texParams.format = format;
            texParams.sampleable = true;
            texParams.storable = true;
            texParams.renderable = true;
            texParams.debug_tag = PL_DEBUG_TAG;
            surface.planes[plane] = pl_tex_create(vulkan->gpu, &texParams);
            if (surface.planes[plane] == nullptr) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "PyroWave: creating a %dx%d plane texture failed",
                             texParams.w, texParams.h);
                return false;
            }

            VkFormat vkFormat;
            pyrowave_image_view& view = surface.views.planes[plane];
            view.image = pl_vulkan_unwrap(vulkan->gpu, surface.planes[plane], &vkFormat, nullptr);
            view.width = texParams.w;
            view.height = texParams.h;
            view.image_format = vkFormat;
            view.view_format = vkFormat;
            view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
            view.layout = VK_IMAGE_LAYOUT_GENERAL;
        }

        surface.holdSemaphore = pl_vulkan_sem_create(vulkan->gpu, &semParams);
        if (surface.holdSemaphore == VK_NULL_HANDLE || !hold(i, &surface.holdValue)) {
            return false;
        }
    }

    // PyroWave submits only to libplacebo's first graphics queue, under
    // libplacebo's lock for it.
    m_AppInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    m_AppInfo.apiVersion = instance->api_version;
    m_InstanceInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    m_InstanceInfo.pApplicationInfo = &m_AppInfo;
    m_InstanceInfo.enabledExtensionCount = instance->num_extensions;
    m_InstanceInfo.ppEnabledExtensionNames = instance->extensions;

    m_QueueInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    m_QueueInfo.queueFamilyIndex = vulkan->queue_graphics.index;
    m_QueueInfo.queueCount = 1;
    m_QueueInfo.pQueuePriorities = &m_QueuePriority;

    m_VkDeviceInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    m_VkDeviceInfo.pNext = vulkan->features;
    m_VkDeviceInfo.queueCreateInfoCount = 1;
    m_VkDeviceInfo.pQueueCreateInfos = &m_QueueInfo;
    m_VkDeviceInfo.enabledExtensionCount = vulkan->num_extensions;
    m_VkDeviceInfo.ppEnabledExtensionNames = vulkan->extensions;

    m_Queue.familyIndex = vulkan->queue_graphics.index;
    m_Queue.index = 0;
    getDeviceQueue(vulkan->device, m_Queue.familyIndex, m_Queue.index, &m_Queue.queue);

    m_DeviceInfo = {};
    m_DeviceInfo.GetInstanceProcAddr = vulkan->get_proc_addr;
    m_DeviceInfo.instance = vulkan->instance;
    m_DeviceInfo.physical_device = vulkan->phys_device;
    m_DeviceInfo.device = vulkan->device;
    m_DeviceInfo.instance_create_info = &m_InstanceInfo;
    m_DeviceInfo.device_create_info = &m_VkDeviceInfo;
    m_DeviceInfo.queue_info = &m_Queue;
    m_DeviceInfo.queue_info_count = 1;
    m_DeviceInfo.queue_lock_callback = [](void* userdata) {
        auto self = static_cast<PlVkPyroWaveSurfaces*>(userdata);
        self->m_Vulkan->lock_queue(self->m_Vulkan, self->m_Queue.familyIndex, self->m_Queue.index);
    };
    m_DeviceInfo.queue_unlock_callback = [](void* userdata) {
        auto self = static_cast<PlVkPyroWaveSurfaces*>(userdata);
        self->m_Vulkan->unlock_queue(self->m_Vulkan, self->m_Queue.familyIndex, self->m_Queue.index);
    };
    m_DeviceInfo.userdata = this;

    return true;
}

PlVkPyroWaveSurfaces::~PlVkPyroWaveSurfaces()
{
    if (m_Vulkan == nullptr) {
        return;
    }

    // The decoder is already destroyed; finish libplacebo's work on the planes
    pl_gpu_finish(m_Vulkan->gpu);
    for (auto& surface : m_Surfaces) {
        for (auto& plane : surface.planes) {
            pl_tex_destroy(m_Vulkan->gpu, &plane);
        }
        if (surface.holdSemaphore != VK_NULL_HANDLE) {
            pl_vulkan_sem_destroy(m_Vulkan->gpu, &surface.holdSemaphore);
        }
    }
    if (m_DecodeSemaphore != VK_NULL_HANDLE) {
        pl_vulkan_sem_destroy(m_Vulkan->gpu, &m_DecodeSemaphore);
    }
}

bool PlVkPyroWaveSurfaces::hold(int surface, uint64_t* value)
{
    Surface& s = m_Surfaces[surface];
    for (pl_tex plane : s.planes) {
        pl_vulkan_hold_params params = {};
        params.tex = plane;
        params.layout = VK_IMAGE_LAYOUT_GENERAL;
        params.qf = VK_QUEUE_FAMILY_IGNORED;
        params.semaphore = { s.holdSemaphore, ++s.holdValue };
        if (!pl_vulkan_hold_ex(m_Vulkan->gpu, &params)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: pl_vulkan_hold_ex() failed for surface %d", surface);
            return false;
        }
    }
    // The planes' holds are queued in order, so the last value covers all three
    *value = s.holdValue;
    return true;
}

void PlVkPyroWaveSurfaces::pyroWaveSurface(int index, pyrowave_gpu_buffers* planes,
                                           VkSemaphore* holdSemaphore, uint64_t* holdValue)
{
    *planes = m_Surfaces[index].views;
    *holdSemaphore = m_Surfaces[index].holdSemaphore;
    *holdValue = m_Surfaces[index].holdValue;
}

bool PlVkPyroWaveSurfaces::map(const AVFrame* frame, const PyroWaveFrameRef* ref, pl_frame* mappedFrame)
{
    if (ref->surface < 0 || ref->surface >= (int)m_Surfaces.size()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave frame references unknown surface %d", ref->surface);
        return false;
    }

    // Colour metadata, plane layout and chroma siting from the AVFrame
    pl_frame_from_avframe(mappedFrame, frame);
    if (mappedFrame->num_planes != 3) {
        return false;
    }

    Surface& surface = m_Surfaces[ref->surface];
    for (int plane = 0; plane < 3; plane++) {
        pl_vulkan_release_params params = {};
        params.tex = surface.planes[plane];
        params.layout = VK_IMAGE_LAYOUT_GENERAL;
        params.qf = VK_QUEUE_FAMILY_IGNORED;
        params.semaphore = { m_DecodeSemaphore, ref->decodeFenceValue };
        pl_vulkan_release_ex(m_Vulkan->gpu, &params);

        mappedFrame->planes[plane].texture = surface.planes[plane];
    }

    if (m_TenBit) {
        // PyroWave writes normalized 16-bit samples, not 10 bits in the low bits
        mappedFrame->repr.bits.sample_depth = 16;
        mappedFrame->repr.bits.color_depth = 16;
        mappedFrame->repr.bits.bit_shift = 0;
    }
    return true;
}

void PlVkPyroWaveSurfaces::unmap(PyroWaveFrameRef* ref)
{
    uint64_t value;
    if (hold(ref->surface, &value)) {
        ref->noteRelease(value);
    }
}

uint64_t PlVkPyroWaveSurfaces::waitForDecode(const PyroWaveFrameRef* ref)
{
    VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &m_DecodeSemaphore;
    waitInfo.pValues = &ref->decodeFenceValue;

    const uint64_t startUs = LiGetMicroseconds();
    if (m_WaitSemaphores(m_Vulkan->device, &waitInfo, k_DecodeWaitTimeoutNs) != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave decode semaphore wait failed (target=%llu)",
                     (unsigned long long)ref->decodeFenceValue);
        return 0;
    }
    return LiGetMicroseconds() - startUs;
}
