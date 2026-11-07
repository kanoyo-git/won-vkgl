#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv0;
layout(location = 2) in vec2 v_uv1;
layout(location = 3) in float v_fog_z;

layout(set = 0, binding = 0) uniform sampler2D u_tex0;
layout(set = 0, binding = 1) uniform sampler2D u_tex1;

layout(push_constant) uniform Push {
    mat4 mvp;
    float alpha_ref;
    uint flags;
    vec4 eye_z;
    vec4 fog_color;
    vec4 fog;
} pc;

layout(location = 0) out vec4 o_color;

const uint ENV_OFF = 0u, ENV_MODULATE = 1u, ENV_REPLACE = 2u, ENV_DECAL = 3u,
           ENV_ADD = 4u;

vec4 texenv(uint mode, vec4 c, vec4 t)
{
    if (mode == ENV_MODULATE) return c * t;
    if (mode == ENV_REPLACE) return t;
    if (mode == ENV_DECAL) return vec4(mix(c.rgb, t.rgb, t.a), c.a);
    if (mode == ENV_ADD) return vec4(min(c.rgb + t.rgb, 1.0), c.a * t.a);
    return c;
}

bool alpha_pass(uint func, float a, float ref)
{
    switch (func) {
    case 0u: return false;
    case 1u: return a < ref;
    case 2u: return a == ref;
    case 3u: return a <= ref;
    case 4u: return a > ref;
    case 5u: return a != ref;
    case 6u: return a >= ref;
    }
    return true;
}

void main()
{
    vec4 c = v_color;
    uint env0 = pc.flags & 7u, env1 = (pc.flags >> 3) & 7u;

    if (env0 != ENV_OFF)
        c = texenv(env0, c, texture(u_tex0, v_uv0));
    if (env1 != ENV_OFF)
        c = texenv(env1, c, texture(u_tex1, v_uv1));
    uint fog = (pc.flags >> 9) & 3u;
    if (fog != 0u) {
        float z = v_fog_z, f;
        if (fog == 1u)
            f = (pc.fog.x - z) * pc.fog.y;
        else if (fog == 2u)
            f = exp(-pc.fog.z * z);
        else
            f = exp(-(pc.fog.z * z) * (pc.fog.z * z));
        c.rgb = mix(pc.fog_color.rgb, c.rgb, clamp(f, 0.0, 1.0));
    }
    if (!alpha_pass((pc.flags >> 6) & 7u, c.a, pc.alpha_ref))
        discard;
    o_color = c;
}
