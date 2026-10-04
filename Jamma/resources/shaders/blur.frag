#version 330 core

precision mediump float;

in vec2 UV;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform vec2 BlurDirection;

float CalcGauss( float x, float sigma )
{
    float coeff = 1.0 / (2.0 * 3.14157 * sigma);
    float expon = -(x*x) / (2.0 * sigma);
    return (coeff*exp(expon));
}

void main()
{
    float sigma = 0.2;
    vec4 texCol = texture( TextureSampler, UV );
    vec4 gaussCol = vec4( texCol.rgba );
    vec2 texelSize = 1.0 / vec2(textureSize(TextureSampler, 0));
    vec2 stepUV = BlurDirection * texelSize;
    vec2 minUV = 0.5 * texelSize;
    vec2 maxUV = vec2(1.0) - minUV;
    float norm = 1.0;

    for ( int i = 1; i <= 20; ++ i )
    {
        float weight = CalcGauss( float(i) / 32.0, sigma * 0.5 );
        texCol = texture( TextureSampler, clamp(UV + float(i) * stepUV, minUV, maxUV) );
        gaussCol += vec4( texCol.rgba * weight );
        norm += weight;
        texCol = texture( TextureSampler, clamp(UV - float(i) * stepUV, minUV, maxUV) );
        gaussCol += vec4( texCol.rgba * weight );
        norm += weight;
    }

    gaussCol.rgba /= norm;

    ColorOUT = vec4( gaussCol.rgba );
    // Reduce opacity only in the final (vertical) composite pass.
    if (BlurDirection.y != 0.0)
        ColorOUT.a *= 0.55;
}
