#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;

out vec2 UV;
out float diff;

uniform mat4 MVP;
uniform sampler1D WaveformSampler;
uniform float WaveformRadius;
uniform float WaveformHeightScale;
uniform float WaveformMinHeight;
uniform float WaveformColorMultiplier;
uniform float WaveformUnitMeshRadius;

void main()
{
    float u = clamp(UvIN.x, 0.0, 1.0);
    float segmentCount = float(textureSize(WaveformSampler, 0));
    float segmentIndex = clamp(floor(u * segmentCount), 0.0, segmentCount - 1.0);
    float waveformU = (segmentIndex + 0.5) / segmentCount;
    vec2 minMax = texture(WaveformSampler, waveformU).rg;

    float yMin = (WaveformHeightScale * minMax.x) - WaveformMinHeight;
    float yMax = (WaveformHeightScale * minMax.y) + WaveformMinHeight;
    float y = PositionIN.y >= 0.0 ? yMax : yMin;

    float safeUnitRadius = max(WaveformUnitMeshRadius, 0.0001);
    float radiusScale = WaveformRadius / safeUnitRadius; 
    vec2 scaledXZ = PositionIN.xz * radiusScale;

    gl_Position = MVP * vec4(scaledXZ.x, y, scaledXZ.y, 1.0);
    float colorV = clamp(0.5 - (y * WaveformColorMultiplier), 0.0, 1.0);
    UV = vec2(u, colorV);

    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    vec4 normScreen = MVP * vec4(NormalIN, 0.0);
    diff = 0.1 + clamp(dot(normScreen.xyz, lightDir), 0.0, 0.9);
}
