#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec4 out_color;
layout(location = 2) out vec4 out_shadow_pos;

layout(set = 0, binding = 1) uniform SceneUbo {
    mat4 light_vp;
    vec4 sun_dir;
    vec4 sky_tint;
} ubo;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 flags;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
    out_normal = in_normal;
    out_color = in_color;
    out_shadow_pos = ubo.light_vp * vec4(in_pos, 1.0);
}
