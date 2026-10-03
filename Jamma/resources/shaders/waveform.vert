#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;

out vec2 UV;
out vec3 ProbeNormal;
out vec3 ViewPosition;

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

    vec4 position = vec4(scaledXZ.x, y, scaledXZ.y, 1.0);
    gl_Position = MVP * position;
    ViewPosition = (ModelView * position).xyz;
    float colorV = clamp(0.5 - (y * colorScale * WaveformColorMultiplier), 0.0, 1.0);
    UV = vec2(u, colorV);
    // The waveform can be scaled to zero in Y. Recover its orientation from
    // the unaffected X/Z axes so probe normals never inherit that scale.
    vec3 probeX = normalize(ModelView[0].xyz);
    vec3 probeZ = normalize(ModelView[2].xyz);
    mat3 probeBasis = mat3(probeX, normalize(cross(probeZ, probeX)), probeZ);
    ProbeNormal = normalize(probeBasis * NormalIN);
}
