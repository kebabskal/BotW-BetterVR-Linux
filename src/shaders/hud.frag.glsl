#version 450
// HUD quad shader. Mirrors the upstream BetterVR approach: the magic-color
// clear is replaced with a clear-to-zero in our CmdClearColorImage hook,
// and the ImproveGUI PPC patches make BotW write proper alpha on UI
// elements. So the texture's own alpha channel is a faithful UI mask:
//   - alpha = 0: background (didn't draw here)  → transparent
//   - alpha > 0: UI element drew here           → opaque (or blended)
// The XR runtime composites this quad over the projection layer using
// XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 magicA;     // unused (kept for ABI compat with existing pipeline)
    vec4 magicB;
} pc;
void main() {
    vec4 c = texture(src, vUV);
    outColor = vec4(c.rgb, c.a);
}
