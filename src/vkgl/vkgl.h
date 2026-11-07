#pragma once
#include <string.h>
#include <windows.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include "glcore.h"
#include "vkgl_ids.h"

#define GLFUNC(ret, name, params) \
    ret WINAPI impl_##name params __asm__("_impl_" #name); \
    ret WINAPI impl_##name params

void vkgl_log(const char *fmt, ...);
void vkgl_fatal(const char *fmt, ...) __attribute__((noreturn));
extern HMODULE vkgl_module;

void vkgl_trace_rearm(void);
void vkgl_select_renderer(void);

BOOL vkgl_renderer_vulkan(void);
BOOL vkgl_renderer_wanted(void);
void vkgl_renderer_request(BOOL vulkan);
void vkgl_menu_patch(void *gameui);
void vkgl_menu_test_frame(void);
void vkgl_engine_patches(void);
BOOL vkgl_engine_image(BYTE **base, SIZE_T *size);
int vkgl_ini_int(const char *section, const char *key, int def);
void vkgl_capture_setup(void);
void vkgl_mouse_setup(void);
void vkgl_mouse_frame(void);
void vkgl_mouse_feed(LONG dx, LONG dy);
void vkgl_capture_report(PFN_vkGetInstanceProcAddr gipa);

void vkgl_profile_thread(void);
extern volatile LONG vkgl_frame_count;
extern volatile LONG vkgl_upload_count;
void vkgl_swapchain_renew(void);
void *vkgl_vk_export(const char *name);
void *vkgl_vk_driver_present(void);
void vkgl_capture_frame(LONG frame);

#define VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) \
    X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) \
    X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetImageMemoryRequirements) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkBindBufferMemory) \
    X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdClearAttachments) \
    X(vkCmdPipelineBarrier) X(vkCmdBlitImage) X(vkCmdCopyImage) \
    X(vkCmdResolveImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyBufferToImage) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) \
    X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) \
    X(vkCmdPushConstants) X(vkCmdPushDescriptorSetKHR) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreateSampler) X(vkDestroySampler)

#define X(f) extern PFN_##f f;
VK_FUNCS(X)
VK_DEVICE_FUNCS(X)
#undef X

#define FRAMES_IN_FLIGHT 2

struct vk_core {
    VkPhysicalDevice gpu;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures features;
    VkDevice device;
    VkRenderPass render_pass;
    VkSampleCountFlagBits samples;
};
extern struct vk_core vkc;

void vk_bind_window(HWND wnd);

void vk_release_window(void);

void vk_backbuffer_size(int *w, int *h);

void vk_clear(BOOL color, const float rgba[4], BOOL depth, float z,
              int x, int y, int w, int h);

BOOL vk_read_rgba(int x, int y, int w, int h, unsigned char *dst);
void vk_present(void);
uint32_t vk_memory_type(uint32_t bits, VkMemoryPropertyFlags want);

VkCommandBuffer vk_draw_cmd(void);

VkCommandBuffer vk_transfer_cmd(void);

VkCommandBuffer vk_upload_cmd(void);

void vk_memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src_stage, VkAccessFlags src,
                       VkPipelineStageFlags dst_stage, VkAccessFlags dst);

unsigned vk_record_serial(void);

unsigned vk_pass_serial(void);
int vk_frame_slot(void);

void vk_begin_frame(void);
void vk_wait_idle(void);
void vk_set_swap_interval(int interval);
int vk_swap_interval(void);
const char *vk_device_name(void);

struct vertex {
    float pos[3];
    unsigned char color[4];
    float uv[2][2];
};

enum prim_kind { PRIM_TRIANGLES, PRIM_LINES, PRIM_POINTS };

void render_init(void);

void render_frame_reset(int slot);

void render_flush(void);

struct vkgl_stats {
    unsigned draws, batches, pipeline_binds;
    unsigned uploads, upload_kb, images, passes, copies, unchanged;
};
extern struct vkgl_stats vkgl_stats;

void render_draw(enum prim_kind kind, const struct vertex *v, uint32_t count);

void *render_arena_alloc(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *offset);

void render_defer_image(VkImage image, VkImageView view, VkDeviceMemory mem);

void texture_frame_reset(int slot);

void texture_init(void);

void texture_descriptor(GLuint name, VkDescriptorImageInfo *out);

void texture_note_draw(GLuint name, int unit, const struct vertex *v, uint32_t count);

#define MAX_TEX_UNITS 2
#define MATRIX_STACK_DEPTH 32

struct gl_texunit {
    BOOL enabled;
    GLuint bound;
    GLenum env_mode;
    float env_color[4];
    float texcoord[4];
};

struct gl_array {
    BOOL enabled;
    GLint size;
    GLenum type;
    GLsizei stride;
    const unsigned char *ptr;
};

struct gl_context {
    HWND wnd;
    HDC dc;
    GLenum error;
    float clear_color[4];
    float clear_depth;
    int clear_stencil;
    int viewport[4];
    int pack_alignment, unpack_alignment;

    BOOL blend, alpha_test, depth_test, cull_face, scissor_test, fog,
         polygon_offset_fill, dither;
    GLenum blend_src, blend_dst, alpha_func, depth_func, cull_mode, front_face,
           shade_model, polygon_mode;
    float alpha_ref;
    BOOL depth_mask, color_mask[4];
    float depth_range[2];
    int scissor[4];
    float polygon_offset[2];
    GLenum fog_mode;
    float fog_color[4], fog_density, fog_start, fog_end;

    GLenum matrix_mode;
    float matrix[3][MATRIX_STACK_DEPTH][16];
    int matrix_depth[3];
    BOOL mvp_dirty;
    float mvp[16];

    int active_texture, client_active_texture;
    struct gl_texunit unit[MAX_TEX_UNITS];
    float color[4];
    unsigned char color_ub[4];

    struct gl_array vertex_array, color_array, texcoord_array[MAX_TEX_UNITS];
    BOOL in_begin;
    GLenum begin_mode;
};

extern struct gl_context *gl_current;

void gl_set_error(GLenum e);
void gl_init_state(struct gl_context *ctx);

const float *gl_mvp(struct gl_context *ctx);

PROC gl_get_extension_proc(const char *name);
