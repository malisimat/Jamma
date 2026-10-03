#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;

uniform mat4 MVP;
uniform float StationLevel;
uniform int StationVisualState;
uniform float RingCapY;
uniform float RingDirection;
uniform float RingScale;

out vec3 Normal;
out vec2 Uv;
out vec3 WorldPos;
flat out float StationLevelOut;

const int OccluderInstances = 20;
const float TwoPi = 6.283185307179586;

vec2 windowRange(int boundary)
{
    float angle = TwoPi * float(boundary) / float(OccluderInstances);
    float center = -8.7;
    float halfHeight = 2.2;
    if (StationVisualState == 0)
    {
        center += 0.7 * sin(4.0 * angle);
        halfHeight += 0.35 * cos(4.0 * angle);
    }
    else if (StationVisualState == 1)
    {
        center += 0.8 * sin(10.0 * angle);
        halfHeight += 0.45 * cos(10.0 * angle);
    }
    else if (StationVisualState == 2)
    {
        center += 1.8 * sin(4.0 * angle);
        halfHeight += 0.65 * cos(4.0 * angle);
    }
    else if (StationVisualState == 3)
    {
        center += 1.2 * sin(5.0 * angle);
        halfHeight += 0.55 * cos(5.0 * angle);
    }
    else if (StationVisualState == 4)
    {
        center += 1.5 * sin(4.0 * angle);
        halfHeight += 0.7 * sin(8.0 * angle);
    }
    else
    {
        center += 1.7 * sin(8.0 * angle);
        halfHeight += 0.45 * cos(4.0 * angle);
    }
    return vec2(center - halfHeight, center + halfHeight);
}

void main()
{
    vec3 pos = PositionIN;
    vec3 normal = NormalIN;
    if (UvIN.y > 4.5)
    {
        int endpoint = PositionIN.z > 0.5 ? 1 : 0;
        int boundary = (gl_InstanceID + endpoint) % OccluderInstances;
        float azimuth = TwoPi * float(boundary) / float(OccluderInstances);
        vec2 window = windowRange(boundary);
        bool upperBar = UvIN.x < 0.5;
        float localY = upperBar
            ? mix(-3.0 * RingScale, window.y * RingScale, PositionIN.y)
            : mix(window.x * RingScale, -15.7 * RingScale, PositionIN.y);
        pos = vec3(cos(azimuth) * PositionIN.x,
            RingCapY + RingDirection * localY,
            sin(azimuth) * PositionIN.x);

        float normalAngle = TwoPi * (float(gl_InstanceID) + 0.5) / float(OccluderInstances);
        normal = vec3(
            cos(normalAngle) * NormalIN.x - sin(normalAngle) * NormalIN.z,
            RingDirection * NormalIN.y,
            sin(normalAngle) * NormalIN.x + cos(normalAngle) * NormalIN.z);
    }

    Normal = normal;
    Uv = UvIN;
    WorldPos = pos;
    StationLevelOut = clamp(StationLevel, 0.0, 1.0);
    gl_Position = MVP * vec4(pos, 1.0);
}
