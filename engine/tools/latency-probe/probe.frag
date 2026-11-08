#version 450

// Fullscreen content for the latency probe. The vertex stage (probe.vert, generated
// by CMakeLists.txt) passes normalised screen coordinates in v_uv, 0..1 across the
// visible area. The scene is a dark background with one moving mid-grey bar. It must
// not flash or strobe.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform ProbePush {
    float time_s;
    uint event_count;
    uint load;
} pc;

void main() {
    // Dark background.
    vec3 color = vec3(0.04);

    // Vertical mid-grey bar: one screen width every two seconds, wrapping.
    float bar_x = fract(pc.time_s * 0.5);
    float d = v_uv.x - bar_x;
    d -= floor(d + 0.5);
    if (abs(d) < 0.008) {
        color = vec3(0.45);
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
