#version 450
// HUD quad shader: read captured 2D image, output RGBA where alpha=0 for
// magic-clear pixels (transparent) and alpha=1 for HUD pixels (opaque).
// Runtime alpha-blends this quad on top of the 3D projection layer.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Push {
    vec4 magicA;     // .rgb = magic A, .a = tolerance
    vec4 magicB;     // .rgb = magic B
} pc;
void main() {
    vec4 c = texture(src, vUV);
    float tol = pc.magicA.a;
    vec3 ma = pc.magicA.rgb;
    vec3 mb = pc.magicB.rgb;
    bool isMagic = all(lessThan(abs(c.rgb - ma), vec3(tol)))
                || all(lessThan(abs(c.rgb - mb), vec3(tol)));
    outColor = isMagic ? vec4(0.0) : vec4(c.rgb, 1.0);
}
