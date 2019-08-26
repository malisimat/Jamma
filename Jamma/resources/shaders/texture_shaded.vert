#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;

out vec2 UV;
out float diff;

uniform mat4 MVP;

void main()
{
    gl_Position =  MVP * vec4(PositionIN,1);
    UV = UvIN;

	vec3 lightDir = normalize(vec3(1.0, -0.5, -0.3));
	diff = 0.1 + clamp(dot(NormalIN, lightDir), 0, 0.9);
}