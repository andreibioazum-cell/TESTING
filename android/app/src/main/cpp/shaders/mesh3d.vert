#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec4 out_color;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 sun_dir;   // w = unlit (1.0 = unlit)
    vec4 sky_tint;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    out_normal = in_normal;
    out_color = in_color;
}
