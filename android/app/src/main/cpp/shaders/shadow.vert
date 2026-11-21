#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec2 in_uv;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 flags;
} pc;

void main() {
    vec4 p = pc.mvp * vec4(in_pos, 1.0);
    p.z = min(p.z + 0.0025 * p.w, p.w);
    gl_Position = p;
}
