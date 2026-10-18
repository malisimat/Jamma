#version 330 core

in vec2 UV;
out vec4 ColorOUT;
uniform float Opacity;

uniform sampler2D TextureSampler;
uniform vec3 TintColor;
uniform sampler2D TintMaskSampler;
uniform int HasTintMask;

void main()
{
    vec4 texel = texture(TextureSampler, UV);
    // No mask preserves legacy multiplication exactly. A mask releases tint
    // toward authored RGB: 0 = tinted, 1 = authored colour, gray = a blend.
    // RGB still supplies brightness; alpha remains coverage, never tint data.
    float releaseTint = HasTintMask != 0 ? texture(TintMaskSampler, UV).r : 0.0;
    ColorOUT = vec4(texel.rgb * mix(TintColor, vec3(1.0), releaseTint), texel.a * Opacity);
}
