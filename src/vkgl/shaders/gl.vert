#version 450

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv0;
layout(location = 3) in vec2 a_uv1;

layout(push_constant) uniform Push {
    mat4 mvp;
    float alpha_ref;
    uint flags;
    vec4 eye_z;
    vec4 fog_color;
    vec4 fog;
} pc;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv0;
layout(location = 2) out vec2 v_uv1;
layout(location = 3) out float v_fog_z;

void main()
{
    vec4 p = pc.mvp * vec4(a_pos, 1.0);
    p.z = (p.z + p.w) * 0.5;
    gl_Position = p;
    v_color = a_color;
    v_uv0 = a_uv0;
    v_uv1 = a_uv1;

    v_fog_z = abs(dot(pc.eye_z, vec4(a_pos, 1.0)));
}
