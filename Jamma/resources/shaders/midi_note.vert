#version 330 core

layout(location = 0) in vec3 PositionIN;
layout(location = 1) in vec2 UvIN;
layout(location = 2) in vec3 NormalIN;
layout(location = 3) in vec4 InstanceTimePitch;
layout(location = 4) in vec4 InstanceShape;

out float Velocity;
out float Diff;
out vec3 ProbeNormal;
flat out float IsDisc;
flat out float IsEndCap;

uniform mat4 MVP;
uniform mat4 ModelView;

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

    // The arc mesh is wound outward; keep its top, bottom and end-cap signs.
    vec3 tangent = vec3(cos(angle), 0.0, -sin(angle));
    vec3 radialNormal = normalize(vec3(sin(angle) * NormalIN.z, NormalIN.y, cos(angle) * NormalIN.z) +
        tangent * NormalIN.x);
    float topEdge = smoothstep(0.39, 0.5, abs(PositionIN.y));
    float radialEdge = smoothstep(0.76, 1.0, abs(PositionIN.z));
    float endEdge = (1.0 - IsDisc) * (1.0 - smoothstep(0.0, 0.035, min(PositionIN.x, 1.0 - PositionIN.x)));
    vec3 bevelNormal = normalize(radialNormal +
        vec3(0.0, sign(PositionIN.y) * topEdge * 0.45, 0.0) +
        vec3(sin(angle), 0.0, cos(angle)) * sign(PositionIN.z) * radialEdge * 0.25 +
        tangent * (PositionIN.x < 0.5 ? -1.0 : 1.0) * endEdge * 0.4);
    ProbeNormal = normalize(mat3(ModelView) * bevelNormal);
    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    vec4 normScreen = MVP * vec4(radialNormal, 0.0);
    Diff = 0.15 + clamp(dot(normScreen.xyz, lightDir), 0.0, 0.85);
}
