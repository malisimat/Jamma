#version 330 core

in vec2 UV;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform vec2 BlurDirection;

// The original radius-20 kernel, normalized once instead of per fragment.
// With GL_LINEAR, each weighted offset combines two adjacent integer taps:
// offset = (i*w[i] + (i+1)*w[i+1]) / (w[i] + w[i+1]).
const float CenterWeight = 0.0262776575;
const float Offsets[10] = float[10](
    1.4963379561, 3.4914559099, 5.4865754928, 7.4816976342, 9.4768232616,
    11.4719532998, 13.4670886704, 15.4622302906, 17.4573790734, 19.4525359258);
const float Weights[10] = float[10](
    0.0826323509, 0.0787038708, 0.0720972754, 0.0635211529, 0.0538263120,
    0.0438679739, 0.0343856459, 0.0259228894, 0.0187960321, 0.0131076682);

void main()
{
    vec2 texelSize = 1.0 / vec2(textureSize(TextureSampler, 0));
    vec2 stepUV = BlurDirection * texelSize;
    vec2 minUV = 0.5 * texelSize;
    vec2 maxUV = vec2(1.0) - minUV;
    vec4 colour = texture(TextureSampler, UV) * CenterWeight;

    for (int i = 0; i < 10; ++i)
    {
        vec2 offset = Offsets[i] * stepUV;
        colour += Weights[i] * texture(TextureSampler, clamp(UV + offset, minUV, maxUV));
        colour += Weights[i] * texture(TextureSampler, clamp(UV - offset, minUV, maxUV));
    }

    ColorOUT = colour;
    // Reduce opacity only in the final (vertical) composite pass.
    if (BlurDirection.y != 0.0)
        ColorOUT.a *= 0.55;
}
