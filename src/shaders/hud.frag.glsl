#version 450
// HUD quad shader: use the texture's own alpha as the transparency mask
// (no chromakey). BetterVR's 2D buffer is cleared with alpha=0 for slot 0,
// and the ImproveGUI patches force UI elements to write alpha=1 over it.
// So source alpha directly indicates UI coverage. The XR runtime alpha-
// blends this quad over the projection layer.
//
// Color space: source sampled as UNORM but the swapchain is sRGB, so the
// runtime applies linear→sRGB on write — pre-convert sRGB→linear so the
// round-trip is identity.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 magicA;     // unused (kept for compat)
    vec4 magicB;     // unused
} pc;
vec3 srgbToLinear(vec3 c) {
    bvec3 cutoff = lessThan(c, vec3(0.04045));
    vec3 lo = c / 12.92;
    vec3 hi = pow((c + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, vec3(cutoff));
}
void main() {
    vec4 c = texture(src, vUV);
    outColor = vec4(srgbToLinear(c.rgb), c.a);
}
