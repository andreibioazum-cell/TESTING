#version 450
layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_uv;
layout(location = 0) out vec4 color;
layout(location = 1) out vec2 uv;
layout(push_constant) uniform Screen { vec2 scale; } screen;
void main() {
    gl_Position = vec4(in_position * screen.scale - 1.0, 0.0, 1.0);
    color = in_color;
    uv = in_uv;
}
