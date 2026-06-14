#version 330 core

layout(location = 0) in vec3 PositionIN;

uniform mat4 MVP;
uniform float WaveformRadius;
uniform float WaveformUnitMeshRadius;

void main()
{
    float safeUnitRadius = max(WaveformUnitMeshRadius, 0.0001);
    float radiusScale = WaveformRadius / safeUnitRadius;
    vec2 scaledXZ = PositionIN.xz * radiusScale;
    gl_Position = MVP * vec4(scaledXZ.x, PositionIN.y, scaledXZ.y, 1.0);
}