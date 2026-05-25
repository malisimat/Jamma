#version 330 core

layout(location = 0) in vec3 PositionIN;

uniform mat4 MVP;
uniform float AngleStep;
uniform float PhaseOffset;

void main()
{
	float angle = PhaseOffset + AngleStep * float(gl_InstanceID);
	float c = cos(-angle);
	float s = sin(-angle);
	vec2 rotatedXZ = mat2(c, -s, s, c) * PositionIN.xz;
	gl_Position = MVP * vec4(rotatedXZ.x, PositionIN.y, rotatedXZ.y, 1.0);
}