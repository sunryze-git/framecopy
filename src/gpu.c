#define _GNU_SOURCE
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "gpu.h"
#include "util.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define MAX_SOURCES 8

#define VK(x)                                                                                                          \
    do {                                                                                                               \
        VkResult r_ = (x);                                                                                             \
        if (r_)                                                                                                        \
            die("%s failed: %d", #x, r_);                                                                              \
    } while (0)

struct source {
    uint64_t ino;
    struct fb_info info;
    VkImage image;
    VkDeviceMemory memory;
};

static struct {
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    VkCommandBuffer cb;
    VkFence fence;
    VkImage dst;
    VkBuffer buf;
    void *mapped;
    uint32_t out_w, out_h;
    PFN_vkGetMemoryFdPropertiesKHR get_fd_props;
} vk;

static struct source sources[MAX_SOURCES];
static unsigned next_evict;

static uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;

    vkGetPhysicalDeviceMemoryProperties(vk.pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    die("no suitable memory type");
}

static VkDeviceMemory alloc_memory(VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = find_memory_type(req.memoryTypeBits, want)};
    VkDeviceMemory mem;

    VK(vkAllocateMemory(vk.dev, &info, NULL, &mem));
    return mem;
}

void gpu_init(uint32_t out_w, uint32_t out_h)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    uint32_t n = 1;

    vk.out_w = out_w;
    vk.out_h = out_h;

    VK(vkCreateInstance(&ici, NULL, &inst));
    VkResult r = vkEnumeratePhysicalDevices(inst, &n, &vk.pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0)
        die("no Vulkan device");

    VkQueueFamilyProperties qp[16];
    uint32_t nq = 16;
    uint32_t qf = 0;

    vkGetPhysicalDeviceQueueFamilyProperties(vk.pd, &nq, qp);
    while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        qf++;
    if (qf == nq)
        die("no graphics queue");

    const char *exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                          VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME};
    float prio = 1;
    VkDeviceQueueCreateInfo dq = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = qf,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &dq,
                              .enabledExtensionCount = 3,
                              .ppEnabledExtensionNames = exts};
    VK(vkCreateDevice(vk.pd, &dci, NULL, &vk.dev));
    vkGetDeviceQueue(vk.dev, qf, 0, &vk.queue);
    vk.get_fd_props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(vk.dev, "vkGetMemoryFdPropertiesKHR");

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = qf};
    VkCommandPool pool;
    VK(vkCreateCommandPool(vk.dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VK(vkAllocateCommandBuffers(vk.dev, &cai, &vk.cb));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(vk.dev, &fci, NULL, &vk.fence));

    VkImageCreateInfo dsti = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = VK_FORMAT_B8G8R8A8_UNORM,
                              .extent = {out_w, out_h, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements req;
    VK(vkCreateImage(vk.dev, &dsti, NULL, &vk.dst));
    vkGetImageMemoryRequirements(vk.dev, vk.dst, &req);
    VK(vkBindImageMemory(vk.dev, vk.dst, alloc_memory(req, 0), 0));

    VkDeviceSize bytes = (VkDeviceSize)out_w * out_h * 4;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = bytes,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK(vkCreateBuffer(vk.dev, &bci, NULL, &vk.buf));
    vkGetBufferMemoryRequirements(vk.dev, vk.buf, &req);
    VkDeviceMemory bmem = alloc_memory(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkBindBufferMemory(vk.dev, vk.buf, bmem, 0));
    VK(vkMapMemory(vk.dev, bmem, 0, bytes, 0, &vk.mapped));
}

static struct source *import_source(const struct fb_info *info, int fd, uint64_t ino)
{
    struct source *s = &sources[next_evict];

    if (info->fourcc != FOURCC('X', 'R', '2', '4') && info->fourcc != FOURCC('A', 'R', '2', '4'))
        die("unsupported framebuffer format 0x%x", info->fourcc);

    next_evict = (next_evict + 1) % MAX_SOURCES;
    if (s->image) {
        vkDestroyImage(vk.dev, s->image, NULL);
        vkFreeMemory(vk.dev, s->memory, NULL);
    }

    VkSubresourceLayout plane = {.offset = info->offset, .rowPitch = info->pitch};
    VkImageDrmFormatModifierExplicitCreateInfoEXT drm = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = info->modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane};
    VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .pNext = &drm,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .pNext = &ext,
                             .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_B8G8R8A8_UNORM,
                             .extent = {info->width, info->height, 1},
                             .mipLevels = 1,
                             .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkMemoryRequirements req;
    VK(vkCreateImage(vk.dev, &ici, NULL, &s->image));
    vkGetImageMemoryRequirements(vk.dev, s->image, &req);
    if (req.size > info->size)
        die("framebuffer is %llu bytes, the image needs %llu", (unsigned long long)info->size,
            (unsigned long long)req.size);

    VkMemoryFdPropertiesKHR props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VK(vk.get_fd_props(vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &props));
    uint32_t bits = req.memoryTypeBits & props.memoryTypeBits;
    if (!bits)
        die("no memory type fits the framebuffer");

    int owned = dup(fd);
    if (owned < 0)
        die("dup: %s", strerror(errno));
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = s->image};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .pNext = &ded,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   .fd = owned};
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .pNext = &imp,
                                .allocationSize = req.size,
                                .memoryTypeIndex = __builtin_ctz(bits)};
    VkResult r = vkAllocateMemory(vk.dev, &mai, NULL, &s->memory);
    if (r) {
        close(owned);
        die("vkAllocateMemory failed: %d", r);
    }
    VK(vkBindImageMemory(vk.dev, s->image, s->memory, 0));

    s->ino = ino;
    s->info = *info;
    return s;
}

static struct source *find_source(const struct fb_info *info, int fd)
{
    struct stat st;

    if (fstat(fd, &st))
        die("fstat: %s", strerror(errno));

    for (int i = 0; i < MAX_SOURCES; i++) {
        struct source *s = &sources[i];

        if (s->image && s->ino == st.st_ino && s->info.width == info->width && s->info.height == info->height &&
            s->info.pitch == info->pitch && s->info.offset == info->offset && s->info.modifier == info->modifier)
            return s;
    }
    return import_source(info, fd, st.st_ino);
}

static void image_barrier(VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = src,
                              .dstAccessMask = dst,
                              .oldLayout = from,
                              .newLayout = to,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = vk.dst,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

    vkCmdPipelineBarrier(vk.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                         &b);
}

/* Each eye is one half of the panel image, rotated 180 degrees. */
static void blit_eye(const struct source *s, int eye, int32_t x0, int32_t x1)
{
    int32_t half = s->info.width / 2;
    VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{eye * half, 0, 0}, {(eye + 1) * half, s->info.height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .dstOffsets = {{x1, vk.out_h, 0}, {x0, 0, 1}}};

    vkCmdBlitImage(vk.cb, s->image, VK_IMAGE_LAYOUT_GENERAL, vk.dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
}

const void *gpu_capture(const struct fb_info *info, int fd, enum eye eye)
{
    const struct source *s = find_source(info, fd);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

    VK(vkResetCommandBuffer(vk.cb, 0));
    VK(vkBeginCommandBuffer(vk.cb, &bi));
    image_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);

    if (eye == EYE_BOTH) {
        blit_eye(s, 0, 0, vk.out_w / 2);
        blit_eye(s, 1, vk.out_w / 2, vk.out_w);
    } else {
        blit_eye(s, eye == EYE_RIGHT, 0, vk.out_w);
    }

    image_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {vk.out_w, vk.out_h, 1}};
    vkCmdCopyImageToBuffer(vk.cb, vk.dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.buf, 1, &copy);
    VK(vkEndCommandBuffer(vk.cb));

    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &vk.cb};
    VK(vkQueueSubmit(vk.queue, 1, &si, vk.fence));
    VK(vkWaitForFences(vk.dev, 1, &vk.fence, VK_TRUE, UINT64_MAX));
    VK(vkResetFences(vk.dev, 1, &vk.fence));
    return vk.mapped;
}
