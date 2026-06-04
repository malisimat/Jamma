#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;

uniform mat4 MVP;
uniform float StationLevel;

out vec3 Normal;
out vec2 Uv;
out vec3 WorldPos;
flat out float StationLevelOut;

void main()
{
    float stationLevel = pow(clamp(StationLevel, 0.0, 1.0), 0.5);

    // Radius expansion around the Y axis.
    float radiusScale = 0.5 + 5.0 * stationLevel;

    vec3 pos = PositionIN;
    pos.xz *= radiusScale;

    Normal = NormalIN;
    Uv = UvIN;
    WorldPos = pos;
    StationLevelOut = stationLevel;

    gl_Position = MVP * vec4(pos, 1.0);
}
