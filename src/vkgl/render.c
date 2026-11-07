#include <stdlib.h>
#include "vkgl.h"

#define GL_NEVER               0x0200
#define GL_ALWAYS              0x0207
#define GL_ZERO                0
#define GL_ONE                 1
#define GL_SRC_COLOR           0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA           0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA           0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR           0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE  0x0308
#define GL_FRONT               0x0404
#define GL_BACK                0x0405
#define GL_FRONT_AND_BACK      0x0408
#define GL_CW                  0x0900
#define GL_LINE                0x1B01
#define GL_MODULATE            0x2100
#define GL_DECAL               0x2101
#define GL_REPLACE             0x1E01
#define GL_ADD                 0x0104
#define GL_BLEND               0x0BE2
#define GL_EXP                 0x0800
#define GL_EXP2                0x0801
#define GL_LINEAR              0x2601

#define CHUNK_SIZE (4u << 20)
#define MAX_CHUNKS 32
#define MAX_GARBAGE 256

static const uint32_t vert_spv[] =
#include "gl.vert.inc"
;
static const uint32_t frag_spv[] =
#include "gl.frag.inc"
;

struct push {
    float mvp[16];
    float alpha_ref;
    uint32_t flags;
    float pad[2];
    float eye_z[4];
    float fog_color[4];
    float fog[4];
};

struct chunk {
    VkBuffer buf;
    VkDeviceMemory mem;
    unsigned char *ptr;
    VkDeviceSize size, used;
};

static struct arena {
    struct chunk chunks[MAX_CHUNKS];
    int count, cur;
} arenas[FRAMES_IN_FLIGHT];

static struct garbage {
    VkImage image;
    VkImageView view;
    VkDeviceMemory mem;
} garbage[FRAMES_IN_FLIGHT][MAX_GARBAGE];
static int garbage_count[FRAMES_IN_FLIGHT];

static BOOL initialized;

static void chunk_create(struct chunk *c, VkDeviceSize size)
{
    VkBufferCreateInfo binfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VkMemoryAllocateInfo ainfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;

    if (vkCreateBuffer(vkc.device, &binfo, NULL, &c->buf) != VK_SUCCESS)
        vkgl_fatal("cannot create a %u byte arena buffer", (unsigned)size);
    vkGetBufferMemoryRequirements(vkc.device, c->buf, &req);
    ainfo.allocationSize = req.size;
    ainfo.memoryTypeIndex = vk_memory_type(req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(vkc.device, &ainfo, NULL, &c->mem) != VK_SUCCESS)
        vkgl_fatal("out of memory for a %u byte arena chunk", (unsigned)size);
    vkBindBufferMemory(vkc.device, c->buf, c->mem, 0);
    vkMapMemory(vkc.device, c->mem, 0, VK_WHOLE_SIZE, 0, (void **)&c->ptr);
    c->size = size;
    c->used = 0;
}

static void chunk_destroy(struct chunk *c)
{
    vkUnmapMemory(vkc.device, c->mem);
    vkDestroyBuffer(vkc.device, c->buf, NULL);
    vkFreeMemory(vkc.device, c->mem, NULL);
    memset(c, 0, sizeof(*c));
}

void *render_arena_alloc(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *offset)
{
    struct arena *a;
    struct chunk *c;

    vk_begin_frame();
    a = &arenas[vk_frame_slot()];

    size = (size + sizeof(struct vertex) - 1) / sizeof(struct vertex) * sizeof(struct vertex);
    for (;;) {
        if (a->cur >= a->count) {
            if (a->count == MAX_CHUNKS) {

                vk_wait_idle();
                vk_begin_frame();
                continue;
            }
            chunk_create(&a->chunks[a->count++], size > CHUNK_SIZE ? size : CHUNK_SIZE);
        }
        c = &a->chunks[a->cur];
        if (c->used + size <= c->size)
            break;
        a->cur++;
    }
    *buf = c->buf;
    *offset = c->used;
    c->used += size;
    return c->ptr + *offset;
}

void render_defer_image(VkImage image, VkImageView view, VkDeviceMemory mem)
{
    int slot;

    vk_begin_frame();
    slot = vk_frame_slot();
    if (garbage_count[slot] == MAX_GARBAGE) {

        vk_wait_idle();
        render_frame_reset(slot);
    }
    garbage[slot][garbage_count[slot]++] = (struct garbage){ image, view, mem };
}

void render_frame_reset(int slot)
{
    struct arena *a = &arenas[slot];

    if (!initialized)
        return;
    for (int i = 0; i < garbage_count[slot]; i++) {
        struct garbage *g = &garbage[slot][i];
        vkDestroyImageView(vkc.device, g->view, NULL);
        vkDestroyImage(vkc.device, g->image, NULL);
        vkFreeMemory(vkc.device, g->mem, NULL);
    }
    garbage_count[slot] = 0;
    texture_frame_reset(slot);

    while (a->count > 2)
        chunk_destroy(&a->chunks[--a->count]);
    for (int i = 0; i < a->count; i++)
        a->chunks[i].used = 0;
    a->cur = 0;
}

static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkShaderModule vert_module, frag_module;
static struct pipeline { uint32_t key; VkPipeline pipeline; } *pipelines;
static int pipeline_count, pipeline_cap;

static VkBlendFactor blend_factor(GLenum f)
{
    switch (f) {
    case GL_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GL_ONE: return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    }
    return VK_BLEND_FACTOR_ONE;
}

static uint32_t pipeline_key(const struct gl_context *ctx, enum prim_kind kind)
{
    uint32_t k = kind;

    if (ctx->blend)
        k |= 1u << 2 | (blend_factor(ctx->blend_src) & 15u) << 3 |
             (blend_factor(ctx->blend_dst) & 15u) << 7;
    if (ctx->depth_test)
        k |= 1u << 11 | (ctx->depth_mask ? 1u << 12 : 0) |
             ((ctx->depth_func - GL_NEVER) & 7u) << 13;
    if (ctx->cull_face && kind == PRIM_TRIANGLES)
        k |= 1u << 16 | (ctx->cull_mode == GL_FRONT ? 1u :
                         ctx->cull_mode == GL_BACK ? 2u : 3u) << 17;
    if (ctx->front_face == GL_CW)
        k |= 1u << 19;
    for (int i = 0; i < 4; i++)
        if (ctx->color_mask[i])
            k |= 1u << (20 + i);
    if (ctx->polygon_offset_fill && kind == PRIM_TRIANGLES)
        k |= 1u << 24;
    if (ctx->polygon_mode == GL_LINE && vkc.features.fillModeNonSolid && kind == PRIM_TRIANGLES)
        k |= 1u << 25;
    return k;
}

static VkPipeline create_pipeline(uint32_t k)
{
    static const VkPrimitiveTopology topology[] = {
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
    };
    static const VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
    };
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert_module, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag_module, .pName = "main" },
    };
    VkVertexInputBindingDescription binding = { 0, sizeof(struct vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[4] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(struct vertex, pos) },
        { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(struct vertex, color) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct vertex, uv[0]) },
        { 3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct vertex, uv[1]) },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 4, .pVertexAttributeDescriptions = attrs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topology[k & 3],
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };

    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = (k >> 25 & 1) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .cullMode = (k >> 16 & 1) ? (k >> 17 & 3) : VK_CULL_MODE_NONE,
        .frontFace = (k >> 19 & 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = k >> 24 & 1,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = vkc.samples,
    };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = k >> 11 & 1,
        .depthWriteEnable = k >> 12 & 1,
        .depthCompareOp = (VkCompareOp)(k >> 13 & 7),
    };
    VkPipelineColorBlendAttachmentState blend = {
        .blendEnable = k >> 2 & 1,
        .srcColorBlendFactor = k >> 3 & 15, .dstColorBlendFactor = k >> 7 & 15,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = k >> 3 & 15, .dstAlphaBlendFactor = k >> 7 & 15,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = k >> 20 & 15,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend,
    };
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3, .pDynamicStates = dynamic,
    };
    VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
        .pColorBlendState = &cb, .pDynamicState = &dyn,
        .layout = layout, .renderPass = vkc.render_pass,
    };
    VkPipeline p;

    if (vkCreateGraphicsPipelines(vkc.device, VK_NULL_HANDLE, 1, &info, NULL, &p) != VK_SUCCESS)
        vkgl_fatal("vkCreateGraphicsPipelines failed for state 0x%08x", k);
    return p;
}

static VkPipeline get_pipeline(uint32_t key)
{
    static int last = -1;

    if (last >= 0 && pipelines[last].key == key)
        return pipelines[last].pipeline;
    for (int i = 0; i < pipeline_count; i++)
        if (pipelines[i].key == key)
            return pipelines[last = i].pipeline;
    if (pipeline_count == pipeline_cap) {
        struct pipeline *grown;
        pipeline_cap = pipeline_cap ? pipeline_cap * 2 : 64;
        if (!(grown = realloc(pipelines, pipeline_cap * sizeof(*pipelines))))
            vkgl_fatal("out of memory for %d pipelines", pipeline_cap);
        pipelines = grown;
    }
    pipelines[pipeline_count].key = key;
    vkgl_log("pipeline %d: state 0x%08x", pipeline_count, key);
    pipelines[pipeline_count].pipeline = create_pipeline(key);
    last = pipeline_count++;
    return pipelines[last].pipeline;
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code,
    };
    VkShaderModule m;

    if (vkCreateShaderModule(vkc.device, &info, NULL, &m) != VK_SUCCESS)
        vkgl_fatal("vkCreateShaderModule failed");
    return m;
}

void render_init(void)
{
    VkDescriptorSetLayoutBinding bindings[MAX_TEX_UNITS];
    VkDescriptorSetLayoutCreateInfo sinfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = MAX_TEX_UNITS,
        .pBindings = bindings,
    };
    VkPushConstantRange range = {
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct push),
    };
    VkPipelineLayoutCreateInfo linfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };

    for (int i = 0; i < MAX_TEX_UNITS; i++)
        bindings[i] = (VkDescriptorSetLayoutBinding){
            i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL,
        };
    if (vkCreateDescriptorSetLayout(vkc.device, &sinfo, NULL, &set_layout) != VK_SUCCESS ||
        vkCreatePipelineLayout(vkc.device, &linfo, NULL, &layout) != VK_SUCCESS)
        vkgl_fatal("cannot create the pipeline layout");
    vert_module = shader(vert_spv, sizeof(vert_spv));
    frag_module = shader(frag_spv, sizeof(frag_spv));
    initialized = TRUE;
}

struct draw_state {
    uint32_t key;
    struct push push;
    VkDescriptorImageInfo tex[MAX_TEX_UNITS];
    VkViewport viewport;
    VkRect2D scissor;
    float bias[2];
};

static struct {
    BOOL active;
    struct draw_state st;
    VkBuffer buf;
    VkDeviceSize offset;
    uint32_t count;
} pending;

static struct {
    unsigned serial;
    VkPipeline pipeline;
    VkBuffer buf;
    struct draw_state st;
} bound;

static uint32_t env_code(GLenum mode)
{
    switch (mode) {
    case GL_REPLACE: return 2;
    case GL_DECAL: return 3;
    case GL_ADD: return 4;
    default: return 1;
    }
}

static BOOL fog_logged;

static void capture_state(struct gl_context *ctx, enum prim_kind kind, struct draw_state *st)
{
    int bw, bh;
    uint32_t alpha = 7;

    memset(st, 0, sizeof(*st));
    st->key = pipeline_key(ctx, kind);
    memcpy(st->push.mvp, gl_mvp(ctx), sizeof(st->push.mvp));
    if (ctx->alpha_test) {
        alpha = (ctx->alpha_func - GL_NEVER) & 7;
        st->push.alpha_ref = ctx->alpha_ref;
    }
    st->push.flags = alpha << 6;
    if (ctx->fog) {
        const float *mv = ctx->matrix[0][ctx->matrix_depth[0]];
        float range = ctx->fog_end - ctx->fog_start;

        st->push.flags |= (ctx->fog_mode == GL_LINEAR ? 1u : ctx->fog_mode == GL_EXP ? 2u : 3u) << 9;
        for (int i = 0; i < 4; i++)
            st->push.eye_z[i] = mv[4 * i + 2];
        memcpy(st->push.fog_color, ctx->fog_color, sizeof(st->push.fog_color));
        st->push.fog[0] = ctx->fog_end;
        st->push.fog[1] = range != 0 ? 1.0f / range : 0.0f;
        st->push.fog[2] = ctx->fog_density;
        if (!fog_logged) {
            fog_logged = TRUE;
            vkgl_log("fog: mode 0x%04x start %g end %g density %g color %g %g %g",
                     ctx->fog_mode, ctx->fog_start, ctx->fog_end, ctx->fog_density,
                     ctx->fog_color[0], ctx->fog_color[1], ctx->fog_color[2]);
        }
    }
    for (int i = 0; i < MAX_TEX_UNITS; i++) {
        if (!ctx->unit[i].enabled)
            continue;
        st->push.flags |= env_code(ctx->unit[i].env_mode) << (3 * i);
        texture_descriptor(ctx->unit[i].bound, &st->tex[i]);
    }
    for (int i = 0; i < MAX_TEX_UNITS; i++)
        if (!st->tex[i].imageView)
            texture_descriptor(0xFFFFFFFFu, &st->tex[i]);

    vk_backbuffer_size(&bw, &bh);
    st->viewport.x = (float)ctx->viewport[0];
    st->viewport.y = (float)(bh - ctx->viewport[1]);
    st->viewport.width = (float)ctx->viewport[2];
    st->viewport.height = -(float)ctx->viewport[3];
    st->viewport.minDepth = ctx->depth_range[0];
    st->viewport.maxDepth = ctx->depth_range[1];
    if (ctx->scissor_test) {
        int x = ctx->scissor[0], y = bh - ctx->scissor[1] - ctx->scissor[3];
        int w = ctx->scissor[2], h = ctx->scissor[3];
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        st->scissor.offset.x = x;
        st->scissor.offset.y = y;
        st->scissor.extent.width = w > 0 ? w : 0;
        st->scissor.extent.height = h > 0 ? h : 0;
    } else {
        st->scissor.extent.width = bw;
        st->scissor.extent.height = bh;
    }
    if (st->key >> 24 & 1) {
        st->bias[0] = ctx->polygon_offset[0];
        st->bias[1] = ctx->polygon_offset[1];
    }
}

struct vkgl_stats vkgl_stats;

void render_flush(void)
{
    VkCommandBuffer cmd;
    struct draw_state *st = &pending.st;
    BOOL fresh;
    VkWriteDescriptorSet writes[MAX_TEX_UNITS];

    if (!pending.active)
        return;
    pending.active = FALSE;
    cmd = vk_draw_cmd();
    fresh = bound.serial != vk_pass_serial();
    if (fresh) {
        memset(&bound, 0, sizeof(bound));
        bound.serial = vk_pass_serial();
    }

    if (fresh || bound.st.key != st->key) {
        VkPipeline p = get_pipeline(st->key);
        if (p != bound.pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            vkgl_stats.pipeline_binds++;
        }
        bound.pipeline = p;
    }
    if (fresh || memcmp(&bound.st.viewport, &st->viewport, sizeof(st->viewport)))
        vkCmdSetViewport(cmd, 0, 1, &st->viewport);
    if (fresh || memcmp(&bound.st.scissor, &st->scissor, sizeof(st->scissor)))
        vkCmdSetScissor(cmd, 0, 1, &st->scissor);
    if (fresh || memcmp(bound.st.bias, st->bias, sizeof(st->bias)))
        vkCmdSetDepthBias(cmd, st->bias[1], 0.0f, st->bias[0]);
    if (fresh || memcmp(&bound.st.push, &st->push, sizeof(st->push)))
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(st->push), &st->push);
    if (fresh || memcmp(bound.st.tex, st->tex, sizeof(st->tex))) {
        for (int i = 0; i < MAX_TEX_UNITS; i++) {
            st->tex[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            writes[i] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &st->tex[i],
            };
        }
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                                  MAX_TEX_UNITS, writes);
    }
    if (fresh || bound.buf != pending.buf) {
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &pending.buf, &zero);
        bound.buf = pending.buf;
    }
    bound.st = *st;
    vkgl_stats.batches++;
    vkCmdDraw(cmd, pending.count, 1, (uint32_t)(pending.offset / sizeof(struct vertex)), 0);
}

void render_draw(enum prim_kind kind, const struct vertex *v, uint32_t count)
{
    struct draw_state st;
    VkBuffer buf;
    VkDeviceSize offset;
    void *dst;

    if (!count || !gl_current || gl_current->viewport[2] <= 0 || gl_current->viewport[3] <= 0)
        return;
    vkgl_stats.draws++;
    capture_state(gl_current, kind, &st);
    for (int i = 0; i < MAX_TEX_UNITS; i++)
        st.tex[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    dst = render_arena_alloc((VkDeviceSize)count * sizeof(struct vertex), &buf, &offset);
    memcpy(dst, v, (size_t)count * sizeof(struct vertex));
    for (int i = 0; i < MAX_TEX_UNITS; i++)
        if (gl_current->unit[i].enabled)
            texture_note_draw(gl_current->unit[i].bound, i, v, count);

    if (pending.active && pending.buf == buf &&
        pending.offset + (VkDeviceSize)pending.count * sizeof(struct vertex) == offset &&
        !memcmp(&pending.st, &st, sizeof(st))) {
        pending.count += count;
        return;
    }
    render_flush();
    pending.active = TRUE;
    pending.st = st;
    pending.buf = buf;
    pending.offset = offset;
    pending.count = count;
}
