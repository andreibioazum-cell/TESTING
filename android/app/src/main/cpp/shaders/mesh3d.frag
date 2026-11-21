#version 450
layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec4 in_shadow_pos;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_shadow;
layout(set = 0, binding = 1) uniform SceneUbo {
    mat4 light_vp;
    vec4 sun_dir;
    vec4 sky_tint;
} ubo;
layout(set = 0, binding = 2) uniform sampler2D u_tex;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 flags; // x = unlit (1.0 = unlit)
} pc;

void main() {
    if (pc.flags.x > 0.5) {
        out_color = in_color;
        return;
    }
    vec4 texel = texture(u_tex, in_uv);
    vec4 base = vec4(in_color.rgb * texel.rgb, in_color.a * texel.a);
    vec3 n = normalize(in_normal);
    vec3 l = normalize(ubo.sun_dir.xyz);
    float diff = max(dot(n, l), 0.0);
    float hemi = n.y * 0.14 + 0.14;

    float shadow = 1.0;
    if (diff > 0.03) {
        vec3 proj = in_shadow_pos.xyz / max(in_shadow_pos.w, 0.0001);
        vec2 suv = proj.xy * 0.5 + 0.5;
        if (suv.x > 0.01 && suv.x < 0.99 && suv.y > 0.01 && suv.y < 0.99 && proj.z > 0.0 && proj.z < 1.0) {
            float bias = max(0.016 * (1.0 - diff), 0.0065);
            vec2 texelSz = vec2(1.0 / 1024.0);
            float litSum = 0.0;
            for (int ox = 0; ox <= 1; ++ox) {
                for (int oy = 0; oy <= 1; ++oy) {
                    float d = texture(u_shadow, suv + (vec2(ox, oy) - 0.5) * texelSz).r;
                    litSum += (proj.z - bias <= d) ? 1.0 : 0.0;
                }
            }
            shadow = mix(0.45, 1.0, litSum * 0.25);
        }
    }

    float shade = clamp(ubo.sky_tint.w + diff * 0.58 * shadow + hemi, 0.28, 1.15);
    vec3 lit = base.rgb * shade * ubo.sky_tint.rgb;
    out_color = vec4(clamp(lit, 0.0, 1.0), base.a);
}
