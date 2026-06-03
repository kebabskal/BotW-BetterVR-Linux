#version 450
// HUD quad shader. The 2D buffer's transparency model is hybrid because
// the BetterVR PPC patches that should set up an alpha mask on Linux/Cemu
// don't fully apply, so we can't trust the alpha channel alone:
//   * Strict chromakey: discard pixels exactly matching one of the four
//     BetterVR magic clears (2D L/R + 3D L/R). Tight per-channel tolerance
//     so real UI pixels aren't false-matched.
//   * Else: opaque, using sRGB→linear correction so the linear→sRGB the
//     runtime applies on the sRGB swapchain round-trips to identity.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 magicA;     // .a = override threshold (kept for compat)
    vec4 magicB;
} pc;
bool nearMagic(vec3 c, vec3 m, float tol) {
    return all(lessThan(abs(c - m), vec3(tol)));
}
void main() {
    vec4 c = texture(src, vUV);
    // Tolerance covers BGRA8 quantization (deltas ~0.002/channel) with
    // headroom but not enough to false-match real UI graphics.
    float tol = max(pc.magicA.a, 0.025);
    bool m2dL = nearMagic(c.rgb, vec3(0.0625, 0.123, 0.987), tol);
    bool m2dR = nearMagic(c.rgb, vec3(0.0625, 0.987, 0.123), tol);
    bool m3dL = nearMagic(c.rgb, vec3(0.0,    0.123, 0.987), tol);
    bool m3dR = nearMagic(c.rgb, vec3(0.0,    0.987, 0.123), tol);
    if (m2dL || m2dR || m3dL || m3dR) discard;
    // Swapchain is R16G16B16A16_UNORM (no implicit gamma) and the
    // projection-layer passthrough images already look correct without
    // any shader conversion, so write source pixels directly.
    outColor = vec4(c.rgb, 1.0);
}
