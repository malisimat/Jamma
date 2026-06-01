#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;

uniform mat4 MVP;

out vec3 Normal;
out vec2 Uv;
out vec3 WorldPos;

void main()
{
	Normal   = NormalIN;
	Uv       = UvIN;
	WorldPos = PositionIN;
	gl_Position = MVP * vec4(PositionIN, 1.0);
}
