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
    // Ring meshes carry authored lathe face normals so the collar lighting
    // remains independent from the body cylinder's height-based expansion.
    Normal = NormalIN;
    Uv = UvIN;
    WorldPos = PositionIN;
    StationLevelOut = clamp(StationLevel, 0.0, 1.0);
    gl_Position = MVP * vec4(PositionIN, 1.0);
}
