#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_color;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 flags;
} pc;

void main() {
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
}
