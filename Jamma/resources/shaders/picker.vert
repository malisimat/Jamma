#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;

uniform mat4 MVP;
uniform float WaveformRadius;
uniform float WaveformUnitMeshRadius;
uniform float EditorMorph;

void main()
{
    float safeUnitRadius = max(WaveformUnitMeshRadius, 0.0001);
    float radiusScale = WaveformRadius / safeUnitRadius;
    vec2 scaledXZ = PositionIN.xz * radiusScale;
    vec3 ringPosition = vec3(scaledXZ.x, PositionIN.y, scaledXZ.y);
    vec3 gridPosition = vec3((UvIN.x - 0.5) * WaveformRadius * 2.0,
        3.0 + (length(scaledXZ) - WaveformRadius) * 0.55,
        (UvIN.y * 2.0 - 1.0) * WaveformRadius * 0.78);
    gl_Position = MVP * vec4(mix(ringPosition, gridPosition, EditorMorph), 1.0);
}
