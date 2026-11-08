#version 450

// Fullscreen content for the latency probe. The vertex stage (probe.vert, generated
// by CMakeLists.txt) passes normalised screen coordinates in v_uv, 0..1 across the
// visible area. Derivatives of v_uv give the per-pixel step, which keeps the centre
// square square on non-square windows without a resolution push constant.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform ProbePush {
    float time_s;
    uint event_count;
    uint load;
} pc;

void main() {
    float du = abs(dFdx(v_uv.x));
    float dv = abs(dFdy(v_uv.y));

    // Dark background.
    vec3 color = vec3(0.04);

    // Large centred square, 40 percent of the height, black or white by event parity.
    float half_v = 0.2;
    float half_u = half_v * du / max(dv, 1e-9);
    if (abs(v_uv.x - 0.5) < half_u && abs(v_uv.y - 0.5) < half_v) {
        color = vec3((pc.event_count & 1u) != 0u ? 1.0 : 0.0);
    }

    // Vertical white bar: one screen width every two seconds, wrapping.
    float bar_x = fract(pc.time_s * 0.5);
    float d = v_uv.x - bar_x;
    d -= floor(d + 0.5);
    if (abs(d) < 0.008) {
        color = vec3(1.0);
    }

    // Optional GPU load: a cheap hash loop with a negligible weight in the output.
    uint h = uint(gl_FragCoord.x) * 73856093u ^ uint(gl_FragCoord.y) * 19349663u ^ pc.event_count;
    for (uint i = 0u; i < pc.load; ++i) {
        h = h * 1664525u + 1013904223u;
        h ^= h >> 13u;
    }
    color += float(h & 1023u) * (1.0 / 1023.0) * 1e-6;

    out_color = vec4(color, 1.0);
}
