#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 3) in vec4 InstanceTransformIN;
layout(location = 4) in float InstanceFillHalfWidthIN;

uniform mat4 MVP;

out float PartKind;

void main()
{
	float angle = InstanceTransformIN.x;
	float c = cos(angle);
	float s = sin(angle);
	vec3 local = PositionIN;
	if (UvIN.x > 2.5)
		local.x *= InstanceFillHalfWidthIN;
	local.y = (local.y * InstanceTransformIN.z) + InstanceTransformIN.y;
	vec2 rotatedXZ = mat2(c, -s, s, c) * (local.xz * InstanceTransformIN.w);
	PartKind = UvIN.x;
	gl_Position = MVP * vec4(rotatedXZ.x, local.y, rotatedXZ.y, 1.0);
}