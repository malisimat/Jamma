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

    // Local model height runs from y=0 at the top to y=-470 at the bottom.
    // A raised cosine gives a narrow profile at the caps and a wide profile in the middle.
    float y01 = clamp((-PositionIN.y) / 470.0, 0.0, 1.0);
    float radialProfile = 0.5 - 0.5 * cos(6.28318530718 * y01);

    // Radius expansion around the Y axis, modulated by height.
    float radiusScale = 0.5 + 4.0 * stationLevel * radialProfile;

    vec3 pos = PositionIN;
    pos.xz *= radiusScale;

    Normal = NormalIN;
    Uv = UvIN;
    WorldPos = pos;
    StationLevelOut = stationLevel;

    gl_Position = MVP * vec4(pos, 1.0);
}
