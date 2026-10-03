#version 330 core

layout(location = 0) in vec3 PositionIN;

uniform mat4 MVP;
uniform float StationLevel;

void main()
{
    float stationLevel = pow(clamp(StationLevel, 0.0, 1.0), 0.5);
    float y01 = clamp((-PositionIN.y) / 470.0, 0.0, 1.0);
    float radialProfile = 0.5 - 0.5 * cos(6.28318530718 * y01);
    float radiusScale = 0.5 + 4.0 * stationLevel * radialProfile;

    vec3 pos = PositionIN;
    pos.xz *= radiusScale;
    gl_Position = MVP * vec4(pos, 1.0);
}
