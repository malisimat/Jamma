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
out float EditorU;
out float EditorPitchRow;
out float EditorMorphV;

uniform mat4 MVP;
uniform float EditorMorph;
uniform int EditorBottomPitch;
uniform int EditorVisibleRows;
uniform float EditorGridRadius;

const float TwoPi = 6.28318530718;

void main()
{
    float startFrac = InstanceTimePitch.x;
    float durationFrac = InstanceTimePitch.y;
    float pitchOffset = InstanceTimePitch.z;
    Velocity = InstanceTimePitch.w;
    IsDisc = InstanceShape.w > 0.5 ? 1.0 : 0.0;
    IsEndCap = (UvIN.y > 1.5) ? 1.0 : 0.0;

    float angle = TwoPi * (startFrac + (PositionIN.x * durationFrac));
    float radius = InstanceShape.x + (PositionIN.z * InstanceShape.y);
    float height = InstanceShape.z;

    vec3 position = vec3( 
        sin(angle) * radius,
        pitchOffset + (PositionIN.y * height),
        cos(angle) * radius);

    float pitch = -InstanceShape.w - 1.0;
    float rows = max(float(EditorVisibleRows), 1.0);
    float width = EditorGridRadius * 2.0;
    float gridU = startFrac + PositionIN.x * durationFrac;
    float row = pitch - float(EditorBottomPitch) + 0.5;
    float gridZ = IsDisc > 0.5
        ? -PositionIN.z * EditorGridRadius * 0.78
        : -((row / rows) * 2.0 - 1.0) * EditorGridRadius * 0.78
            + PositionIN.z * EditorGridRadius * 1.56 / rows * 0.40;
    float gridY = (IsDisc > 0.5 ? 0.0 : 3.0 + Velocity * 4.0)
        + PositionIN.y * (IsDisc > 0.5 ? 3.0 : height);
    vec3 gridPosition = vec3((gridU - 0.5) * width, gridY, gridZ);
    gl_Position = MVP * vec4(mix(position, gridPosition, EditorMorph), 1.0);
    EditorU = gridU;
    EditorPitchRow = IsDisc > 0.5
        ? (PositionIN.z + 1.0) * rows * 0.5 : row;
    EditorMorphV = EditorMorph;

    // The generated MIDI mesh carries inward-facing normal sign on Y, so flip
    // it here to keep lighting oriented outward on notes and the center disc.
    vec3 radialNormal = normalize(vec3(sin(angle), -NormalIN.y * 0.35, cos(angle)));
    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    vec4 normScreen = MVP * vec4(radialNormal, 0.0);
    Diff = 0.15 + clamp(dot(normScreen.xyz, lightDir), 0.0, 0.85);
}
