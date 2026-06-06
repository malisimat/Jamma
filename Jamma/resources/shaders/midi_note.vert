#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;
layout(location = 3) in vec4 InstanceTimePitch;
layout(location = 4) in vec4 InstanceShape;

out float Velocity;
out float Diff;
flat out float IsDisc;
flat out float IsEndCap;

uniform mat4 MVP;

const float TwoPi = 6.28318530718;

void main()
{
    float startFrac = InstanceTimePitch.x;
    float durationFrac = InstanceTimePitch.y;
    float pitchOffset = InstanceTimePitch.z;
    Velocity = InstanceTimePitch.w;
    IsDisc = InstanceShape.w;
    IsEndCap = (UvIN.y > 1.5) ? 1.0 : 0.0;

    float angle = TwoPi * (startFrac + (PositionIN.x * durationFrac));
    float radius = InstanceShape.x + (PositionIN.z * InstanceShape.y);
    float height = InstanceShape.z;

    vec3 position = vec3( 
        sin(angle) * radius,
        pitchOffset + (PositionIN.y * height),
        cos(angle) * radius);

    gl_Position = MVP * vec4(position, 1.0);

    // The generated MIDI mesh carries inward-facing normal sign on Y, so flip
    // it here to keep lighting oriented outward on notes and the center disc.
    vec3 radialNormal = normalize(vec3(sin(angle), -NormalIN.y * 0.35, cos(angle)));
    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    vec4 normScreen = MVP * vec4(radialNormal, 0.0);
    Diff = 0.15 + clamp(dot(normScreen.xyz, lightDir), 0.0, 0.85);
}