#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;
layout(location = 3) in vec4 InstanceTransformIN;
layout(location = 4) in float InstanceModeIN;
layout(location = 5) in float InstanceBandIN;

uniform mat4 MVP;

out float PartKind;
out float InstanceMode;
out float BandValue;
out vec3 Normal;
out vec3 LocalPosition;

void main()
{
	float angle = InstanceTransformIN.x;
	float c = cos(angle);
	float s = sin(angle);
	vec3 local = PositionIN;
	local.y = (local.y * InstanceTransformIN.z) + InstanceTransformIN.y;
	vec2 rotatedXZ = mat2(c, -s, s, c) * (local.xz * InstanceTransformIN.w);
	vec2 rotatedNormalXZ = mat2(c, -s, s, c) * NormalIN.xz;
	PartKind = UvIN.x;
	InstanceMode = InstanceModeIN;
	BandValue = InstanceBandIN;
	Normal = normalize(vec3(rotatedNormalXZ.x, NormalIN.y, rotatedNormalXZ.y));
	LocalPosition = PositionIN;
	gl_Position = MVP * vec4(rotatedXZ.x, local.y, rotatedXZ.y, 1.0);
}