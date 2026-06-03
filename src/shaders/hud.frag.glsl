#version 450
// Sample the captured 2D image (HUD layer). Discard pixels matching the
// BetterVR magic-clear colors so the underlying 3D scene shows through.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 magicA;     // 2D magic color A (left), .a = tolerance
    vec4 magicB;     // 2D magic color B (right), .a unused
} pc;
void main() {
    vec4 c = texture(src, vUV);
    float tol = pc.magicA.a;
    vec3 ma = pc.magicA.rgb;
    vec3 mb = pc.magicB.rgb;
    if (all(lessThan(abs(c.rgb - ma), vec3(tol))) ||
        all(lessThan(abs(c.rgb - mb), vec3(tol)))) {
        discard;
    }
    outColor = c;
}
