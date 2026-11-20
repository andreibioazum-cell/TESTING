#version 450
layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 sun_dir;   // xyz = sun dir, w = unlit flag
    vec4 sky_tint;  // xyz = ambient tint, w = ambient strength
} pc;

void main() {
    if (pc.sun_dir.w > 0.5) {
        out_color = in_color;
        return;
    }
    vec3 n = normalize(in_normal);
    vec3 l = normalize(pc.sun_dir.xyz);
    float diff = max(dot(n, l), 0.0);
    float hemi = n.y * 0.14 + 0.14;
    float shade = clamp(pc.sky_tint.w + diff * 0.58 + hemi, 0.35, 1.15);
    vec3 lit = in_color.rgb * shade * pc.sky_tint.rgb;
    out_color = vec4(clamp(lit, 0.0, 1.0), in_color.a);
}
