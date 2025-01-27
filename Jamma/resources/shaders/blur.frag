#version 330 core

precision mediump float;

in vec2 UV;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;

float CalcGauss( float x, float sigma )
{
    float coeff = 1.0 / (2.0 * 3.14157 * sigma);
    float expon = -(x*x) / (2.0 * sigma);
    return (coeff*exp(expon));
}

void main()
{
    float textureSizeX = 1080;
    float sigma = 0.2;
    vec4 texCol = texture2D( TextureSampler, UV );
    vec4 gaussCol = vec4( texCol.rgba );
    float stepX = 1.0 / textureSizeX;
    float norm = 1.0;

    for ( int i = 1; i <= 20; ++ i )
    {
        float weight = CalcGauss( float(i) / 32.0, sigma * 0.5 );
        texCol = texture2D( TextureSampler, UV + vec2( float(i) * stepX, 0.0 ) );
        gaussCol += vec4( texCol.rgba * weight );
        norm += weight;
        texCol = texture2D( TextureSampler, UV - vec2( float(i) * stepX, 0.0 ) );
        gaussCol += vec4( texCol.rgba * weight );
        norm += weight;
    }

    gaussCol.rgba /= norm;

    ColorOUT = vec4( gaussCol.rgba );
}