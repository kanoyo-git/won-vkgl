#include <stdlib.h>
#include "vkgl.h"

#define MAX_SWAP_IMAGES 8
#define COLOR_FORMAT VK_FORMAT_R8G8B8A8_UNORM

#define X(f) PFN_##f f;
VK_FUNCS(X)
VK_DEVICE_FUNCS(X)
#undef X

struct vk_core vkc;
static PFN_vkGetInstanceProcAddr get_instance_proc;
static VkInstance instance;
static VkPhysicalDevice gpu;
static VkPhysicalDeviceMemoryProperties mem_props;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static VkFormat depth_format;
static char device_name[256];

static HWND window;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkExtent2D swap_extent;
static uint32_t swap_count;
static VkImage swap_images[MAX_SWAP_IMAGES];
static VkSemaphore swap_done[MAX_SWAP_IMAGES];
static int swap_interval;
static volatile BOOL swap_dirty;
static HMODULE vk_loader;
static PFN_vkQueuePresentKHR driver_present;
static int swap_client_w, swap_client_h;

struct image {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
};
static struct image color, depth;
static VkImageLayout color_layout;

static struct image resolved;
static VkRenderPass render_pass;
static VkFramebuffer framebuffer;
static int bb_width, bb_height;

static struct frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkCommandBuffer pre;
    BOOL pre_open;
    VkFence done;
    VkSemaphore acquired;
} frames[FRAMES_IN_FLIGHT];
static int frame_index;
static BOOL recording, in_pass;
static unsigned record_serial;

static CRITICAL_SECTION queue_lock;

static BOOL present_thread_on;
static struct image staging[FRAMES_IN_FLIGHT];
static struct present_job {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence done;
    VkSemaphore rendered, acquired;
    BOOL pending;
} jobs[FRAMES_IN_FLIGHT];
static CRITICAL_SECTION job_lock;
static CONDITION_VARIABLE job_posted, job_finished;
static int job_ring[FRAMES_IN_FLIGHT], job_head, job_tail;

static BOOL present_wait_on;
static PFN_vkWaitForPresentKHR wait_for_present;
static CRITICAL_SECTION swap_lock;
static volatile LONG present_id;
static LONGLONG present_qpc[64];
static volatile LONG lat_n, lat_sum_us, lat_max_us;

static void check(VkResult r, const char *what)
{
    if (r != VK_SUCCESS)
        vkgl_fatal("%s failed: VkResult %d", what, r);
}

static void load_instance(void)
{
    static const char *const exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };
    HMODULE loader = vk_loader = LoadLibraryA("vulkan-1.dll");
    PFN_vkCreateInstance create_instance;
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pEngineName = "vkgl",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = exts,
    };

    if (!loader)
        vkgl_fatal("cannot load vulkan-1.dll (is a Vulkan driver installed?)");
    get_instance_proc = (PFN_vkGetInstanceProcAddr)(void *)
        GetProcAddress(loader, "vkGetInstanceProcAddr");
    if (!get_instance_proc)
        vkgl_fatal("vulkan-1.dll has no vkGetInstanceProcAddr");
    create_instance = (PFN_vkCreateInstance)get_instance_proc(NULL, "vkCreateInstance");
    check(create_instance(&info, NULL, &instance), "vkCreateInstance");
    vkgl_capture_report(get_instance_proc);
#define X(f) \
    if (!(f = (PFN_##f)get_instance_proc(instance, #f))) \
        vkgl_fatal("missing Vulkan function %s", #f);
    VK_FUNCS(X)
#undef X
}

static void pick_gpu(void)
{
    VkPhysicalDevice gpus[16];
    uint32_t count = 16, best = 0;
    int best_score = -1;
    const char *env = getenv("VKGL_GPU");

    vkEnumeratePhysicalDevices(instance, &count, gpus);
    if (!count)
        vkgl_fatal("no Vulkan devices");
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        int score;

        vkGetPhysicalDeviceProperties(gpus[i], &p);
        score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        vkgl_log("gpu %u: %s (type %d, api %u.%u)", i, p.deviceName, p.deviceType,
                 VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
        if (env ? (uint32_t)atoi(env) == i : score > best_score) {
            best = i;
            best_score = score;
        }
    }
    gpu = gpus[best];
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpu, &p);
        lstrcpynA(device_name, p.deviceName, sizeof(device_name));
        vkc.props = p;
    }
    vkc.gpu = gpu;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mem_props);
}

static void pick_samples(void)
{
    VkSampleCountFlags have = vkc.props.limits.framebufferColorSampleCounts &
                              vkc.props.limits.framebufferDepthSampleCounts;
    const char *env = getenv("VKGL_MSAA");
    unsigned want = env ? (unsigned)atoi(env) : 1;

    vkc.samples = VK_SAMPLE_COUNT_1_BIT;
    for (unsigned s = 2; s <= 64 && s <= want; s <<= 1)
        if (have & s)
            vkc.samples = (VkSampleCountFlagBits)s;
    if (vkc.samples != VK_SAMPLE_COUNT_1_BIT)
        vkgl_log("MSAA %ux", (unsigned)vkc.samples);
}

static void start_present_thread(void);
static void drain_presents(void);

static void queue_submit(const VkSubmitInfo *info, VkFence fence)
{
    EnterCriticalSection(&queue_lock);
    check(vkQueueSubmit(queue, 1, info, fence), "vkQueueSubmit");
    LeaveCriticalSection(&queue_lock);
}

static DWORD WINAPI latency_thread(void *arg)
{
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    LONG next = 1;
    LARGE_INTEGER now, freq;

    (void)arg;
    QueryPerformanceFrequency(&freq);
    for (;;) {
        VkResult r;
        EnterCriticalSection(&swap_lock);
        if (swapchain != sc) {
            sc = swapchain;
            next = present_id + 1;
        }
        if (!sc || present_id < next) {
            LeaveCriticalSection(&swap_lock);
            Sleep(1);
            continue;
        }
        if (present_id - next > 16)
            next = present_id;
        r = wait_for_present(device, sc, (uint64_t)next, 20 * 1000000ull);
        if (r == VK_SUCCESS) {
            LONG us;
            QueryPerformanceCounter(&now);
            us = (LONG)((now.QuadPart - present_qpc[next & 63]) * 1000000 / freq.QuadPart);
            InterlockedIncrement(&lat_n);
            InterlockedExchangeAdd(&lat_sum_us, us);
            if (us > lat_max_us)
                lat_max_us = us;
            next++;
        } else if (r != VK_TIMEOUT) {
            next = present_id + 1;
        }
        LeaveCriticalSection(&swap_lock);
    }
    return 0;
}

static void start_latency_thread(void)
{
    wait_for_present = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(device, "vkWaitForPresentKHR");
    if (!wait_for_present) {
        present_wait_on = FALSE;
        return;
    }
    InitializeCriticalSection(&swap_lock);
    CreateThread(NULL, 0, latency_thread, NULL, 0, NULL);
    vkgl_log("measuring present-to-display latency (VK_KHR_present_wait)");
}

static void create_device(void)
{
    const char *exts[4] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    };
    VkPhysicalDevicePresentWaitFeaturesKHR wait_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
    };
    VkPhysicalDevicePresentIdFeaturesKHR id_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
        .pNext = &wait_features,
    };
    BOOL has_id = FALSE, has_wait = FALSE;
    PFN_vkGetPhysicalDeviceFeatures get_features =
        (PFN_vkGetPhysicalDeviceFeatures)get_instance_proc(instance, "vkGetPhysicalDeviceFeatures");
    VkPhysicalDeviceFeatures have;
    static const VkFormat depth_formats[] = {
        VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT,
    };
    VkQueueFamilyProperties families[16];
    uint32_t count = 16;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qinfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qinfo,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = exts,
        .pEnabledFeatures = &vkc.features,
    };

    get_features(gpu, &have);
    vkc.features.fillModeNonSolid = have.fillModeNonSolid;
    vkc.features.samplerAnisotropy = have.samplerAnisotropy;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            queue_family = i;
            break;
        }
    }
    if (queue_family == UINT32_MAX)
        vkgl_fatal("%s has no queue that can draw and present", device_name);
    qinfo.queueFamilyIndex = queue_family;
    {
        static const char *const timing[] = {
            "VK_KHR_present_id", "VK_KHR_present_wait", "VK_KHR_present_id2",
            "VK_KHR_present_wait2", "VK_GOOGLE_display_timing", "VK_EXT_present_timing",
            "VK_NV_low_latency2", "VK_EXT_swapchain_maintenance1",
        };
        VkExtensionProperties props[512];
        uint32_t n = 512;
        char line[512] = "";
        vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, props);
        for (int t = 0; t < 8; t++)
            for (uint32_t i = 0; i < n; i++)
                if (!strcmp(props[i].extensionName, timing[t])) {
                    lstrcatA(line, " ");
                    lstrcatA(line, timing[t]);
                    has_id |= t == 0;
                    has_wait |= t == 1;
                }
        vkgl_log("timing extensions:%s", line[0] ? line : " none");
    }
    if (has_id && has_wait && GetEnvironmentVariableA("VKGL_STATS", NULL, 0)) {
        PFN_vkGetPhysicalDeviceFeatures2 get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)
            get_instance_proc(instance, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceFeatures2 f2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &id_features,
        };
        if (get_features2)
            get_features2(gpu, &f2);
        if (id_features.presentId && wait_features.presentWait) {
            exts[info.enabledExtensionCount++] = VK_KHR_PRESENT_ID_EXTENSION_NAME;
            exts[info.enabledExtensionCount++] = VK_KHR_PRESENT_WAIT_EXTENSION_NAME;
            info.pNext = &id_features;
            present_wait_on = TRUE;
        }
    }
    check(vkCreateDevice(gpu, &info, NULL, &device), "vkCreateDevice");
#define X(f) \
    if (!(f = (PFN_##f)vkGetDeviceProcAddr(device, #f))) \
        vkgl_fatal("missing Vulkan function %s", #f);
    VK_DEVICE_FUNCS(X)
#undef X

    driver_present = vkQueuePresentKHR;
#define X(f) \
    { PFN_##f e = (PFN_##f)(void *)GetProcAddress(vk_loader, #f); if (e) f = e; }
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)
#undef X
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    vkc.device = device;
    if (present_wait_on)
        start_latency_thread();

    depth_format = VK_FORMAT_UNDEFINED;
    for (int i = 0; i < 3 && !depth_format; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu, depth_formats[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            depth_format = depth_formats[i];
    }
    if (!depth_format)
        vkgl_fatal("no usable depth format");
    pick_samples();

    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkCommandPoolCreateInfo pinfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family,
        };
        VkCommandBufferAllocateInfo ainfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkFenceCreateInfo finfo = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        VkSemaphoreCreateInfo sinfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        check(vkCreateCommandPool(device, &pinfo, NULL, &frames[i].pool), "vkCreateCommandPool");
        ainfo.commandPool = frames[i].pool;
        check(vkAllocateCommandBuffers(device, &ainfo, &frames[i].cmd), "vkAllocateCommandBuffers");
        check(vkAllocateCommandBuffers(device, &ainfo, &frames[i].pre), "vkAllocateCommandBuffers");
        check(vkCreateFence(device, &finfo, NULL, &frames[i].done), "vkCreateFence");
        check(vkCreateSemaphore(device, &sinfo, NULL, &frames[i].acquired), "vkCreateSemaphore");
    }
    InitializeCriticalSection(&queue_lock);
    present_thread_on = vkgl_ini_int("render", "present_thread", 1);
    if (present_thread_on)
        start_present_thread();
    vkgl_log("device: %s, queue family %u, depth format %d%s", device_name,
             queue_family, depth_format, present_thread_on ? ", presents from a thread of their own" : "");
}

uint32_t vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    vkgl_fatal("no memory type 0x%x for bits 0x%x", want, bits);
}

static void create_image(struct image *img, VkFormat format, VkImageUsageFlags usage,
                         VkImageAspectFlags aspect, VkSampleCountFlagBits samples)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { bb_width, bb_height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImageViewCreateInfo vinfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ainfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };

    check(vkCreateImage(device, &info, NULL, &img->image), "vkCreateImage");
    vkGetImageMemoryRequirements(device, img->image, &req);
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &ainfo, NULL, &img->memory), "vkAllocateMemory");
    check(vkBindImageMemory(device, img->image, img->memory, 0), "vkBindImageMemory");
    vinfo.image = img->image;
    check(vkCreateImageView(device, &vinfo, NULL, &img->view), "vkCreateImageView");
}

static void destroy_image(struct image *img)
{
    if (!img->image)
        return;
    vkDestroyImageView(device, img->view, NULL);
    vkDestroyImage(device, img->image, NULL);
    vkFreeMemory(device, img->memory, NULL);
    img->image = VK_NULL_HANDLE;
}

static void create_render_pass(void)
{

    VkAttachmentDescription att[2] = {
        {
            .format = COLOR_FORMAT,
            .samples = vkc.samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        },
        {
            .format = depth_format,
            .samples = vkc.samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        },
    };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &cref,
        .pDepthStencilAttachment = &dref,
    };
    VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = att,
        .subpassCount = 1,
        .pSubpasses = &sub,
    };

    check(vkCreateRenderPass(device, &info, NULL, &render_pass), "vkCreateRenderPass");
    vkc.render_pass = render_pass;
}

static void barrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                    VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                    VkPipelineStageFlags dst_stage, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void begin_recording(void);
static void submit_and_wait(void);

static void create_backbuffer(int w, int h)
{
    VkImageAspectFlags daspect = VK_IMAGE_ASPECT_DEPTH_BIT |
        (depth_format == VK_FORMAT_D32_SFLOAT ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageView views[2];
    VkFramebufferCreateInfo finfo = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = views,
        .layers = 1,
    };
    VkClearColorValue black = { { 0, 0, 0, 1 } };
    VkClearDepthStencilValue one = { 1.0f, 0 };
    VkImageSubresourceRange crange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresourceRange drange = { daspect, 0, 1, 0, 1 };
    VkCommandBuffer cmd;

    bb_width = w;
    bb_height = h;
    create_image(&color, COLOR_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT, vkc.samples);
    create_image(&depth, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT, daspect, vkc.samples);
    if (vkc.samples != VK_SAMPLE_COUNT_1_BIT)
        create_image(&resolved, COLOR_FORMAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_SAMPLE_COUNT_1_BIT);
    for (int i = 0; i < FRAMES_IN_FLIGHT && present_thread_on; i++)
        create_image(&staging[i], COLOR_FORMAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT);
    views[0] = color.view;
    views[1] = depth.view;
    finfo.renderPass = render_pass;
    finfo.width = w;
    finfo.height = h;
    check(vkCreateFramebuffer(device, &finfo, NULL, &framebuffer), "vkCreateFramebuffer");

    begin_recording();
    cmd = frames[frame_index].cmd;
    barrier(cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    barrier(cmd, depth.image, daspect, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &crange);
    vkCmdClearDepthStencilImage(cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &one, 1, &drange);
    barrier(cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(cmd, depth.image, daspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    submit_and_wait();
    vkgl_log("backbuffer %dx%d", w, h);
}

static void destroy_backbuffer(void)
{
    if (framebuffer)
        vkDestroyFramebuffer(device, framebuffer, NULL);
    framebuffer = VK_NULL_HANDLE;
    destroy_image(&color);
    destroy_image(&depth);
    destroy_image(&resolved);
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++)
        destroy_image(&staging[i]);
}

static void client_size(int *w, int *h)
{
    RECT r = { 0 };

    GetClientRect(window, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

static VkPresentModeKHR pick_present_mode(void)
{
    VkPresentModeKHR modes[8], want[2];
    uint32_t count = 8;

    if (swap_interval)
        return VK_PRESENT_MODE_FIFO_KHR;
    want[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    want[1] = VK_PRESENT_MODE_MAILBOX_KHR;
    vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &count, modes);
    {
        static BOOL logged;
        char line[64] = "";
        if (!logged) {
            logged = TRUE;
            for (uint32_t i = 0; i < count; i++)
                wsprintfA(line + lstrlenA(line), " %d", (int)modes[i]);
            vkgl_log("present modes:%s", line);
        }
    }
    for (int w = 0; w < 2; w++)
        for (uint32_t i = 0; i < count; i++)
            if (modes[i] == want[w])
                return want[w];
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[32], fmt;
    uint32_t nformats = 32;
    VkSwapchainKHR old = swapchain;
    VkSwapchainCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .clipped = VK_TRUE,
    };
    VkSemaphoreCreateInfo sinfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    int w, h;

    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        vkgl_fatal("swapchain images cannot be blit targets");

    info.imageUsage |= caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &nformats, formats);
    fmt = formats[0];
    for (uint32_t i = 0; i < nformats; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            fmt = formats[i];
            break;
        }

    client_size(&w, &h);
    swap_client_w = w;
    swap_client_h = h;
    if (caps.currentExtent.width != UINT32_MAX) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    }
    if (w <= 0 || h <= 0) {

        swap_dirty = TRUE;
        return;
    }
    swap_extent.width = w;
    swap_extent.height = h;
    info.surface = surface;
    vkgl_log("surface: images %u..%u", caps.minImageCount, caps.maxImageCount);
    info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && info.minImageCount > caps.maxImageCount)
        info.minImageCount = caps.maxImageCount;
    info.imageFormat = fmt.format;
    info.imageColorSpace = fmt.colorSpace;
    info.imageExtent = swap_extent;
    info.preTransform = caps.currentTransform;
    info.presentMode = pick_present_mode();
    info.oldSwapchain = old;
    if (present_wait_on)
        EnterCriticalSection(&swap_lock);
    check(vkCreateSwapchainKHR(device, &info, NULL, &swapchain), "vkCreateSwapchainKHR");
    if (old)
        vkDestroySwapchainKHR(device, old, NULL);
    present_id = 0;
    if (present_wait_on)
        LeaveCriticalSection(&swap_lock);

    for (uint32_t i = 0; i < swap_count; i++)
        vkDestroySemaphore(device, swap_done[i], NULL);
    swap_count = MAX_SWAP_IMAGES;
    vkGetSwapchainImagesKHR(device, swapchain, &swap_count, swap_images);
    for (uint32_t i = 0; i < swap_count; i++)
        check(vkCreateSemaphore(device, &sinfo, NULL, &swap_done[i]), "vkCreateSemaphore");
    swap_dirty = FALSE;
    vkgl_log("swapchain %ux%u, %u images, format %d, present mode %d, usage 0x%x",
             swap_extent.width, swap_extent.height, swap_count, fmt.format,
             info.presentMode, info.imageUsage);
}

void vkgl_swapchain_renew(void)
{
    if (swapchain)
        swap_dirty = TRUE;
}

void *vkgl_vk_export(const char *name)
{
    return vk_loader ? (void *)GetProcAddress(vk_loader, name) : NULL;
}

void *vkgl_vk_driver_present(void)
{
    return (void *)driver_present;
}

static void destroy_surface(void)
{
    for (uint32_t i = 0; i < swap_count; i++)
        vkDestroySemaphore(device, swap_done[i], NULL);
    swap_count = 0;
    if (present_wait_on)
        EnterCriticalSection(&swap_lock);
    if (swapchain)
        vkDestroySwapchainKHR(device, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
    if (present_wait_on)
        LeaveCriticalSection(&swap_lock);
    if (surface)
        vkDestroySurfaceKHR(instance, surface, NULL);
    surface = VK_NULL_HANDLE;
}

void vk_bind_window(HWND wnd)
{
    VkWin32SurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = (HINSTANCE)GetWindowLongPtrW(wnd, GWLP_HINSTANCE),
        .hwnd = wnd,
    };
    int w, h;

    if (wnd == window && surface)
        return;
    if (!instance) {
        load_instance();
        pick_gpu();
    }
    if (device) {
        vk_wait_idle();
        destroy_backbuffer();
        destroy_surface();
    }
    window = wnd;
    check(vkCreateWin32SurfaceKHR(instance, &info, NULL, &surface), "vkCreateWin32SurfaceKHR");
    if (!device) {
        create_device();
        create_render_pass();
        render_init();
        texture_init();
    } else {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, queue_family, surface, &ok);
        if (!ok)
            vkgl_fatal("new window cannot be presented from queue family %u", queue_family);
    }
    create_swapchain();
    client_size(&w, &h);
    create_backbuffer(w > 0 ? w : 1, h > 0 ? h : 1);
    vkgl_log("bound to window %p", (void *)wnd);
}

void vk_release_window(void)
{
    if (!device || !window)
        return;
    vk_wait_idle();
    destroy_backbuffer();
    destroy_surface();
    window = NULL;
    vkgl_log("released window");
}

static int stats_on = -1, stats_window;
static LONGLONG stat_wait, stat_present, stat_last_frame;
static unsigned stat_frames;

static void stats_time(LONGLONG *mark, LONGLONG *acc)
{
    LARGE_INTEGER now;

    if (!stats_on)
        return;
    QueryPerformanceCounter(&now);
    if (acc)
        *acc += now.QuadPart - *mark;
    *mark = now.QuadPart;
}

static void stats_frame(void)
{
    LARGE_INTEGER now, freq;
    double ms;

    if (stats_on < 0) {
        char buf[8] = "";
        stats_on = GetEnvironmentVariableA("VKGL_STATS", buf, sizeof(buf)) > 0;
        stats_window = atoi(buf) > 1 ? atoi(buf) : 1000;
    }
    if (!stats_on)
        return;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    {

        static LONGLONG prev;
        static struct vkgl_stats at;
        double dt = prev ? (now.QuadPart - prev) * 1000.0 / freq.QuadPart : 0;
        double avg = stat_frames && stat_last_frame ? (prev - stat_last_frame) * 1000.0 / freq.QuadPart / stat_frames : 0;
        if (avg > 0 && dt > 3 * avg && dt > 3.0)
            vkgl_log("stats: long frame %.2f ms (window %.2f): %u draws, %u uploads (%u KB), %u new images, "
                     "%u render passes, %u texture copies", dt, avg, vkgl_stats.draws - at.draws,
                     vkgl_stats.uploads - at.uploads, vkgl_stats.upload_kb - at.upload_kb,
                     vkgl_stats.images - at.images, vkgl_stats.passes - at.passes,
                     vkgl_stats.copies - at.copies);
        prev = now.QuadPart;
        at = vkgl_stats;
    }
    if (++stat_frames < stats_window)
        return;
    ms = 1000.0 / freq.QuadPart / stat_frames;
    if (stat_last_frame)
        vkgl_log("stats: %.3f ms/frame (fence wait %.3f, present %.3f), per frame: "
                 "%u draws, %u batches, %u pipeline binds, %u render passes, %u uploads (%u KB), "
                 "%u texture copies",
                 (now.QuadPart - stat_last_frame) * ms, stat_wait * ms, stat_present * ms,
                 vkgl_stats.draws / stat_frames, vkgl_stats.batches / stat_frames,
                 vkgl_stats.pipeline_binds / stat_frames, vkgl_stats.passes / stat_frames,
                 vkgl_stats.uploads / stat_frames, vkgl_stats.upload_kb / stat_frames,
                 vkgl_stats.copies / stat_frames);
    if (present_wait_on) {
        LONG n = InterlockedExchange(&lat_n, 0), sum = InterlockedExchange(&lat_sum_us, 0);
        LONG max = InterlockedExchange(&lat_max_us, 0);
        if (n)
            vkgl_log("stats: present to display %.2f ms on average, %.2f max (%ld presents)",
                     sum / 1000.0 / n, max / 1000.0, n);
    }
    stat_last_frame = now.QuadPart;
    stat_frames = 0;
    stat_wait = stat_present = 0;
    memset(&vkgl_stats, 0, sizeof(vkgl_stats));
}

static void begin_recording(void)
{
    struct frame *f = &frames[frame_index];
    VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    LONGLONG mark = 0;

    if (recording)
        return;
    stats_time(&mark, NULL);
    vkWaitForFences(device, 1, &f->done, VK_TRUE, UINT64_MAX);
    stats_time(&mark, &stat_wait);
    render_frame_reset(frame_index);
    vkResetCommandBuffer(f->cmd, 0);
    check(vkBeginCommandBuffer(f->cmd, &info), "vkBeginCommandBuffer");
    recording = TRUE;
    record_serial++;
}

static uint32_t frame_cmds(struct frame *f, VkCommandBuffer cmds[2])
{
    uint32_t n = 0;

    if (f->pre_open) {
        vk_memory_barrier(f->pre, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                          VK_ACCESS_SHADER_READ_BIT);
        check(vkEndCommandBuffer(f->pre), "vkEndCommandBuffer");
        f->pre_open = FALSE;
        cmds[n++] = f->pre;
    }
    cmds[n++] = f->cmd;
    return n;
}

static unsigned pass_serial;

static void end_pass(void)
{
    if (recording)
        render_flush();
    if (!in_pass)
        return;
    vkCmdEndRenderPass(frames[frame_index].cmd);
    in_pass = FALSE;
}

static void begin_pass(void)
{
    VkCommandBuffer cmd;
    VkRenderPassBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = framebuffer,
        .renderArea = { { 0, 0 }, { bb_width, bb_height } },
    };

    if (in_pass)
        return;
    begin_recording();
    cmd = frames[frame_index].cmd;
    if (color_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier(cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT, color_layout,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        color_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    in_pass = TRUE;
    pass_serial++;
    vkgl_stats.passes++;
}

VkCommandBuffer vk_draw_cmd(void)
{
    begin_pass();
    return frames[frame_index].cmd;
}

void vk_begin_frame(void)
{
    if (device)
        begin_recording();
}

VkCommandBuffer vk_transfer_cmd(void)
{
    begin_recording();
    end_pass();
    return frames[frame_index].cmd;
}

VkCommandBuffer vk_upload_cmd(void)
{
    struct frame *f;
    VkCommandBufferBeginInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };

    begin_recording();
    f = &frames[frame_index];
    if (!f->pre_open) {
        vkResetCommandBuffer(f->pre, 0);
        check(vkBeginCommandBuffer(f->pre, &info), "vkBeginCommandBuffer");
        f->pre_open = TRUE;

        vk_memory_barrier(f->pre, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    return f->pre;
}

void vk_memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage, VkAccessFlags src,
                       VkPipelineStageFlags dst_stage, VkAccessFlags dst)
{
    VkMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = src,
        .dstAccessMask = dst,
    };

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &b, 0, NULL, 0, NULL);
}

unsigned vk_record_serial(void)
{
    return record_serial;
}

unsigned vk_pass_serial(void)
{
    return pass_serial;
}

int vk_frame_slot(void)
{
    return frame_index;
}

static VkImage color_to_transfer_src(VkCommandBuffer cmd)
{
    VkImageResolve region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { bb_width, bb_height, 1 },
    };

    end_pass();
    if (color_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        return resolved.image ? resolved.image : color.image;
    barrier(cmd, color.image, VK_IMAGE_ASPECT_COLOR_BIT, color_layout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    color_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (!resolved.image)
        return color.image;
    barrier(cmd, resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdResolveImage(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      resolved.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, resolved.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return resolved.image;
}

static void submit_and_wait(void)
{
    struct frame *f = &frames[frame_index];
    VkCommandBuffer cmds[2];
    VkSubmitInfo info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pCommandBuffers = cmds,
    };

    if (!recording)
        return;
    end_pass();
    check(vkEndCommandBuffer(f->cmd), "vkEndCommandBuffer");
    info.commandBufferCount = frame_cmds(f, cmds);
    vkResetFences(device, 1, &f->done);
    queue_submit(&info, f->done);
    recording = FALSE;
    vkWaitForFences(device, 1, &f->done, VK_TRUE, UINT64_MAX);
}

void vk_backbuffer_size(int *w, int *h)
{
    *w = bb_width;
    *h = bb_height;
}

void vk_clear(BOOL do_color, const float rgba[4], BOOL do_depth, float z,
              int x, int y, int w, int h)
{
    VkClearAttachment att[2];
    VkClearRect rect = { .layerCount = 1 };
    uint32_t n = 0;

    if (!device || (!do_color && !do_depth))
        return;
    if (w <= 0 || h <= 0) {
        x = y = 0;
        w = bb_width;
        h = bb_height;
    }

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > bb_width) w = bb_width - x;
    if (y + h > bb_height) h = bb_height - y;
    if (w <= 0 || h <= 0)
        return;
    rect.rect.offset.x = x;
    rect.rect.offset.y = bb_height - y - h;
    rect.rect.extent.width = w;
    rect.rect.extent.height = h;

    if (do_color) {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        for (int i = 0; i < 4; i++)
            att[n].clearValue.color.float32[i] = rgba[i];
        n++;
    }
    if (do_depth) {
        att[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        att[n].colorAttachment = 0;
        att[n].clearValue.depthStencil.depth = z;
        att[n].clearValue.depthStencil.stencil = 0;
        n++;
    }
    begin_pass();
    render_flush();
    vkCmdClearAttachments(frames[frame_index].cmd, n, att, 1, &rect);
}

BOOL vk_read_rgba(int x, int y, int w, int h, unsigned char *dst)
{
    VkBufferCreateInfo binfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)w * h * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkMemoryAllocateInfo ainfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { x, bb_height - y - h, 0 },
        .imageExtent = { w, h, 1 },
    };
    VkMemoryRequirements req;
    VkBuffer buf;
    VkDeviceMemory mem;
    VkImage img;
    VkCommandBuffer cmd;
    unsigned char *src;

    if (!device || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > bb_width || y + h > bb_height)
        return FALSE;
    check(vkCreateBuffer(device, &binfo, NULL, &buf), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(device, buf, &req);
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = vk_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(device, &ainfo, NULL, &mem), "vkAllocateMemory");
    check(vkBindBufferMemory(device, buf, mem, 0), "vkBindBufferMemory");

    begin_recording();
    cmd = frames[frame_index].cmd;
    img = color_to_transfer_src(cmd);
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);
    submit_and_wait();

    check(vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, (void **)&src), "vkMapMemory");
    for (int row = 0; row < h; row++)
        memcpy(dst + (size_t)row * w * 4, src + (size_t)(h - 1 - row) * w * 4, (size_t)w * 4);
    vkUnmapMemory(device, mem);
    vkDestroyBuffer(device, buf, NULL);
    vkFreeMemory(device, mem, NULL);
    return TRUE;
}

static void recreate_for_window_size(void)
{
    int w, h;

    vk_wait_idle();
    create_swapchain();
    client_size(&w, &h);
    if (w > 0 && h > 0 && (w != bb_width || h != bb_height)) {
        destroy_backbuffer();
        create_backbuffer(w, h);
    }
}

static void present_slot(int slot)
{
    struct present_job *j = &jobs[slot];
    VkPipelineStageFlags stages[2] = { VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT };
    VkSemaphore waits[2] = { j->rendered, j->acquired };
    VkSubmitInfo sinfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 2,
        .pWaitSemaphores = waits,
        .pWaitDstStageMask = stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &j->cmd,
        .signalSemaphoreCount = 1,
    };
    VkCommandBufferBeginInfo binfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkPresentInfoKHR pinfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
    };
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets[1] = { bb_width, bb_height, 1 },
        .dstOffsets[1] = { swap_extent.width, swap_extent.height, 1 },
    };
    uint64_t pid = 0;
    VkPresentIdKHR id_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &pid,
    };
    uint32_t idx;
    VkResult r;

    vkWaitForFences(device, 1, &j->done, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &j->done);
    r = swap_dirty ? VK_ERROR_OUT_OF_DATE_KHR
                   : vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, j->acquired, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        if (r != VK_ERROR_OUT_OF_DATE_KHR)
            check(r, "vkAcquireNextImageKHR");

        swap_dirty = TRUE;
        sinfo.waitSemaphoreCount = 1;
        sinfo.commandBufferCount = 0;
        sinfo.signalSemaphoreCount = 0;
        queue_submit(&sinfo, j->done);
        return;
    }
    vkResetCommandBuffer(j->cmd, 0);
    check(vkBeginCommandBuffer(j->cmd, &binfo), "vkBeginCommandBuffer");
    barrier(j->cmd, swap_images[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdBlitImage(j->cmd, staging[slot].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swap_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    barrier(j->cmd, swap_images[idx], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    check(vkEndCommandBuffer(j->cmd), "vkEndCommandBuffer");
    sinfo.pSignalSemaphores = &swap_done[idx];
    pinfo.pWaitSemaphores = &swap_done[idx];
    pinfo.pImageIndices = &idx;
    if (present_wait_on) {
        LARGE_INTEGER now;
        pid = (uint64_t)present_id + 1;
        pinfo.pNext = &id_info;
        QueryPerformanceCounter(&now);
        present_qpc[pid & 63] = now.QuadPart;
    }
    EnterCriticalSection(&queue_lock);
    r = vkQueueSubmit(queue, 1, &sinfo, j->done);
    check(r, "vkQueueSubmit");
    r = vkQueuePresentKHR(queue, &pinfo);
    LeaveCriticalSection(&queue_lock);
    if (present_wait_on)
        InterlockedIncrement(&present_id);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        swap_dirty = TRUE;
    else
        check(r, "vkQueuePresentKHR");
}

static DWORD WINAPI present_thread(void *arg)
{
    (void)arg;
    EnterCriticalSection(&job_lock);
    for (;;) {
        int slot;

        while (job_head == job_tail)
            SleepConditionVariableCS(&job_posted, &job_lock, INFINITE);
        slot = job_ring[job_head % FRAMES_IN_FLIGHT];
        LeaveCriticalSection(&job_lock);
        present_slot(slot);
        EnterCriticalSection(&job_lock);
        job_head++;
        jobs[slot].pending = FALSE;
        WakeAllConditionVariable(&job_finished);
    }
    return 0;
}

static void start_present_thread(void)
{
    VkFenceCreateInfo finfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    VkSemaphoreCreateInfo sinfo = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkCommandPoolCreateInfo pinfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queue_family,
        };
        VkCommandBufferAllocateInfo ainfo = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };

        check(vkCreateCommandPool(device, &pinfo, NULL, &jobs[i].pool), "vkCreateCommandPool");
        ainfo.commandPool = jobs[i].pool;
        check(vkAllocateCommandBuffers(device, &ainfo, &jobs[i].cmd), "vkAllocateCommandBuffers");
        check(vkCreateFence(device, &finfo, NULL, &jobs[i].done), "vkCreateFence");
        check(vkCreateSemaphore(device, &sinfo, NULL, &jobs[i].rendered), "vkCreateSemaphore");
        check(vkCreateSemaphore(device, &sinfo, NULL, &jobs[i].acquired), "vkCreateSemaphore");
    }
    InitializeCriticalSection(&job_lock);
    InitializeConditionVariable(&job_posted);
    InitializeConditionVariable(&job_finished);
    CloseHandle(CreateThread(NULL, 0, present_thread, NULL, 0, NULL));
}

static void drain_presents(void)
{
    if (!present_thread_on)
        return;
    EnterCriticalSection(&job_lock);
    while (job_head != job_tail)
        SleepConditionVariableCS(&job_finished, &job_lock, INFINITE);
    LeaveCriticalSection(&job_lock);
}

static void present_frame_async(void)
{
    static int client_w, client_h;
    static unsigned n;
    int slot = frame_index;
    struct frame *f = &frames[slot];
    VkCommandBuffer cmds[2];
    VkSubmitInfo sinfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pCommandBuffers = cmds,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &jobs[slot].rendered,
    };
    VkImageCopy region = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { bb_width, bb_height, 1 },
    };
    VkCommandBuffer cmd;
    VkImage src;

    if (n++ % 16 == 0 || swap_dirty)
        client_size(&client_w, &client_h);
    if (client_w <= 0 || client_h <= 0) {
        submit_and_wait();
        return;
    }
    if (swap_dirty || client_w != swap_client_w || client_h != swap_client_h) {
        submit_and_wait();
        drain_presents();
        recreate_for_window_size();
        if (swap_dirty)
            return;
    }

    begin_recording();
    cmd = f->cmd;
    src = color_to_transfer_src(cmd);
    barrier(cmd, staging[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   staging[slot].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, staging[slot].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    sinfo.commandBufferCount = frame_cmds(f, cmds);
    recording = FALSE;

    EnterCriticalSection(&job_lock);
    while (jobs[slot].pending)
        SleepConditionVariableCS(&job_finished, &job_lock, INFINITE);
    LeaveCriticalSection(&job_lock);
    vkResetFences(device, 1, &f->done);
    queue_submit(&sinfo, f->done);
    EnterCriticalSection(&job_lock);
    jobs[slot].pending = TRUE;
    job_ring[job_tail % FRAMES_IN_FLIGHT] = slot;
    job_tail++;
    WakeConditionVariable(&job_posted);
    LeaveCriticalSection(&job_lock);
    frame_index = (frame_index + 1) % FRAMES_IN_FLIGHT;
}

static void present_frame(void)
{
    struct frame *f = &frames[frame_index];
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkCommandBuffer cmds[2];
    VkSubmitInfo sinfo = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &f->acquired,
        .pWaitDstStageMask = &wait_stage,
        .pCommandBuffers = cmds,
        .signalSemaphoreCount = 1,
    };
    VkPresentInfoKHR pinfo = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
    };
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
    };
    uint64_t pid = 0;
    VkPresentIdKHR id_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &pid,
    };
    VkImage src;
    VkCommandBuffer cmd;
    VkResult r;
    uint32_t idx;
    int w, h;

    if (!device)
        return;
    if (present_thread_on) {
        present_frame_async();
        return;
    }
    client_size(&w, &h);
    if (w <= 0 || h <= 0) {
        submit_and_wait();
        return;
    }
    if (swap_dirty || w != swap_client_w || h != swap_client_h) {
        submit_and_wait();
        recreate_for_window_size();
        if (swap_dirty)
            return;
    }

    begin_recording();
    r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        submit_and_wait();
        recreate_for_window_size();
        return;
    }
    if (r != VK_SUBOPTIMAL_KHR)
        check(r, "vkAcquireNextImageKHR");

    cmd = f->cmd;
    src = color_to_transfer_src(cmd);
    barrier(cmd, swap_images[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    blit.srcOffsets[1] = (VkOffset3D){ bb_width, bb_height, 1 };
    blit.dstOffsets[1] = (VkOffset3D){ swap_extent.width, swap_extent.height, 1 };
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swap_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_NEAREST);
    barrier(cmd, swap_images[idx], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    sinfo.commandBufferCount = frame_cmds(f, cmds);
    recording = FALSE;

    sinfo.pSignalSemaphores = &swap_done[idx];
    vkResetFences(device, 1, &f->done);
    queue_submit(&sinfo, f->done);
    pinfo.pWaitSemaphores = &swap_done[idx];
    pinfo.pImageIndices = &idx;
    if (present_wait_on) {
        LARGE_INTEGER now;
        pid = (uint64_t)present_id + 1;
        pinfo.pNext = &id_info;
        QueryPerformanceCounter(&now);
        present_qpc[pid & 63] = now.QuadPart;
    }
    EnterCriticalSection(&queue_lock);
    r = vkQueuePresentKHR(queue, &pinfo);
    LeaveCriticalSection(&queue_lock);
    if (present_wait_on)
        InterlockedIncrement(&present_id);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        swap_dirty = TRUE;
    else
        check(r, "vkQueuePresentKHR");
    frame_index = (frame_index + 1) % FRAMES_IN_FLIGHT;
}

void vk_present(void)
{
    LONGLONG mark = 0;

    stats_time(&mark, NULL);
    present_frame();
    stats_time(&mark, &stat_present);
    stats_frame();
}

void vk_wait_idle(void)
{
    if (!device)
        return;
    submit_and_wait();
    drain_presents();
    EnterCriticalSection(&queue_lock);
    vkDeviceWaitIdle(device);
    LeaveCriticalSection(&queue_lock);
}

void vk_set_swap_interval(int interval)
{
    if ((interval != 0) != (swap_interval != 0))
        swap_dirty = TRUE;
    swap_interval = interval;
}

int vk_swap_interval(void)
{
    return swap_interval;
}

const char *vk_device_name(void)
{
    return device_name;
}
