#include <stdlib.h>
#include "vkgl.h"

#define GL_TEXTURE_2D          0x0DE1
#define GL_TEXTURE_MAG_FILTER  0x2800
#define GL_TEXTURE_MIN_FILTER  0x2801
#define GL_TEXTURE_WRAP_S      0x2802
#define GL_TEXTURE_WRAP_T      0x2803
#define GL_NEAREST             0x2600
#define GL_LINEAR              0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST  0x2701
#define GL_NEAREST_MIPMAP_LINEAR  0x2702
#define GL_LINEAR_MIPMAP_LINEAR   0x2703
#define GL_CLAMP               0x2900
#define GL_REPEAT              0x2901
#define GL_CLAMP_TO_EDGE       0x812F
#define GL_ALPHA               0x1906
#define GL_RGB                 0x1907
#define GL_RGBA                0x1908
#define GL_LUMINANCE           0x1909
#define GL_LUMINANCE_ALPHA     0x190A
#define GL_INTENSITY           0x8049
#define GL_BGR                 0x80E0
#define GL_BGRA                0x80E1
#define GL_UNSIGNED_BYTE       0x1401
#define GL_TEXTURE_WIDTH       0x1000
#define GL_TEXTURE_HEIGHT      0x1001
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003
#define GL_TEXTURE_RED_SIZE    0x805C
#define GL_TEXTURE_ALPHA_SIZE  0x805F
#define GL_INVALID_ENUM        0x0500
#define GL_INVALID_VALUE       0x0501

#define DIRECT_NAMES 65536
#define MAX_LEVELS 16
#define MAX_SAMPLERS 1024

struct rect { int x0, y0, x1, y1; };

#define SHADOW_TEXELS (256 * 256)
#define SHADOW_BUDGET (64u << 20)
#define MAX_SAMPLED 32

struct shadow {
    unsigned char *texels;
    unsigned sampled_serial;
    int sampled_count;
    struct rect sampled[MAX_SAMPLED];
};

struct texture {
    GLuint name;
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
    int width, height, levels;
    uint32_t level_mask;
    GLenum internal;
    GLenum base;
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    VkSampler sampler;
    unsigned used;
    unsigned inline_write;
    struct shadow *sh;

    unsigned wr_serial, wr_phase;
    struct rect wr;
};

#define MAX_SPARES 512
#define SPARE_BYTES (64u << 20)
struct spare {
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
    VkDeviceSize size;
    int width, height, levels;
};
static struct spare spares[MAX_SPARES];
static int spare_count;
static VkDeviceSize spare_bytes;
static struct spare retired[FRAMES_IN_FLIGHT][MAX_SPARES];
static int retired_count[FRAMES_IN_FLIGHT];
static VkDeviceSize retired_bytes[FRAMES_IN_FLIGHT];
static size_t shadow_bytes;
static unsigned pre_serial, pre_phase;

static struct texture *direct[DIRECT_NAMES];
static struct texture **far_list;
static int far_count;
static GLuint next_gen_name = 1;
static int image_count;

static struct texture white;
static struct { uint32_t key; VkSampler sampler; } samplers[MAX_SAMPLERS];
static int sampler_count;
static float aniso;
static BOOL inline_uploads;

static struct texture **slot_for(GLuint name, BOOL create)
{
    struct texture **grown;

    if (name < DIRECT_NAMES)
        return &direct[name];
    for (int i = 0; i < far_count; i++)
        if (far_list[i] && far_list[i]->name == name)
            return &far_list[i];
    if (!create)
        return NULL;
    for (int i = 0; i < far_count; i++)
        if (!far_list[i])
            return &far_list[i];
    if (!(grown = realloc(far_list, (far_count + 1) * sizeof(*far_list))))
        vkgl_fatal("out of memory for texture names");
    far_list = grown;
    far_list[far_count] = NULL;
    return &far_list[far_count++];
}

static struct texture *lookup(GLuint name, BOOL create)
{
    struct texture **slot = slot_for(name, create);

    if (!slot)
        return NULL;
    if (!*slot && create) {
        struct texture *t = calloc(1, sizeof(*t));
        if (!t)
            vkgl_fatal("out of memory for texture %u", name);
        t->name = name;
        t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
        t->mag_filter = GL_LINEAR;
        t->wrap_s = t->wrap_t = GL_REPEAT;
        *slot = t;
    }
    return *slot;
}

static struct texture *bound_texture(GLenum target)
{
    if (!gl_current)
        return NULL;
    if (target != GL_TEXTURE_2D) {
        vkgl_log("texture target 0x%04x not supported", target);
        gl_set_error(GL_INVALID_ENUM);
        return NULL;
    }
    return lookup(gl_current->unit[gl_current->active_texture].bound, TRUE);
}

static BOOL overlap(const struct rect *a, const struct rect *b)
{
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

static void merge(struct rect *a, const struct rect *b)
{
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

static void to_general(VkCommandBuffer cmd, VkImage image, int levels)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 },
    };

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static void drop_shadow(struct texture *t)
{
    if (!t->sh)
        return;
    shadow_bytes -= (size_t)t->width * t->height * 4;
    free(t->sh->texels);
    free(t->sh);
    t->sh = NULL;
}

static void make_shadow(struct texture *t, const unsigned char *rgba)
{
    size_t n = (size_t)t->width * t->height * 4;

    if (t->width * t->height > SHADOW_TEXELS || shadow_bytes + n > SHADOW_BUDGET ||
        !(t->sh = calloc(1, sizeof(*t->sh))))
        return;
    if (!(t->sh->texels = malloc(n))) {
        free(t->sh);
        t->sh = NULL;
        return;
    }
    memcpy(t->sh->texels, rgba, n);
    shadow_bytes += n;
    if (t->used == vk_record_serial()) {

        t->sh->sampled_serial = t->used;
        t->sh->sampled_count = 1;
        t->sh->sampled[0] = (struct rect){ 0, 0, t->width, t->height };
    }
}

static void destroy_image(struct texture *t)
{
    if (!t->image)
        return;
    drop_shadow(t);
    render_defer_image(t->image, t->view, t->mem);
    t->image = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->level_mask = 0;
    image_count--;
}

static void alloc_image(int w, int h, struct spare *out)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { w, h, 1 },
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImageViewCreateInfo vinfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
    };
    VkMemoryAllocateInfo ainfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;
    int levels = 1;

    while ((w >> levels) || (h >> levels))
        levels++;
    if (levels > MAX_LEVELS)
        levels = MAX_LEVELS;
    info.mipLevels = levels;
    if (vkCreateImage(vkc.device, &info, NULL, &out->image) != VK_SUCCESS)
        vkgl_fatal("cannot create a %dx%d texture", w, h);
    vkGetImageMemoryRequirements(vkc.device, out->image, &req);
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(vkc.device, &ainfo, NULL, &out->mem) != VK_SUCCESS)
        vkgl_fatal("out of video memory for a %dx%d texture (%d textures)", w, h, image_count);
    vkBindImageMemory(vkc.device, out->image, out->mem, 0);
    vinfo.image = out->image;
    vinfo.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
    if (vkCreateImageView(vkc.device, &vinfo, NULL, &out->view) != VK_SUCCESS)
        vkgl_fatal("cannot create a texture view");
    out->size = req.size;
    out->width = w;
    out->height = h;
    out->levels = levels;
    vkgl_stats.images++;
}

static void free_spare(const struct spare *s)
{
    vkDestroyImageView(vkc.device, s->view, NULL);
    vkDestroyImage(vkc.device, s->image, NULL);
    vkFreeMemory(vkc.device, s->mem, NULL);
}

static BOOL take_spare(int w, int h, struct spare *out)
{
    for (int i = 0; i < spare_count; i++) {
        if (spares[i].width != w || spares[i].height != h)
            continue;
        *out = spares[i];
        spares[i] = spares[--spare_count];
        spare_bytes -= out->size;
        return TRUE;
    }
    return FALSE;
}

void texture_frame_reset(int slot)
{
    for (int i = 0; i < retired_count[slot]; i++) {
        struct spare *s = &retired[slot][i];
        if (spare_count < MAX_SPARES && spare_bytes + s->size <= SPARE_BYTES) {
            spares[spare_count++] = *s;
            spare_bytes += s->size;
        } else {
            free_spare(s);
        }
    }
    retired_count[slot] = 0;
    retired_bytes[slot] = 0;
}

static void create_image(struct texture *t, int w, int h)
{
    struct spare s;

    alloc_image(w, h, &s);
    to_general(inline_uploads ? vk_transfer_cmd() : vk_upload_cmd(), s.image, s.levels);
    t->image = s.image;
    t->view = s.view;
    t->mem = s.mem;
    t->width = w;
    t->height = h;
    t->levels = s.levels;
    t->level_mask = 0;
    t->used = t->inline_write = t->wr_serial = 0;
    t->sampler = VK_NULL_HANDLE;
    if (++image_count % 1000 == 0)
        vkgl_log("%d textures allocated", image_count);
}

static void pre_order(VkCommandBuffer cmd, struct texture *t, const struct rect *r)
{
    unsigned serial = vk_record_serial();

    if (pre_serial != serial) {
        pre_serial = serial;
        pre_phase = 0;
    }
    if (t->wr_serial == serial && t->wr_phase == pre_phase && (!r || overlap(&t->wr, r))) {
        vk_memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        pre_phase++;
    }
}

static void pre_wrote(struct texture *t, const struct rect *r)
{
    if (t->wr_serial == pre_serial && t->wr_phase == pre_phase) {
        merge(&t->wr, r);
    } else {
        t->wr = *r;
        t->wr_serial = pre_serial;
        t->wr_phase = pre_phase;
    }
}

static BOOL new_version(struct texture *t)
{
    int slot = vk_frame_slot();
    struct spare s, *old;
    struct rect all = { 0, 0, t->width, t->height };
    VkImageCopy regions[MAX_LEVELS];
    VkCommandBuffer cmd;
    BOOL fresh = FALSE;
    int n = 0;

    if (retired_count[slot] == MAX_SPARES || retired_bytes[slot] > SPARE_BYTES)
        return FALSE;
    if (!take_spare(t->width, t->height, &s)) {
        alloc_image(t->width, t->height, &s);
        fresh = TRUE;
    }
    for (int i = 0; i < t->levels; i++) {
        if (!(t->level_mask >> i & 1))
            continue;
        regions[n++] = (VkImageCopy){
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 },
            .extent = { t->width >> i ? t->width >> i : 1, t->height >> i ? t->height >> i : 1, 1 },
        };
    }
    cmd = vk_upload_cmd();
    if (fresh)
        to_general(cmd, s.image, s.levels);
    pre_order(cmd, t, NULL);
    if (n)
        vkCmdCopyImage(cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, s.image, VK_IMAGE_LAYOUT_GENERAL,
                       n, regions);
    old = &retired[slot][retired_count[slot]++];
    *old = s;
    old->image = t->image;
    old->view = t->view;
    old->mem = t->mem;
    retired_bytes[slot] += s.size;
    t->image = s.image;
    t->view = s.view;
    t->mem = s.mem;
    t->used = 0;
    t->wr_serial = 0;
    pre_wrote(t, &all);
    if (t->sh)
        t->sh->sampled_count = 0;
    vkgl_stats.copies++;
    return TRUE;
}

static BOOL changed_texels(const struct texture *t, const unsigned char *rgba, struct rect *r)
{
    int w = r->x1 - r->x0, h = r->y1 - r->y0, x0 = w, x1 = 0, y0 = -1, y1 = 0;

    for (int y = 0; y < h; y++) {
        const uint32_t *a = (const uint32_t *)(rgba + (size_t)y * w * 4);
        const uint32_t *b = (const uint32_t *)(t->sh->texels +
                                                ((size_t)(r->y0 + y) * t->width + r->x0) * 4);
        int l = 0, e = w;
        if (!memcmp(a, b, (size_t)w * 4))
            continue;
        while (a[l] == b[l])
            l++;
        while (a[e - 1] == b[e - 1])
            e--;
        if (y0 < 0)
            y0 = y;
        y1 = y + 1;
        if (l < x0)
            x0 = l;
        if (e > x1)
            x1 = e;
    }
    if (y0 < 0)
        return FALSE;
    r->x1 = r->x0 + x1;
    r->x0 += x0;
    r->y1 = r->y0 + y1;
    r->y0 += y0;
    return TRUE;
}

static BOOL sampled(const struct texture *t, unsigned serial, const struct rect *r)
{
    const struct shadow *sh = t->sh;

    if (sh->sampled_serial != serial)
        return FALSE;
    for (int i = 0; i < sh->sampled_count; i++)
        if (overlap(&sh->sampled[i], r))
            return TRUE;
    return FALSE;
}

static void update(struct texture *t, int level, int x, int y, int w, int h,
                   const unsigned char *rgba)
{
    struct rect r = { x, y, x + w, y + h };
    const unsigned char *src;
    unsigned char *dst;
    VkBuffer buf;
    VkDeviceSize offset;
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 } };
    VkCommandBuffer cmd;
    unsigned serial;
    int rw, rh;

    if (t->sh && level)
        drop_shadow(t);
    if (t->sh && !changed_texels(t, rgba, &r)) {
        vkgl_stats.unchanged++;
        return;
    }
    rw = r.x1 - r.x0;
    rh = r.y1 - r.y0;
    src = rgba + ((size_t)(r.y0 - y) * w + (r.x0 - x)) * 4;
    dst = render_arena_alloc((VkDeviceSize)rw * rh * 4, &buf, &offset);
    for (int i = 0; i < rh; i++) {
        memcpy(dst + (size_t)i * rw * 4, src + (size_t)i * w * 4, (size_t)rw * 4);
        if (t->sh)
            memcpy(t->sh->texels + ((size_t)(r.y0 + i) * t->width + r.x0) * 4,
                   src + (size_t)i * w * 4, (size_t)rw * 4);
    }
    serial = vk_record_serial();
    vkgl_stats.uploads++;
    vkgl_stats.upload_kb += (unsigned)((size_t)rw * rh * 4 >> 10);
    vkgl_upload_count++;
    region.bufferOffset = offset;
    region.imageOffset = (VkOffset3D){ r.x0, r.y0, 0 };
    region.imageExtent = (VkExtent3D){ rw, rh, 1 };
    if (!inline_uploads && t->inline_write != serial &&
        (t->used != serial || (t->sh && !sampled(t, serial, &r)) || new_version(t))) {
        cmd = vk_upload_cmd();
        pre_order(cmd, t, &r);
        vkCmdCopyBufferToImage(cmd, buf, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        pre_wrote(t, &r);
        return;
    }

    cmd = vk_transfer_cmd();
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, buf, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    t->inline_write = serial;
}

static GLenum base_format(GLenum internal)
{
    switch (internal) {
    case 1: case GL_LUMINANCE: case 0x803F: case 0x8040: case 0x8041: case 0x8042:
        return GL_LUMINANCE;
    case 2: case GL_LUMINANCE_ALPHA: case 0x8043: case 0x8044: case 0x8045:
    case 0x8046: case 0x8047: case 0x8048:
        return GL_LUMINANCE_ALPHA;
    case GL_ALPHA: case 0x803B: case 0x803C: case 0x803D: case 0x803E:
        return GL_ALPHA;
    case GL_INTENSITY: case 0x804A: case 0x804B: case 0x804C: case 0x804D:
        return GL_INTENSITY;
    case 3: case GL_RGB: case 0x2A10: case 0x804F: case 0x8050: case 0x8051:
    case 0x8052: case 0x8053: case 0x8054:
        return GL_RGB;
    case 4: case GL_RGBA: case 0x8055: case 0x8056: case 0x8057: case 0x8058:
    case 0x8059: case 0x805A: case GL_BGRA:
        return GL_RGBA;
    }
    return 0;
}

static int format_size(GLenum format)
{
    switch (format) {
    case GL_RGBA: case GL_BGRA: return 4;
    case GL_RGB: case GL_BGR: return 3;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_LUMINANCE: case GL_ALPHA: return 1;
    }
    return 0;
}

static unsigned char *to_rgba(const unsigned char *src, int w, int h, GLenum format,
                              GLenum type, GLenum base)
{
    int n = format_size(format), align = gl_current->unpack_alignment;
    int stride = (w * n + align - 1) / align * align;
    unsigned char *out;

    if (!n || type != GL_UNSIGNED_BYTE) {
        vkgl_log("texture upload: format 0x%04x type 0x%04x not supported", format, type);
        gl_set_error(GL_INVALID_ENUM);
        return NULL;
    }
    if (!(out = malloc((size_t)w * h * 4))) {
        gl_set_error(0x0505);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *s = src + (size_t)y * stride;
        unsigned char *d = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, s += n, d += 4) {
            unsigned char r, g, b, a;
            switch (format) {
            case GL_RGBA: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
            case GL_BGRA: r = s[2]; g = s[1]; b = s[0]; a = s[3]; break;
            case GL_RGB: r = s[0]; g = s[1]; b = s[2]; a = 255; break;
            case GL_BGR: r = s[2]; g = s[1]; b = s[0]; a = 255; break;
            case GL_LUMINANCE_ALPHA: r = g = b = s[0]; a = s[1]; break;
            case GL_LUMINANCE: r = g = b = s[0]; a = 255; break;
            default: r = g = b = 0; a = s[0]; break;
            }
            switch (base) {
            case GL_RGB: a = 255; break;
            case GL_LUMINANCE: g = b = r; a = 255; break;
            case GL_LUMINANCE_ALPHA: g = b = r; break;
            case GL_INTENSITY: g = b = a = r; break;
            case GL_ALPHA: r = g = b = 255; break;
            }
            d[0] = r; d[1] = g; d[2] = b; d[3] = a;
        }
    }
    return out;
}

static const unsigned char *as_rgba(const unsigned char *src, int w, int h, GLenum format,
                                    GLenum type, GLenum base, unsigned char **owned)
{
    int align = gl_current->unpack_alignment;

    *owned = NULL;
    if (format == GL_RGBA && base == GL_RGBA && type == GL_UNSIGNED_BYTE &&
        (w * 4 + align - 1) / align * align == w * 4)
        return src;
    return *owned = to_rgba(src, w, h, format, type, base);
}

static VkSampler get_sampler(struct texture *t)
{
    BOOL mip = t->min_filter >= GL_NEAREST_MIPMAP_NEAREST && t->min_filter <= GL_LINEAR_MIPMAP_LINEAR;
    BOOL min_linear = t->min_filter == GL_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_NEAREST ||
                      t->min_filter == GL_LINEAR_MIPMAP_LINEAR;
    BOOL mip_linear = t->min_filter == GL_NEAREST_MIPMAP_LINEAR ||
                      t->min_filter == GL_LINEAR_MIPMAP_LINEAR;
    int levels = 0;
    uint32_t key;
    VkSamplerCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

    while (levels < t->levels && (t->level_mask >> levels & 1))
        levels++;
    if (!mip || levels < 1)
        levels = 1;
    key = (uint32_t)min_linear | mip << 1 | mip_linear << 2 |
          (t->mag_filter != GL_NEAREST) << 3 |
          (t->wrap_s == GL_REPEAT) << 4 | (t->wrap_t == GL_REPEAT) << 5 | (uint32_t)levels << 6;
    for (int i = 0; i < sampler_count; i++)
        if (samplers[i].key == key)
            return samplers[i].sampler;
    if (sampler_count == MAX_SAMPLERS)
        vkgl_fatal("more than %d sampler states", MAX_SAMPLERS);

    info.magFilter = (key >> 3 & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

    info.addressModeU = (key >> 4 & 1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                       : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = (key >> 5 & 1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                       : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

    info.maxLod = mip ? (float)(levels - 1) : 0.25f;
    if (aniso > 1 && mip && min_linear) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = aniso;
    }
    if (vkCreateSampler(vkc.device, &info, NULL, &samplers[sampler_count].sampler) != VK_SUCCESS)
        vkgl_fatal("vkCreateSampler failed");
    samplers[sampler_count].key = key;
    return samplers[sampler_count++].sampler;
}

void texture_descriptor(GLuint name, VkDescriptorImageInfo *out)
{
    struct texture *t = name == 0xFFFFFFFFu ? NULL : lookup(name, FALSE);

    if (!t || !t->image || !(t->level_mask & 1))
        t = &white;
    if (!t->sampler)
        t->sampler = get_sampler(t);
    out->sampler = t->sampler;
    out->imageView = t->view;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

void texture_note_draw(GLuint name, int unit, const struct vertex *v, uint32_t count)
{
    struct texture *t = lookup(name, FALSE);
    struct shadow *sh;
    float u0, u1, v0, v1;
    struct rect r;

    if (!t || !t->image || !(t->level_mask & 1))
        return;
    t->used = vk_record_serial();
    if (!(sh = t->sh) || !count)
        return;
    u0 = u1 = v[0].uv[unit][0];
    v0 = v1 = v[0].uv[unit][1];
    for (uint32_t i = 1; i < count; i++) {
        float u = v[i].uv[unit][0], w = v[i].uv[unit][1];
        if (u < u0) u0 = u;
        if (u > u1) u1 = u;
        if (w < v0) v0 = w;
        if (w > v1) v1 = w;
    }

    r = (struct rect){ 0, 0, t->width, t->height };
    if (u0 >= 0.0f && u1 <= 1.0f) {
        r.x0 = (int)(u0 * t->width) - 1;
        r.x1 = (int)(u1 * t->width) + 2;
        if (r.x0 < 0 || r.x1 > t->width) {
            if (t->wrap_s == GL_REPEAT)
                r.x0 = 0, r.x1 = t->width;
            r.x0 = r.x0 < 0 ? 0 : r.x0;
            r.x1 = r.x1 > t->width ? t->width : r.x1;
        }
    }
    if (v0 >= 0.0f && v1 <= 1.0f) {
        r.y0 = (int)(v0 * t->height) - 1;
        r.y1 = (int)(v1 * t->height) + 2;
        if (r.y0 < 0 || r.y1 > t->height) {
            if (t->wrap_t == GL_REPEAT)
                r.y0 = 0, r.y1 = t->height;
            r.y0 = r.y0 < 0 ? 0 : r.y0;
            r.y1 = r.y1 > t->height ? t->height : r.y1;
        }
    }
    if (sh->sampled_serial != t->used) {
        sh->sampled_serial = t->used;
        sh->sampled_count = 0;
    }
    if (sh->sampled_count == MAX_SAMPLED) {
        for (int i = 1; i < MAX_SAMPLED; i++)
            merge(&sh->sampled[0], &sh->sampled[i]);
        sh->sampled_count = 1;
    }
    sh->sampled[sh->sampled_count++] = r;
}

void texture_init(void)
{
    static const unsigned char px[4] = { 255, 255, 255, 255 };
    char buf[16];

    if (GetEnvironmentVariableA("VKGL_ANISO", buf, sizeof(buf)) && vkc.features.samplerAnisotropy) {
        aniso = (float)atof(buf);
        if (aniso > vkc.props.limits.maxSamplerAnisotropy)
            aniso = vkc.props.limits.maxSamplerAnisotropy;
        if (aniso > 1)
            vkgl_log("anisotropic filtering %gx", aniso);
    }

    inline_uploads = GetEnvironmentVariableA("VKGL_INLINE_UPLOADS", buf, sizeof(buf)) > 0;
    if (inline_uploads)
        vkgl_log("texture uploads between draws (VKGL_INLINE_UPLOADS)");
    white.min_filter = white.mag_filter = GL_NEAREST;
    white.wrap_s = white.wrap_t = GL_REPEAT;
    create_image(&white, 1, 1);
    update(&white, 0, 0, 0, 1, 1, px);
    white.level_mask = 1;
}

GLFUNC(void, glBindTexture, (GLenum target, GLuint name))
{
    if (!gl_current)
        return;
    if (target != GL_TEXTURE_2D) {
        gl_set_error(GL_INVALID_ENUM);
        return;
    }
    gl_current->unit[gl_current->active_texture].bound = name;
}

GLFUNC(void, glGenTextures, (GLsizei n, GLuint *names))
{
    for (int i = 0; i < n; i++) {
        while (lookup(next_gen_name, FALSE))
            next_gen_name++;
        lookup(next_gen_name, TRUE);
        names[i] = next_gen_name++;
    }
}

GLFUNC(void, glDeleteTextures, (GLsizei n, const GLuint *names))
{
    for (int i = 0; i < n; i++) {
        struct texture **slot = slot_for(names[i], FALSE);

        if (!names[i] || !slot || !*slot)
            continue;
        destroy_image(*slot);
        free(*slot);
        *slot = NULL;
        if (gl_current)
            for (int u = 0; u < MAX_TEX_UNITS; u++)
                if (gl_current->unit[u].bound == names[i])
                    gl_current->unit[u].bound = 0;
    }
}

GLFUNC(GLboolean, glIsTexture, (GLuint name))
{
    return name && lookup(name, FALSE) != NULL;
}

GLFUNC(GLboolean, glAreTexturesResident, (GLsizei n, const GLuint *names, GLboolean *res))
{
    for (int i = 0; i < n; i++)
        res[i] = 1;
    return 1;
}

GLFUNC(void, glPrioritizeTextures, (GLsizei n, const GLuint *names, const GLclampf *prio))
{
}

GLFUNC(void, glTexImage2D, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                            GLint border, GLenum format, GLenum type, const GLvoid *pixels))
{
    struct texture *t = bound_texture(target);
    GLenum base = base_format(internal);
    int bw, bh;
    const unsigned char *rgba;
    unsigned char *owned;

    if (!t)
        return;
    if (!base || level < 0 || level >= MAX_LEVELS || w <= 0 || h <= 0 || border ||
        (uint32_t)w > vkc.props.limits.maxImageDimension2D >> level ||
        (uint32_t)h > vkc.props.limits.maxImageDimension2D >> level) {
        vkgl_log("glTexImage2D: unsupported level %d internal 0x%x %dx%d border %d",
                 level, internal, w, h, border);
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    bw = w << level;
    bh = h << level;

    if (t->image && (level == 0 ? (bw != t->width || bh != t->height)
                                : ((t->width >> level ? t->width >> level : 1) != w ||
                                   (t->height >> level ? t->height >> level : 1) != h)))
        destroy_image(t);
    if (!t->image)
        create_image(t, bw, bh);
    if (level >= t->levels) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    t->internal = internal;
    t->base = base;
    if (level)
        drop_shadow(t);
    if (pixels) {
        if (!(rgba = as_rgba(pixels, w, h, format, type, base, &owned)))
            return;
        update(t, level, 0, 0, w, h, rgba);
        if (!level && !t->sh && !(t->level_mask & ~1u))
            make_shadow(t, rgba);
        free(owned);
    } else {
        drop_shadow(t);
    }
    if (!(t->level_mask >> level & 1))
        t->sampler = VK_NULL_HANDLE;
    t->level_mask |= 1u << level;
}

GLFUNC(void, glTexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                               GLsizei h, GLenum format, GLenum type, const GLvoid *pixels))
{
    struct texture *t = bound_texture(target);
    const unsigned char *rgba;
    unsigned char *owned;

    if (!t || !pixels || w <= 0 || h <= 0)
        return;
    if (!t->image || level < 0 || level >= t->levels || !(t->level_mask >> level & 1) ||
        x < 0 || y < 0 ||
        x + w > (t->width >> level ? t->width >> level : 1) ||
        y + h > (t->height >> level ? t->height >> level : 1)) {
        gl_set_error(GL_INVALID_VALUE);
        return;
    }
    if (!(rgba = as_rgba(pixels, w, h, format, type, t->base, &owned)))
        return;
    update(t, level, x, y, w, h, rgba);
    free(owned);
}

static void tex_parameter(GLenum target, GLenum pname, GLint value)
{
    struct texture *t = bound_texture(target);

    if (!t)
        return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: t->min_filter = value; break;
    case GL_TEXTURE_MAG_FILTER: t->mag_filter = value; break;
    case GL_TEXTURE_WRAP_S: t->wrap_s = value; break;
    case GL_TEXTURE_WRAP_T: t->wrap_t = value; break;
    default:
        return;
    }
    t->sampler = VK_NULL_HANDLE;
}

GLFUNC(void, glTexParameteri, (GLenum target, GLenum pname, GLint value))
{
    tex_parameter(target, pname, value);
}

GLFUNC(void, glTexParameterf, (GLenum target, GLenum pname, GLfloat value))
{
    tex_parameter(target, pname, (GLint)value);
}

GLFUNC(void, glTexParameteriv, (GLenum target, GLenum pname, const GLint *value))
{
    tex_parameter(target, pname, value[0]);
}

GLFUNC(void, glTexParameterfv, (GLenum target, GLenum pname, const GLfloat *value))
{
    tex_parameter(target, pname, (GLint)value[0]);
}

GLFUNC(void, glGetTexLevelParameteriv, (GLenum target, GLint level, GLenum pname, GLint *out))
{
    struct texture *t = bound_texture(target);
    BOOL have = t && t->image && level >= 0 && level < MAX_LEVELS && (t->level_mask >> level & 1);

    if (!t)
        return;
    switch (pname) {
    case GL_TEXTURE_WIDTH:
        *out = have ? (t->width >> level ? t->width >> level : 1) : 0;
        break;
    case GL_TEXTURE_HEIGHT:
        *out = have ? (t->height >> level ? t->height >> level : 1) : 0;
        break;
    case GL_TEXTURE_INTERNAL_FORMAT:
        *out = have ? (GLint)t->internal : 1;
        break;
    case GL_TEXTURE_RED_SIZE: case GL_TEXTURE_RED_SIZE + 1: case GL_TEXTURE_RED_SIZE + 2:
    case GL_TEXTURE_ALPHA_SIZE:
        *out = have ? 8 : 0;
        break;
    default:
        *out = 0;
    }
}
