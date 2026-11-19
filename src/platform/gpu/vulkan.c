/* Vulkan presenter. The few Vulkan declarations it needs are written out here (matching
 * vulkan_core.h) and the loader is opened at run time, so building needs no Vulkan SDK. Each frame
 * is copied into a host-visible staging buffer and transferred into an image of the device's own that keeps the frame
 * (only the rectangles that changed, after the first), which is copied into the swapchain image on the device. The
 * surface is a Win32 one, an Xlib one (Linux), an Android one or a Metal
 * one (macOS, through MoltenVK when it is installed). */
#include "platform/common/platformInternal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#ifdef _WIN32
#define VKAPI __stdcall
#else
#define VKAPI
#endif
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef uint64_t VkSurfaceKHR, VkSwapchainKHR, VkImage, VkBuffer, VkDeviceMemory, VkCommandPool, VkSemaphore, VkFence;
typedef uint32_t VkBool32, VkFlags;
typedef uint64_t VkDeviceSize;
typedef int32_t VkResult;
typedef void(VKAPI* PFN_vkVoidFunction)(void);

enum { VK_OK = 0, VK_SUBOPTIMAL = 1000001003, VK_ERROR_OUT_OF_DATE = -1000001004 };
enum {
    ST_APPLICATION_INFO = 0, ST_INSTANCE_CREATE_INFO = 1, ST_DEVICE_QUEUE_CREATE_INFO = 2, ST_DEVICE_CREATE_INFO = 3,
    ST_SUBMIT_INFO = 4, ST_MEMORY_ALLOCATE_INFO = 5, ST_FENCE_CREATE_INFO = 8, ST_SEMAPHORE_CREATE_INFO = 9,
    ST_BUFFER_CREATE_INFO = 12, ST_IMAGE_CREATE_INFO = 14, ST_COMMAND_POOL_CREATE_INFO = 39, ST_COMMAND_BUFFER_ALLOCATE_INFO = 40,
    ST_COMMAND_BUFFER_BEGIN_INFO = 42, ST_IMAGE_MEMORY_BARRIER = 45, ST_SWAPCHAIN_CREATE_INFO = 1000001000,
    ST_PRESENT_INFO = 1000001001, ST_WIN32_SURFACE_CREATE_INFO = 1000009000, ST_XLIB_SURFACE_CREATE_INFO = 1000004000,
    ST_METAL_SURFACE_CREATE_INFO = 1000217000, ST_ANDROID_SURFACE_CREATE_INFO = 1000008000
};

typedef struct ApplicationInfo { int32_t sType; const void* pNext; const char* pApplicationName; uint32_t applicationVersion; const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion; } ApplicationInfo;
typedef struct InstanceCreateInfo { int32_t sType; const void* pNext; VkFlags flags; const ApplicationInfo* pApplicationInfo; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames; } InstanceCreateInfo;
typedef struct Win32SurfaceCreateInfo { int32_t sType; const void* pNext; VkFlags flags; void* hinstance; void* hwnd; } Win32SurfaceCreateInfo;
typedef struct XlibSurfaceCreateInfo { int32_t sType; const void* pNext; VkFlags flags; void* dpy; unsigned long window; } XlibSurfaceCreateInfo;
typedef struct WaylandSurfaceCreateInfo { int32_t sType; const void* pNext; VkFlags flags; void* display; void* surface; } WaylandSurfaceCreateInfo;
typedef struct MetalSurfaceCreateInfo { int32_t sType; const void* pNext; VkFlags flags; const void* pLayer; } MetalSurfaceCreateInfo;
typedef struct AndroidSurfaceCreateInfo { int32_t sType; const void* pNext; VkFlags flags; void* window; } AndroidSurfaceCreateInfo;
typedef struct ExtensionProperties { char extensionName[256]; uint32_t specVersion; } ExtensionProperties;
typedef struct Extent2D { uint32_t width, height; } Extent2D;
typedef struct Extent3D { uint32_t width, height, depth; } Extent3D;
typedef struct Offset3D { int32_t x, y, z; } Offset3D;
typedef struct QueueFamilyProperties { VkFlags queueFlags; uint32_t queueCount; uint32_t timestampValidBits; Extent3D minImageTransferGranularity; } QueueFamilyProperties;
typedef struct DeviceQueueCreateInfo { int32_t sType; const void* pNext; VkFlags flags; uint32_t queueFamilyIndex; uint32_t queueCount; const float* pQueuePriorities; } DeviceQueueCreateInfo;
typedef struct DeviceCreateInfo { int32_t sType; const void* pNext; VkFlags flags; uint32_t queueCreateInfoCount; const DeviceQueueCreateInfo* pQueueCreateInfos; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames; uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames; const void* pEnabledFeatures; } DeviceCreateInfo;
typedef struct SurfaceCapabilities { uint32_t minImageCount, maxImageCount; Extent2D currentExtent, minImageExtent, maxImageExtent; uint32_t maxImageArrayLayers; VkFlags supportedTransforms; int32_t currentTransform; VkFlags supportedCompositeAlpha; VkFlags supportedUsageFlags; } SurfaceCapabilities;
typedef struct SurfaceFormat { int32_t format; int32_t colorSpace; } SurfaceFormat;
typedef struct SwapchainCreateInfo { int32_t sType; const void* pNext; VkFlags flags; VkSurfaceKHR surface; uint32_t minImageCount; int32_t imageFormat; int32_t imageColorSpace; Extent2D imageExtent; uint32_t imageArrayLayers; VkFlags imageUsage; int32_t imageSharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; int32_t preTransform; int32_t compositeAlpha; int32_t presentMode; VkBool32 clipped; VkSwapchainKHR oldSwapchain; } SwapchainCreateInfo;
typedef struct BufferCreateInfo { int32_t sType; const void* pNext; VkFlags flags; VkDeviceSize size; VkFlags usage; int32_t sharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; } BufferCreateInfo;
typedef struct MemoryRequirements { VkDeviceSize size; VkDeviceSize alignment; uint32_t memoryTypeBits; } MemoryRequirements;
typedef struct MemoryType { VkFlags propertyFlags; uint32_t heapIndex; } MemoryType;
typedef struct MemoryHeap { VkDeviceSize size; VkFlags flags; } MemoryHeap;
typedef struct PhysicalDeviceMemoryProperties { uint32_t memoryTypeCount; MemoryType memoryTypes[32]; uint32_t memoryHeapCount; MemoryHeap memoryHeaps[16]; } PhysicalDeviceMemoryProperties;
typedef struct MemoryAllocateInfo { int32_t sType; const void* pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } MemoryAllocateInfo;
typedef struct CommandPoolCreateInfo { int32_t sType; const void* pNext; VkFlags flags; uint32_t queueFamilyIndex; } CommandPoolCreateInfo;
typedef struct CommandBufferAllocateInfo { int32_t sType; const void* pNext; VkCommandPool commandPool; int32_t level; uint32_t commandBufferCount; } CommandBufferAllocateInfo;
typedef struct CommandBufferBeginInfo { int32_t sType; const void* pNext; VkFlags flags; const void* pInheritanceInfo; } CommandBufferBeginInfo;
typedef struct ImageSubresourceRange { VkFlags aspectMask; uint32_t baseMipLevel, levelCount, baseArrayLayer, layerCount; } ImageSubresourceRange;
typedef struct ImageMemoryBarrier { int32_t sType; const void* pNext; VkFlags srcAccessMask, dstAccessMask; int32_t oldLayout, newLayout; uint32_t srcQueueFamilyIndex, dstQueueFamilyIndex; VkImage image; ImageSubresourceRange subresourceRange; } ImageMemoryBarrier;
typedef struct ImageSubresourceLayers { VkFlags aspectMask; uint32_t mipLevel, baseArrayLayer, layerCount; } ImageSubresourceLayers;
typedef struct BufferImageCopy { VkDeviceSize bufferOffset; uint32_t bufferRowLength, bufferImageHeight; ImageSubresourceLayers imageSubresource; Offset3D imageOffset; Extent3D imageExtent; } BufferImageCopy;
typedef struct ImageCreateInfo { int32_t sType; const void* pNext; VkFlags flags; int32_t imageType; int32_t format; Extent3D extent; uint32_t mipLevels; uint32_t arrayLayers; int32_t samples; int32_t tiling; VkFlags usage; int32_t sharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; int32_t initialLayout; } ImageCreateInfo;
typedef struct ImageCopy { ImageSubresourceLayers srcSubresource; Offset3D srcOffset; ImageSubresourceLayers dstSubresource; Offset3D dstOffset; Extent3D extent; } ImageCopy;
typedef struct SemaphoreCreateInfo { int32_t sType; const void* pNext; VkFlags flags; } SemaphoreCreateInfo;
typedef struct FenceCreateInfo { int32_t sType; const void* pNext; VkFlags flags; } FenceCreateInfo;
typedef struct SubmitInfo { int32_t sType; const void* pNext; uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores; const VkFlags* pWaitDstStageMask; uint32_t commandBufferCount; const VkCommandBuffer* pCommandBuffers; uint32_t signalSemaphoreCount; const VkSemaphore* pSignalSemaphores; } SubmitInfo;
typedef struct PresentInfo { int32_t sType; const void* pNext; uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores; uint32_t swapchainCount; const VkSwapchainKHR* pSwapchains; const uint32_t* pImageIndices; VkResult* pResults; } PresentInfo;

#define FORMAT_B8G8R8A8_UNORM 44
#define FORMAT_R8G8B8A8_UNORM 37
#define QUEUE_FAMILY_IGNORED (~0u)

#define VK_FUNCS(X) \
    X(vkCreateInstance, VkResult, (const InstanceCreateInfo*, const void*, VkInstance*)) \
    X(vkDestroyInstance, void, (VkInstance, const void*)) \
    X(vkEnumeratePhysicalDevices, VkResult, (VkInstance, uint32_t*, VkPhysicalDevice*)) \
    X(vkGetPhysicalDeviceProperties, void, (VkPhysicalDevice, void*)) \
    X(vkGetPhysicalDeviceQueueFamilyProperties, void, (VkPhysicalDevice, uint32_t*, QueueFamilyProperties*)) \
    X(vkGetPhysicalDeviceMemoryProperties, void, (VkPhysicalDevice, PhysicalDeviceMemoryProperties*)) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR, VkResult, (VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32*)) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, SurfaceCapabilities*)) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, uint32_t*, SurfaceFormat*)) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR, VkResult, (VkPhysicalDevice, VkSurfaceKHR, uint32_t*, int32_t*)) \
    X(vkEnumerateDeviceExtensionProperties, VkResult, (VkPhysicalDevice, const char*, uint32_t*, ExtensionProperties*)) \
    X(vkDestroySurfaceKHR, void, (VkInstance, VkSurfaceKHR, const void*)) \
    X(vkCreateDevice, VkResult, (VkPhysicalDevice, const DeviceCreateInfo*, const void*, VkDevice*)) \
    X(vkGetDeviceProcAddr, PFN_vkVoidFunction, (VkDevice, const char*))

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice, void, (VkDevice, const void*)) \
    X(vkGetDeviceQueue, void, (VkDevice, uint32_t, uint32_t, VkQueue*)) \
    X(vkDeviceWaitIdle, VkResult, (VkDevice)) \
    X(vkCreateSwapchainKHR, VkResult, (VkDevice, const SwapchainCreateInfo*, const void*, VkSwapchainKHR*)) \
    X(vkDestroySwapchainKHR, void, (VkDevice, VkSwapchainKHR, const void*)) \
    X(vkGetSwapchainImagesKHR, VkResult, (VkDevice, VkSwapchainKHR, uint32_t*, VkImage*)) \
    X(vkAcquireNextImageKHR, VkResult, (VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)) \
    X(vkQueuePresentKHR, VkResult, (VkQueue, const PresentInfo*)) \
    X(vkCreateBuffer, VkResult, (VkDevice, const BufferCreateInfo*, const void*, VkBuffer*)) \
    X(vkDestroyBuffer, void, (VkDevice, VkBuffer, const void*)) \
    X(vkGetBufferMemoryRequirements, void, (VkDevice, VkBuffer, MemoryRequirements*)) \
    X(vkAllocateMemory, VkResult, (VkDevice, const MemoryAllocateInfo*, const void*, VkDeviceMemory*)) \
    X(vkFreeMemory, void, (VkDevice, VkDeviceMemory, const void*)) \
    X(vkBindBufferMemory, VkResult, (VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize)) \
    X(vkMapMemory, VkResult, (VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkFlags, void**)) \
    X(vkUnmapMemory, void, (VkDevice, VkDeviceMemory)) \
    X(vkCreateCommandPool, VkResult, (VkDevice, const CommandPoolCreateInfo*, const void*, VkCommandPool*)) \
    X(vkDestroyCommandPool, void, (VkDevice, VkCommandPool, const void*)) \
    X(vkAllocateCommandBuffers, VkResult, (VkDevice, const CommandBufferAllocateInfo*, VkCommandBuffer*)) \
    X(vkResetCommandBuffer, VkResult, (VkCommandBuffer, VkFlags)) \
    X(vkBeginCommandBuffer, VkResult, (VkCommandBuffer, const CommandBufferBeginInfo*)) \
    X(vkEndCommandBuffer, VkResult, (VkCommandBuffer)) \
    X(vkCmdPipelineBarrier, void, (VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void*, uint32_t, const void*, uint32_t, const ImageMemoryBarrier*)) \
    X(vkCmdCopyBufferToImage, void, (VkCommandBuffer, VkBuffer, VkImage, int32_t, uint32_t, const BufferImageCopy*)) \
    X(vkCmdCopyImageToBuffer, void, (VkCommandBuffer, VkImage, int32_t, VkBuffer, uint32_t, const BufferImageCopy*)) \
    X(vkCmdCopyImage, void, (VkCommandBuffer, VkImage, int32_t, VkImage, int32_t, uint32_t, const ImageCopy*)) \
    X(vkCreateImage, VkResult, (VkDevice, const ImageCreateInfo*, const void*, VkImage*)) \
    X(vkDestroyImage, void, (VkDevice, VkImage, const void*)) \
    X(vkGetImageMemoryRequirements, void, (VkDevice, VkImage, MemoryRequirements*)) \
    X(vkBindImageMemory, VkResult, (VkDevice, VkImage, VkDeviceMemory, VkDeviceSize)) \
    X(vkQueueSubmit, VkResult, (VkQueue, uint32_t, const SubmitInfo*, VkFence)) \
    X(vkCreateSemaphore, VkResult, (VkDevice, const SemaphoreCreateInfo*, const void*, VkSemaphore*)) \
    X(vkDestroySemaphore, void, (VkDevice, VkSemaphore, const void*)) \
    X(vkCreateFence, VkResult, (VkDevice, const FenceCreateInfo*, const void*, VkFence*)) \
    X(vkDestroyFence, void, (VkDevice, VkFence, const void*)) \
    X(vkWaitForFences, VkResult, (VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)) \
    X(vkResetFences, VkResult, (VkDevice, uint32_t, const VkFence*))

typedef struct VulkanPresenter {
    CscdPresenter base;
#define DECL(name, ret, args) ret(VKAPI* name) args;
    VK_FUNCS(DECL)
    VK_DEVICE_FUNCS(DECL)
#undef DECL
    PFN_vkVoidFunction(VKAPI* getInstanceProcAddr)(VkInstance, const char*);

    void* lib;
    void* native;
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    CSCD_VEC(VkImage) images;
    CSCD_VEC(VkSemaphore) renderDone;
    VkSemaphore acquired;
    VkFence fence;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    void* mapped;
    bool swapRB;       /* an RGBA swapchain (Android has no BGRA one): red and blue change places on the way */
    bool stagingHolds; /* the staging buffer holds the last frame (partial copies are enough) */
    /* The frame, kept on the device: what changed goes up into it, and it is copied into each swapchain image there
     * (a swapchain image keeps nothing from the frames before). 0: none could be made, the whole frame goes up. */
    VkImage frameImage;
    VkDeviceMemory frameMem;
    bool frameHolds;   /* it holds the last frame (and is in TRANSFER_SRC layout) */
    bool check;        /* CASCADE_VULKAN_CHECK: each frame read back from frameImage and compared (slow; for tests) */
    VkBuffer checkBuf;
    VkDeviceMemory checkMem;
    void* checkMapped;
    uint32_t heldW, heldH;  /* (of that frame) */
    VkDeviceSize stagingSize;
    uint32_t extentW, extentH;
    int format;
    int colorSpace;
    int width, height;
    bool needRecreate;
    char description[300];
    const char* failed; /* (what stopped it, said once it falls back) */
} VulkanPresenter;

static CscdBackend vkBackend(const CscdPresenter* p) { (void)p; return CSCD_BACKEND_VULKAN; }
static const char* vkDescription(const CscdPresenter* p) { return ((const VulkanPresenter*)p)->description; }

static bool createSurface(VulkanPresenter* v) {
    typedef VkResult(VKAPI * Create)(VkInstance, const void*, const void*, VkSurfaceKHR*);
    Create create;
#if defined(_WIN32)
    *(void**)&create = (void*)v->getInstanceProcAddr(v->instance, "vkCreateWin32SurfaceKHR");
    Win32SurfaceCreateInfo sci = {ST_WIN32_SURFACE_CREATE_INFO, NULL, 0, (void*)GetModuleHandleA(NULL), v->native};
#elif defined(__ANDROID__)
    *(void**)&create = (void*)v->getInstanceProcAddr(v->instance, "vkCreateAndroidSurfaceKHR");
    AndroidSurfaceCreateInfo sci = {ST_ANDROID_SURFACE_CREATE_INFO, NULL, 0, v->native}; /* (an ANativeWindow*) */
#elif defined(__APPLE__)
    *(void**)&create = (void*)v->getInstanceProcAddr(v->instance, "vkCreateMetalSurfaceEXT");
    MetalSurfaceCreateInfo sci = {ST_METAL_SURFACE_CREATE_INFO, NULL, 0, create ? platformMetalLayer(v->native) : NULL};
    if (!sci.pLayer) return false;
#else
    if (((const CscdWaylandWindow*)v->native)->mark == CSCD_WAYLAND_MARK) {
        const CscdWaylandWindow* ww = (const CscdWaylandWindow*)v->native;
        *(void**)&create = (void*)v->getInstanceProcAddr(v->instance, "vkCreateWaylandSurfaceKHR");
        WaylandSurfaceCreateInfo wci = {1000006000 /* (VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR) */, NULL, 0, ww->display, ww->surface};
        return create && create(v->instance, &wci, NULL, &v->surface) == VK_OK;
    }
    *(void**)&create = (void*)v->getInstanceProcAddr(v->instance, "vkCreateXlibSurfaceKHR");
    const CscdXlibWindow* xw = (const CscdXlibWindow*)v->native;
    XlibSurfaceCreateInfo sci = {ST_XLIB_SURFACE_CREATE_INFO, NULL, 0, xw->display, xw->window};
#endif
    return create && create(v->instance, &sci, NULL, &v->surface) == VK_OK;
}

static uint32_t memoryType(VulkanPresenter* v, uint32_t bits, VkFlags props) {
    PhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    v->vkGetPhysicalDeviceMemoryProperties(v->gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return ~0u;
}

static void destroyStaging(VulkanPresenter* v) {
    if (!v->device) return;
    if (v->mapped) { v->vkUnmapMemory(v->device, v->stagingMem); v->mapped = NULL; }
    if (v->staging) { v->vkDestroyBuffer(v->device, v->staging, NULL); v->staging = 0; }
    if (v->stagingMem) { v->vkFreeMemory(v->device, v->stagingMem, NULL); v->stagingMem = 0; }
    v->stagingSize = 0;
}

static void destroyFrameImage(VulkanPresenter* v) {
    if (!v->device) return;
    if (v->frameImage) { v->vkDestroyImage(v->device, v->frameImage, NULL); v->frameImage = 0; }
    if (v->frameMem) { v->vkFreeMemory(v->device, v->frameMem, NULL); v->frameMem = 0; }
    if (v->checkMapped) { v->vkUnmapMemory(v->device, v->checkMem); v->checkMapped = NULL; }
    if (v->checkBuf) { v->vkDestroyBuffer(v->device, v->checkBuf, NULL); v->checkBuf = 0; }
    if (v->checkMem) { v->vkFreeMemory(v->device, v->checkMem, NULL); v->checkMem = 0; }
    v->frameHolds = false;
}

/* (false: none; frames then go up whole, as before) */
static bool createFrameImage(VulkanPresenter* v, uint32_t w, uint32_t h) {
    destroyFrameImage(v);
    ImageCreateInfo ici = {ST_IMAGE_CREATE_INFO, NULL, 0, 1 /*2D*/, v->format, {w, h, 1}, 1, 1, 1 /*1 sample*/, 0 /*OPTIMAL*/,
                           1 | 2 /*TRANSFER_SRC | TRANSFER_DST*/, 0, 0, NULL, 0 /*UNDEFINED*/};
    if (v->vkCreateImage(v->device, &ici, NULL, &v->frameImage) != VK_OK) { v->frameImage = 0; return false; }
    MemoryRequirements req;
    memset(&req, 0, sizeof req);
    v->vkGetImageMemoryRequirements(v->device, v->frameImage, &req);
    uint32_t type = memoryType(v, req.memoryTypeBits, 1 /*DEVICE_LOCAL*/);
    MemoryAllocateInfo mai = {ST_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (type == ~0u || v->vkAllocateMemory(v->device, &mai, NULL, &v->frameMem) != VK_OK || v->vkBindImageMemory(v->device, v->frameImage, v->frameMem, 0) != VK_OK) {
        destroyFrameImage(v);
        return false;
    }
    if (v->check) { /* (a buffer the frame is read back into) */
        BufferCreateInfo bci = {ST_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)w * h * 4, 2 /*TRANSFER_DST*/, 0, 0, NULL};
        if (v->vkCreateBuffer(v->device, &bci, NULL, &v->checkBuf) == VK_OK) {
            v->vkGetBufferMemoryRequirements(v->device, v->checkBuf, &req);
            type = memoryType(v, req.memoryTypeBits, 2 | 4);
            MemoryAllocateInfo cai = {ST_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
            if (type != ~0u && v->vkAllocateMemory(v->device, &cai, NULL, &v->checkMem) == VK_OK) {
                v->vkBindBufferMemory(v->device, v->checkBuf, v->checkMem, 0);
                v->vkMapMemory(v->device, v->checkMem, 0, (VkDeviceSize)w * h * 4, 0, &v->checkMapped);
            }
        }
    }
    return true;
}

static bool createStaging(VulkanPresenter* v, VkDeviceSize size) {
    v->stagingHolds = false;
    if (v->staging && v->stagingSize >= size) return true;
    destroyStaging(v);
    BufferCreateInfo bci = {ST_BUFFER_CREATE_INFO, NULL, 0, size, 1 /*TRANSFER_SRC*/, 0, 0, NULL};
    if (v->vkCreateBuffer(v->device, &bci, NULL, &v->staging) != VK_OK) return false;
    MemoryRequirements req;
    memset(&req, 0, sizeof req);
    v->vkGetBufferMemoryRequirements(v->device, v->staging, &req);
    uint32_t type = memoryType(v, req.memoryTypeBits, 2 | 4); /* HOST_VISIBLE | HOST_COHERENT */
    if (type == ~0u) return false;
    MemoryAllocateInfo mai = {ST_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (v->vkAllocateMemory(v->device, &mai, NULL, &v->stagingMem) != VK_OK) return false;
    v->vkBindBufferMemory(v->device, v->staging, v->stagingMem, 0);
    if (v->vkMapMemory(v->device, v->stagingMem, 0, size, 0, &v->mapped) != VK_OK) return false;
    v->stagingSize = size;
    return true;
}

#define NOPE(why) do { v->failed = (why); return false; } while (0)

static bool createSwapchain(VulkanPresenter* v) {
    SurfaceCapabilities caps;
    memset(&caps, 0, sizeof caps);
    if (v->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->gpu, v->surface, &caps) != VK_OK) NOPE("the surface's capabilities");
    if (!(caps.supportedUsageFlags & 2)) NOPE("swapchain images cannot be copied to (TRANSFER_DST)");
    uint32_t w = caps.currentExtent.width == 0xFFFFFFFFu ? (uint32_t)v->width : caps.currentExtent.width;
    uint32_t h = caps.currentExtent.height == 0xFFFFFFFFu ? (uint32_t)v->height : caps.currentExtent.height;
    int32_t transform = caps.currentTransform;
#ifdef __ANDROID__
    /* the frame is upright as the window is: no pre-rotation, the compositor turns it with the display;
     * the images are the window's size (a frame shorter than it, above the on-screen keyboard, is
     * copied in at the top) */
    if (caps.supportedTransforms & 1) {
        transform = 1; /* IDENTITY */
        w = (uint32_t)ANativeWindow_getWidth((ANativeWindow*)v->native);
        h = (uint32_t)ANativeWindow_getHeight((ANativeWindow*)v->native);
    }
#endif
    if (w == 0 || h == 0) { v->extentW = v->extentH = 0; return true; } /* minimized */
    uint32_t count = caps.minImageCount > 2u ? caps.minImageCount : 2u;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    uint32_t pn = 0;
    v->vkGetPhysicalDeviceSurfacePresentModesKHR(v->gpu, v->surface, &pn, NULL);
    int32_t* modes = cscdCalloc((size_t)pn + 1, sizeof(int32_t));
    v->vkGetPhysicalDeviceSurfacePresentModesKHR(v->gpu, v->surface, &pn, modes);
    int32_t mode = 2; /* FIFO, always available */
#ifndef __ANDROID__
    for (uint32_t i = 0; i < pn; ++i) if (modes[i] == 1) mode = 1; /* MAILBOX: low latency without tearing */
#endif
    /* (on phones FIFO: presenting waits for the screen's refresh, so a page that animates draws one
     * frame per refresh, not as many as the processor can, which the battery pays for) */
    cscdFree(modes);
    VkSwapchainKHR old = v->swapchain;
    SwapchainCreateInfo sci = {ST_SWAPCHAIN_CREATE_INFO, NULL, 0, v->surface, count, v->format, v->colorSpace, {w, h}, 1,
                               2 /*TRANSFER_DST*/, 0, 0, NULL, transform, 1 /*OPAQUE*/, mode, 1, old};
    if (!(caps.supportedCompositeAlpha & 1)) sci.compositeAlpha = (int32_t)(caps.supportedCompositeAlpha & (0u - caps.supportedCompositeAlpha));
    if (v->vkCreateSwapchainKHR(v->device, &sci, NULL, &v->swapchain) != VK_OK) {
        v->swapchain = old;
        v->failed = "the swapchain";
        return false;
    }
    if (old) v->vkDestroySwapchainKHR(v->device, old, NULL);
    uint32_t n = 0;
    v->vkGetSwapchainImagesKHR(v->device, v->swapchain, &n, NULL);
    cscdVecResize(&v->images, n);
    v->vkGetSwapchainImagesKHR(v->device, v->swapchain, &n, v->images.items);
    for (size_t i = 0; i < v->renderDone.len; ++i) v->vkDestroySemaphore(v->device, v->renderDone.items[i], NULL);
    v->renderDone.len = 0;
    cscdVecResize(&v->renderDone, n);
    SemaphoreCreateInfo semi = {ST_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (size_t i = 0; i < v->renderDone.len; ++i) v->vkCreateSemaphore(v->device, &semi, NULL, &v->renderDone.items[i]);
    v->extentW = w;
    v->extentH = h;
    if (!createStaging(v, (VkDeviceSize)w * h * 4)) NOPE("the staging buffer");
    createFrameImage(v, w, h);
    return true;
}

static bool vkInit(VulkanPresenter* v, void* nativeWindow, int w, int ht) {
    v->native = nativeWindow;
    v->width = w;
    v->height = ht;
    v->format = FORMAT_B8G8R8A8_UNORM;
    v->check = getenv("CASCADE_VULKAN_CHECK") != NULL;
#if defined(_WIN32)
    static const char* const libs[] = {"vulkan-1.dll", NULL};
    const char* exts[] = {"VK_KHR_surface", "VK_KHR_win32_surface", NULL};
#elif defined(__ANDROID__)
    static const char* const libs[] = {"libvulkan.so", NULL};
    const char* exts[] = {"VK_KHR_surface", "VK_KHR_android_surface", NULL};
#elif defined(__APPLE__)
    static const char* const libs[] = {"libvulkan.1.dylib", "libvulkan.dylib", "libMoltenVK.dylib", NULL};
    const char* exts[] = {"VK_KHR_surface", "VK_EXT_metal_surface", "VK_KHR_portability_enumeration"};
#else
    static const char* const libs[] = {"libvulkan.so.1", "libvulkan.so", NULL};
    const char* exts[] = {"VK_KHR_surface", ((const CscdWaylandWindow*)nativeWindow)->mark == CSCD_WAYLAND_MARK ? "VK_KHR_wayland_surface" : "VK_KHR_xlib_surface", NULL};
#endif
    v->lib = platformOpenLibrary(libs);
    if (!v->lib) return false;
    *(void**)&v->getInstanceProcAddr = platformSymbol(v->lib, "vkGetInstanceProcAddr");
    if (!v->getInstanceProcAddr) NOPE("vkGetInstanceProcAddr");
    *(void**)&v->vkCreateInstance = (void*)v->getInstanceProcAddr(NULL, "vkCreateInstance");
    if (!v->vkCreateInstance) NOPE("vkCreateInstance");
    ApplicationInfo app = {ST_APPLICATION_INFO, NULL, "cascade", 1, "cascade", 1, (1u << 22)};
    const uint32_t extCount = exts[2] ? 3 : 2;
    /* ENUMERATE_PORTABILITY: MoltenVK is a "portability" implementation, listed only on request */
    InstanceCreateInfo ici = {ST_INSTANCE_CREATE_INFO, NULL, exts[2] ? 1u : 0u, &app, 0, NULL, extCount, exts};
    if (v->vkCreateInstance(&ici, NULL, &v->instance) != VK_OK) {
        if (extCount == 2) NOPE("the instance");
        ici.flags = 0; /* an older loader, without the portability extension */
        ici.enabledExtensionCount = 2;
        if (v->vkCreateInstance(&ici, NULL, &v->instance) != VK_OK) NOPE("the instance");
    }
    /* (vkCreateInstance stays as it came, asked with no instance: asked with one, a loader may say
     * NULL, as the spec says since 1.2.193 and Android's does) */
#define LOAD(name, ret, args) if (!v->name) *(void**)&v->name = (void*)v->getInstanceProcAddr(v->instance, #name); if (!v->name) NOPE(#name);
    VK_FUNCS(LOAD)
#undef LOAD
    if (!createSurface(v)) NOPE("the window's surface");
    uint32_t n = 0;
    v->vkEnumeratePhysicalDevices(v->instance, &n, NULL);
    if (!n) NOPE("no GPU");
    VkPhysicalDevice* gpus = cscdCalloc(n, sizeof *gpus);
    v->vkEnumeratePhysicalDevices(v->instance, &n, gpus);
    int bestScore = -1;
    for (uint32_t gi = 0; gi < n; ++gi) {
        VkPhysicalDevice g = gpus[gi];
        uint32_t qn = 0;
        v->vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, NULL);
        QueueFamilyProperties* qs = cscdCalloc((size_t)qn + 1, sizeof *qs);
        v->vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs);
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = 0;
            v->vkGetPhysicalDeviceSurfaceSupportKHR(g, i, v->surface, &present);
            if (!present || !(qs[i].queueFlags & 1)) continue;
            /* VkPhysicalDeviceProperties: deviceType at offset 16, deviceName at 20 */
            union { unsigned char bytes[1024]; uint64_t align; } props;
            memset(&props, 0, sizeof props);
            v->vkGetPhysicalDeviceProperties(g, props.bytes);
            int32_t type;
            memcpy(&type, props.bytes + 16, sizeof type);
            int score = type == 2 ? 3 : type == 1 ? 2 : 1; /* discrete, integrated, other */
            if (score > bestScore) {
                bestScore = score;
                v->gpu = g;
                v->family = i;
                snprintf(v->description, sizeof v->description, "Vulkan (%.256s)", (const char*)(props.bytes + 20));
            }
            break;
        }
        cscdFree(qs);
    }
    cscdFree(gpus);
    if (!v->gpu) NOPE("no GPU that presents to the window");
    float prio = 1;
    DeviceQueueCreateInfo qci = {ST_DEVICE_QUEUE_CREATE_INFO, NULL, 0, v->family, 1, &prio};
    /* VK_KHR_portability_subset must be enabled where the device has it (MoltenVK) */
    const char* dext[] = {"VK_KHR_swapchain", "VK_KHR_portability_subset"};
    uint32_t dextCount = 1, en = 0;
    v->vkEnumerateDeviceExtensionProperties(v->gpu, NULL, &en, NULL);
    ExtensionProperties* available = cscdCalloc((size_t)en + 1, sizeof *available);
    v->vkEnumerateDeviceExtensionProperties(v->gpu, NULL, &en, available);
    for (uint32_t i = 0; i < en; ++i) if (!strcmp(available[i].extensionName, dext[1])) dextCount = 2;
    cscdFree(available);
    DeviceCreateInfo dci = {ST_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, dextCount, dext, NULL};
    if (v->vkCreateDevice(v->gpu, &dci, NULL, &v->device) != VK_OK) NOPE("the device");
#define LOADD(name, ret, args) *(void**)&v->name = (void*)v->vkGetDeviceProcAddr(v->device, #name); if (!v->name) NOPE(#name);
    VK_DEVICE_FUNCS(LOADD)
#undef LOADD
    v->vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    CommandPoolCreateInfo cpi = {ST_COMMAND_POOL_CREATE_INFO, NULL, 2, v->family};
    if (v->vkCreateCommandPool(v->device, &cpi, NULL, &v->pool) != VK_OK) NOPE("the command pool");
    CommandBufferAllocateInfo cai = {ST_COMMAND_BUFFER_ALLOCATE_INFO, NULL, v->pool, 0, 1};
    if (v->vkAllocateCommandBuffers(v->device, &cai, &v->cmd) != VK_OK) NOPE("the command buffer");
    SemaphoreCreateInfo semi = {ST_SEMAPHORE_CREATE_INFO, NULL, 0};
    if (v->vkCreateSemaphore(v->device, &semi, NULL, &v->acquired) != VK_OK) NOPE("a semaphore");
    FenceCreateInfo fci = {ST_FENCE_CREATE_INFO, NULL, 1};
    if (v->vkCreateFence(v->device, &fci, NULL, &v->fence) != VK_OK) NOPE("a fence");
    /* surface format: an 8-bit BGRA one, or else RGBA (with red and blue swapped on the way) */
    uint32_t fn = 0;
    v->vkGetPhysicalDeviceSurfaceFormatsKHR(v->gpu, v->surface, &fn, NULL);
    SurfaceFormat* fmts = cscdCalloc((size_t)fn + 1, sizeof *fmts);
    v->vkGetPhysicalDeviceSurfaceFormatsKHR(v->gpu, v->surface, &fn, fmts);
    bool found = false;
    for (uint32_t i = 0; i < fn; ++i)
        if (fmts[i].format == FORMAT_B8G8R8A8_UNORM) {
            v->format = fmts[i].format;
            v->colorSpace = fmts[i].colorSpace;
            found = true;
            break;
        }
    for (uint32_t i = 0; i < fn && !found; ++i)
        if (fmts[i].format == FORMAT_R8G8B8A8_UNORM) {
            v->format = fmts[i].format;
            v->colorSpace = fmts[i].colorSpace;
            v->swapRB = true;
            found = true;
        }
    cscdFree(fmts);
    if (!found) NOPE("no 8-bit BGRA or RGBA surface format");
    return createSwapchain(v);
}

static void vkResize(CscdPresenter* p, int w, int h) {
    VulkanPresenter* v = (VulkanPresenter*)p;
    v->width = w;
    v->height = h;
    v->needRecreate = true;
}

static void copyPixels(const VulkanPresenter* v, uint32_t* dst, const uint32_t* src, uint32_t n) {
    if (!v->swapRB) {
        memcpy(dst, src, (size_t)n * 4);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t s = src[i];
        dst[i] = (s & 0xFF00FF00u) | ((s >> 16) & 0xFFu) | ((s & 0xFFu) << 16);
    }
}

static bool vkPresent(CscdPresenter* p, const CscdImage* frame) {
    VulkanPresenter* v = (VulkanPresenter*)p;
    if (!v->device) return false;
    v->vkWaitForFences(v->device, 1, &v->fence, 1, ~0ull);
    if (v->needRecreate) {
        v->vkDeviceWaitIdle(v->device);
        v->needRecreate = false;
        if (!createSwapchain(v)) return false;
    }
    if (!v->extentW || !v->extentH) return true;
    uint32_t index = 0;
    VkResult r = v->vkAcquireNextImageKHR(v->device, v->swapchain, ~0ull, v->acquired, 0, &index);
    if (r == VK_ERROR_OUT_OF_DATE) { v->needRecreate = true; return true; }
    if (r != VK_OK && r != VK_SUBOPTIMAL) return false;
    /* copy the frame (rows clipped to the swapchain extent) */
    uint32_t cw = v->extentW < (uint32_t)frame->width ? v->extentW : (uint32_t)frame->width;
    uint32_t ch = v->extentH < (uint32_t)frame->height ? v->extentH : (uint32_t)frame->height;
    uint8_t* dst = (uint8_t*)v->mapped;
    enum { kMaxRegions = 32 };
    BufferImageCopy up[kMaxRegions];
    uint32_t ups = 0; /* (0: the whole frame goes up) */
    const bool partial = v->base.partial && v->stagingHolds && v->heldW == (uint32_t)frame->width && v->heldH == (uint32_t)frame->height;
    if (partial && v->frameImage && v->frameHolds && v->base.damageCount <= kMaxRegions)
        for (size_t i = 0; i < v->base.damageCount; ++i) {
            const CscdIRect r = v->base.damage[i];
            const uint32_t x0 = (uint32_t)cscdMaxi(0, r.x), y0 = (uint32_t)cscdMaxi(0, r.y);
            const uint32_t x1 = (uint32_t)cscdMini((int)cw, r.x + r.w), y1 = (uint32_t)cscdMini((int)ch, r.y + r.h);
            if (x1 <= x0 || y1 <= y0) continue;
            BufferImageCopy c = {((VkDeviceSize)y0 * v->extentW + x0) * 4, v->extentW, v->extentH, {1, 0, 0, 1}, {(int32_t)x0, (int32_t)y0, 0}, {x1 - x0, y1 - y0, 1}};
            up[ups++] = c;
        }
    const bool whole = !(partial && v->frameImage && v->frameHolds && v->base.damageCount <= kMaxRegions);
    if (partial) {
        /* only what changed: the staging buffer keeps the rest of the frames before */
        for (size_t i = 0; i < v->base.damageCount; ++i) {
            const CscdIRect r = v->base.damage[i];
            const uint32_t x0 = (uint32_t)cscdMaxi(0, r.x), y0 = (uint32_t)cscdMaxi(0, r.y);
            const uint32_t x1 = (uint32_t)cscdMini((int)cw, r.x + r.w), y1 = (uint32_t)cscdMini((int)ch, r.y + r.h);
            for (uint32_t y = y0; y < y1 && x1 > x0; ++y)
                copyPixels(v, (uint32_t*)(dst + ((size_t)y * v->extentW + x0) * 4), cscdImageRow(frame, (int)y) + x0, x1 - x0);
        }
    } else {
        for (uint32_t y = 0; y < v->extentH; ++y) {
            uint32_t* row = (uint32_t*)(dst + (size_t)y * v->extentW * 4);
            if (y < ch) {
                copyPixels(v, row, cscdImageRow(frame, (int)y), cw);
                for (uint32_t x = cw; x < v->extentW; ++x) row[x] = 0xFFFFFFFFu;
            } else for (uint32_t x = 0; x < v->extentW; ++x) row[x] = 0xFFFFFFFFu;
        }
        v->stagingHolds = true;
        v->heldW = (uint32_t)frame->width;
        v->heldH = (uint32_t)frame->height;
    }
    v->vkResetFences(v->device, 1, &v->fence);
    v->vkResetCommandBuffer(v->cmd, 0);
    CommandBufferBeginInfo bi = {ST_COMMAND_BUFFER_BEGIN_INFO, NULL, 1, NULL};
    v->vkBeginCommandBuffer(v->cmd, &bi);
    VkImage image = v->images.items[index];
    BufferImageCopy region = {0, 0, 0, {1, 0, 0, 1}, {0, 0, 0}, {v->extentW, v->extentH, 1}};
    if (v->frameImage) {
        /* what changed (or all of it) up into the frame kept on the device (TRANSFER_SRC between frames: the copy
         * out of it; UNDEFINED the first time, its contents dropped) */
        if (whole || ups) {
            ImageMemoryBarrier keep = {ST_IMAGE_MEMORY_BARRIER, NULL, 0x800 /*TRANSFER_READ*/, 0x1000, v->frameHolds && !whole ? 6 : 0, 7,
                                       QUEUE_FAMILY_IGNORED, QUEUE_FAMILY_IGNORED, v->frameImage, {1, 0, 1, 0, 1}};
            v->vkCmdPipelineBarrier(v->cmd, 0x1000, 0x1000, 0, 0, NULL, 0, NULL, 1, &keep);
            if (whole) v->vkCmdCopyBufferToImage(v->cmd, v->staging, v->frameImage, 7, 1, &region);
            else v->vkCmdCopyBufferToImage(v->cmd, v->staging, v->frameImage, 7, ups, up);
            ImageMemoryBarrier toSrc = {ST_IMAGE_MEMORY_BARRIER, NULL, 0x1000, 0x800, 7, 6 /*TRANSFER_SRC*/,
                                        QUEUE_FAMILY_IGNORED, QUEUE_FAMILY_IGNORED, v->frameImage, {1, 0, 1, 0, 1}};
            v->vkCmdPipelineBarrier(v->cmd, 0x1000, 0x1000, 0, 0, NULL, 0, NULL, 1, &toSrc);
            v->frameHolds = true;
        }
    }
    ImageMemoryBarrier toDst = {ST_IMAGE_MEMORY_BARRIER, NULL, 0, 0x1000 /*TRANSFER_WRITE*/, 0 /*UNDEFINED*/, 7 /*TRANSFER_DST*/,
                                QUEUE_FAMILY_IGNORED, QUEUE_FAMILY_IGNORED, image, {1, 0, 1, 0, 1}};
    v->vkCmdPipelineBarrier(v->cmd, 0x1000 /*TRANSFER*/, 0x1000, 0, 0, NULL, 0, NULL, 1, &toDst);
    if (v->frameImage) {
        ImageCopy all = {{1, 0, 0, 1}, {0, 0, 0}, {1, 0, 0, 1}, {0, 0, 0}, {v->extentW, v->extentH, 1}};
        v->vkCmdCopyImage(v->cmd, v->frameImage, 6, image, 7, 1, &all);
        if (v->checkMapped) v->vkCmdCopyImageToBuffer(v->cmd, v->frameImage, 6, v->checkBuf, 1, &region);
    } else v->vkCmdCopyBufferToImage(v->cmd, v->staging, image, 7, 1, &region);
    ImageMemoryBarrier toPresent = {ST_IMAGE_MEMORY_BARRIER, NULL, 0x1000, 0, 7, 1000001002 /*PRESENT_SRC*/,
                                    QUEUE_FAMILY_IGNORED, QUEUE_FAMILY_IGNORED, image, {1, 0, 1, 0, 1}};
    v->vkCmdPipelineBarrier(v->cmd, 0x1000, 0x2000 /*BOTTOM_OF_PIPE*/, 0, 0, NULL, 0, NULL, 1, &toPresent);
    v->vkEndCommandBuffer(v->cmd);
    VkFlags waitStage = 0x1000;
    SubmitInfo si = {ST_SUBMIT_INFO, NULL, 1, &v->acquired, &waitStage, 1, &v->cmd, 1, &v->renderDone.items[index]};
    if (v->vkQueueSubmit(v->queue, 1, &si, v->fence) != VK_OK) return false;
    PresentInfo pi = {ST_PRESENT_INFO, NULL, 1, &v->renderDone.items[index], 1, &v->swapchain, &index, NULL};
    r = v->vkQueuePresentKHR(v->queue, &pi);
#ifdef __ANDROID__
    /* (suboptimal there means only "not pre-rotated", which this never is) */
    if (r == VK_ERROR_OUT_OF_DATE) v->needRecreate = true;
#else
    if (r == VK_ERROR_OUT_OF_DATE || r == VK_SUBOPTIMAL) v->needRecreate = true;
#endif
    else if (r != VK_OK) return false;
    if (v->checkMapped) {
        /* (CASCADE_VULKAN_CHECK: the frame the device keeps against the staging buffer, which holds the whole frame) */
        v->vkWaitForFences(v->device, 1, &v->fence, 1, ~0ull);
        const size_t n = (size_t)v->extentW * v->extentH;
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < n; ++i)
            if (((const uint32_t*)v->checkMapped)[i] != ((const uint32_t*)v->mapped)[i] && !bad++) first = i;
        if (bad) fprintf(stderr, "cascade: Vulkan check: %zu pixels differ, the first at %zu,%zu (%u regions sent)\n", bad, first % v->extentW, first / v->extentW, ups);
        else fprintf(stderr, "cascade: Vulkan check: same (%s, %u regions)\n", whole ? "whole" : "partial", ups);
    }
    return true;
}

static void vkDestroy(CscdPresenter* p) {
    VulkanPresenter* v = (VulkanPresenter*)p;
    if (v->device && v->vkResetFences) { /* (a device whose functions all loaded) */
        v->vkDeviceWaitIdle(v->device);
        destroyStaging(v);
        destroyFrameImage(v);
        for (size_t i = 0; i < v->renderDone.len; ++i) v->vkDestroySemaphore(v->device, v->renderDone.items[i], NULL);
        if (v->acquired) v->vkDestroySemaphore(v->device, v->acquired, NULL);
        if (v->fence) v->vkDestroyFence(v->device, v->fence, NULL);
        if (v->pool) v->vkDestroyCommandPool(v->device, v->pool, NULL);
        if (v->swapchain) v->vkDestroySwapchainKHR(v->device, v->swapchain, NULL);
        v->vkDestroyDevice(v->device, NULL);
    }
    if (v->instance) {
        if (v->surface && v->vkDestroySurfaceKHR) v->vkDestroySurfaceKHR(v->instance, v->surface, NULL);
        if (v->vkDestroyInstance) v->vkDestroyInstance(v->instance, NULL);
    }
    platformCloseLibrary(v->lib);
    cscdVecFree(&v->images);
    cscdVecFree(&v->renderDone);
    cscdFree(v);
}

static const CscdPresenterVtbl vkVtbl = {vkBackend, vkPresent, vkResize, vkDescription, vkDestroy};

CscdPresenter* platformVulkanPresenter(void* nativeWindow, int w, int h) {
    VulkanPresenter* v = CSCD_NEW(VulkanPresenter);
    v->base.vt = &vkVtbl;
    if (!vkInit(v, nativeWindow, w, h)) {
        /* (said when there is a Vulkan library and it still does not work here; none at all is quiet) */
        if (v->failed) fprintf(stderr, "cascade: Vulkan does not present here (%s): the next presenter is used\n", v->failed);
        vkDestroy(&v->base);
        return NULL;
    }
    return &v->base;
}
