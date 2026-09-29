#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;

out vec2 UV;
out float diff;
out float EditorU;
out float EditorMorphV;

uniform mat4 MVP;
uniform sampler1D WaveformSampler;
uniform float WaveformRadius;
uniform float WaveformHeightScale;
uniform float WaveformMinHeight;
uniform float WaveformColorMultiplier;
uniform float WaveformUnitMeshRadius;
uniform float WaveformColorScale;
uniform float EditorMorph;

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
    float colorScale = max(WaveformColorScale, 0.0);

    float safeUnitRadius = max(WaveformUnitMeshRadius, 0.0001);
    float radiusScale = WaveformRadius / safeUnitRadius; 
    vec2 scaledXZ = PositionIN.xz * radiusScale;

    // The two UV seam vertices share the ring position but end at opposite
    // grid edges. No vertex buffer changes during the transition.
    float gridZ = clamp(y / max(WaveformHeightScale, 0.0001), -1.0, 1.0)
        * WaveformRadius * 0.78;
    float gridY = 3.0 + (length(scaledXZ) - WaveformRadius) * 0.55;
    vec3 gridPosition = vec3((u - 0.5) * WaveformRadius * 2.0, gridY, gridZ);
    gl_Position = MVP * vec4(mix(vec3(scaledXZ.x, y, scaledXZ.y),
        gridPosition, EditorMorph), 1.0);
    EditorU = u;
    EditorMorphV = EditorMorph;
    float colorV = clamp(0.5 - (y * colorScale * WaveformColorMultiplier), 0.0, 1.0);
    UV = vec2(u, colorV);

    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    vec4 normScreen = MVP * vec4(NormalIN, 0.0);
    diff = 0.1 + clamp(dot(normScreen.xyz, lightDir), 0.0, 0.9);
}
