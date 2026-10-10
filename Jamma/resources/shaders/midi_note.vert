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
out float EditorU;
out float EditorPitchRow;
out float EditorMorphV;
out float EditorCrossNote;
out vec3 EditorLocalPosition;
flat out float EditorTopFace;
flat out vec3 EditorNoteHit;
flat out int EditorNoteInstance;

uniform mat4 MVP;
uniform mat4 ModelView;
uniform float EditorMorph;
uniform int EditorBottomPitch;
uniform int EditorVisibleRows;
uniform float EditorGridRadius;
uniform float EditorTimeOrigin;
uniform float EditorSeamHeadEnd;
uniform float EditorSeamTailStart;
uniform float EditorWrapCopy;
uniform int EditorHeldInstance;
uniform float EditorPreviewVelocity;

const float TwoPi = 6.28318530718;

void main()
{
    float startFrac = InstanceTimePitch.x;
    float durationFrac = InstanceTimePitch.y;
    float pitchOffset = InstanceTimePitch.z;
    Velocity = InstanceTimePitch.w;
    if (EditorHeldInstance == gl_InstanceID && InstanceShape.w < 0.5
        && EditorPreviewVelocity >= 0.0)
        Velocity = EditorPreviewVelocity;
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
    float editorStart = startFrac;
    float editorDuration = durationFrac;
    if (IsDisc < 0.5 && EditorSeamHeadEnd > 0.0)
    {
        float end = startFrac + durationFrac;
        if (startFrac < EditorSeamHeadEnd && end <= EditorSeamTailStart + 1e-6)
        {
            editorStart = EditorSeamTailStart;
            editorDuration = 1.0 + end - editorStart;
        }
        else if (startFrac >= EditorSeamTailStart - 1e-6 && abs(end - 1.0) < 1e-6)
            editorDuration += EditorSeamHeadEnd;
    }
    float displayStart = EditorTimeOrigin > 0.0
        ? fract(editorStart - EditorTimeOrigin + 1.0) : editorStart;
    float displayU = IsDisc > 0.5 ? gridU
        : displayStart + PositionIN.x * editorDuration + EditorWrapCopy;
    float row = pitch - float(EditorBottomPitch) + 0.5;
    float gridZ = IsDisc > 0.5
        ? -PositionIN.z * EditorGridRadius * 0.78
        : -((row / rows) * 2.0 - 1.0) * EditorGridRadius * 0.78
            + PositionIN.z * EditorGridRadius * 1.56 / rows * 0.40;
    float gridY = (IsDisc > 0.5 ? 0.0 : 3.0 + Velocity * 4.0)
        + PositionIN.y * (IsDisc > 0.5 ? 3.0 : height);
    vec3 gridPosition = vec3((displayU - 0.5) * width, gridY, gridZ);
    EditorLocalPosition = gridPosition;
    gl_Position = MVP * vec4(mix(position, gridPosition, EditorMorph), 1.0);
    // Fragment hover/playhead calculations remain in source coordinates.
    EditorU = IsDisc > 0.5 ? gridU + EditorTimeOrigin
        : editorStart + PositionIN.x * editorDuration;
    EditorPitchRow = IsDisc > 0.5
        ? (PositionIN.z + 1.0) * rows * 0.5 : row;
    EditorMorphV = EditorMorph;
    EditorCrossNote = PositionIN.z;
    EditorTopFace = NormalIN.y > 0.5 ? 1.0 : 0.0;
    EditorNoteHit = vec3(editorStart, editorStart + editorDuration, pitch);
    EditorNoteInstance = gl_InstanceID;

    // Preserve outward face signs, including the bottom and note end caps.
    vec3 tangent = vec3(cos(angle), 0.0, -sin(angle));
    vec3 radialNormal = normalize(vec3(sin(angle) * NormalIN.z, NormalIN.y,
        cos(angle) * NormalIN.z) + tangent * NormalIN.x);
    float topEdge = smoothstep(0.39, 0.5, abs(PositionIN.y));
    float radialEdge = smoothstep(0.76, 1.0, abs(PositionIN.z));
    float endEdge = (1.0 - IsDisc) * (1.0 - smoothstep(0.0, 0.035,
        min(PositionIN.x, 1.0 - PositionIN.x)));
    vec3 bevelNormal = normalize(radialNormal
        + vec3(0.0, sign(PositionIN.y) * topEdge * 0.45, 0.0)
        + vec3(sin(angle), 0.0, cos(angle)) * sign(PositionIN.z) * radialEdge * 0.25
        + tangent * (PositionIN.x < 0.5 ? -1.0 : 1.0) * endEdge * 0.4);
    // The backing plane reverses radial Z while notes retain their cross-section.
    vec3 gridNormal = vec3(NormalIN.x, NormalIN.y,
        NormalIN.z * (IsDisc > 0.5 ? -1.0 : 1.0));
    vec3 materialNormal = mix(bevelNormal, gridNormal, EditorMorph);
    // Opposing ring/grid side normals can cancel midway through the morph.
    if (dot(materialNormal, materialNormal) < 1e-6)
        materialNormal = gridNormal;
    ProbeNormal = normalize(mat3(ModelView) * materialNormal);
    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    vec4 normScreen = MVP * vec4(radialNormal, 0.0);
    float sceneDiff = 0.15 + clamp(dot(normScreen.xyz, lightDir), 0.0, 0.85);
    // On the flat grid, face normals give every note top the same light level.
    vec3 editorLightDir = normalize(vec3(-0.2, 0.9, 0.35));
    float editorDiff = 0.15 + 0.85 * max(dot(NormalIN, editorLightDir), 0.0);
    Diff = mix(sceneDiff, editorDiff, EditorMorph);
}
