#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;

uniform mat4 MVP;

out vec2 UV;

void main()
{
    UV = UvIN;
    gl_Position = MVP * vec4(PositionIN, 1.0);
}
