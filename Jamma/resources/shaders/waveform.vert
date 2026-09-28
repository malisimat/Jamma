#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;

out vec2 UV;
out float diff;
out vec3 ProbeNormal;
out vec3 ProbeRadial;
out vec3 ProbeUp;
out vec2 BevelEdge;

uniform mat4 MVP;
uniform mat4 ModelView;
uniform sampler1D WaveformSampler;
uniform float WaveformRadius;
uniform float WaveformHeightScale;
uniform float WaveformMinHeight;
uniform float WaveformColorMultiplier;
uniform float WaveformUnitMeshRadius;
uniform float WaveformColorScale;

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

    gl_Position = MVP * vec4(scaledXZ.x, y, scaledXZ.y, 1.0);
    float colorV = clamp(0.5 - (y * colorScale * WaveformColorMultiplier), 0.0, 1.0);
    UV = vec2(u, colorV);
    vec2 radial = normalize(PositionIN.xz);
    vec3 radialNormal = vec3(radial.x, 0.0, radial.y);
    // Tilt the probe lookup on horizontal faces toward the rim. The flat
    // geometry, amplitude colour lookup and wall normals stay unchanged.
    ProbeNormal = normalize(mat3(ModelView) *
        normalize(NormalIN + radialNormal * (0.72 * abs(NormalIN.y))));
    ProbeRadial = normalize(mat3(ModelView) * radialNormal);
    ProbeUp = normalize(mat3(ModelView) * vec3(0.0, 1.0, 0.0));
    BevelEdge = vec2(clamp((y - yMin) / max(yMax - yMin, 0.001), 0.0, 1.0),
        clamp((length(PositionIN.xz) / safeUnitRadius - 0.95) / 0.1, 0.0, 1.0));

    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    vec4 normScreen = MVP * vec4(NormalIN, 0.0);
    diff = 0.1 + clamp(dot(normScreen.xyz, lightDir), 0.0, 0.9);
}
