#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;

out vec2 UV;
out vec3 Rgb;

uniform mat4 MVP;
uniform float DX;
uniform float DY;
uniform int NumInstances;
uniform int InstanceOffset;

vec3 hsv2rgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

void main()
{
    float instanceId = float(gl_InstanceID + InstanceOffset);
    vec2 offset = vec2(DX * instanceId, DY * instanceId);
    vec4 pos = vec4(PositionIN.x + offset.x, PositionIN.y + offset.y, PositionIN.z, 1);
    gl_Position = MVP * pos;

    UV = UvIN;
    float h = instanceId / float(NumInstances-1);
    float hue = clamp(0.3 - (h * 0.4), 0.0, 0.8);
	Rgb = hsv2rgb(vec3(hue, 1., 1.));
}