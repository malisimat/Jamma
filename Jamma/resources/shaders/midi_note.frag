#version 330 core

in float Velocity;
in float Diff;
in vec3 ProbeNormal;
flat in float IsDisc;
flat in float IsEndCap;

out vec4 ColorOUT;

uniform int ObjectId;
uniform float Highlight;
uniform float LoopHover;
uniform float LoopSelected;
uniform float LoopPressed;
uniform float DiscAlpha;
uniform int RenderMode;
uniform sampler2D TextureSampler;
uniform sampler2D DiscProbeSampler;

const int RenderModeScene = 0;
const int RenderModePicker = 1;
const int RenderModeHighlight = 2;
const int RenderModeNotesOnly = 3;
const int RenderModeDiscOnly = 4;

void main()
{
    // Discard end-cap faces on full-circle disc instances: both caps map to
    // the same angle (0/2pi) and produce an ugly overlapping seam fin.
    if (IsDisc > 0.5 && (IsEndCap > 0.5 || !gl_FrontFacing))
        discard;

    if (RenderMode == RenderModeNotesOnly && IsDisc > 0.5)
        discard;

    if (RenderMode == RenderModeDiscOnly && IsDisc < 0.5)
        discard;

    if (RenderMode == RenderModePicker)
    {
        float r = ((ObjectId >> 16) & 0xff) / 255.0;
        float g = ((ObjectId >> 8) & 0xff) / 255.0;
        float b = (ObjectId & 0xff) / 255.0;
        ColorOUT = vec4(r, g, b, 1.0);
        return;
    }

    if (RenderMode == RenderModeHighlight)
    {
        ColorOUT = vec4(Highlight);
        return;
    }

    if (IsDisc > 0.5)
    {
        vec2 probeUv = clamp(ProbeNormal.xy * 0.49 + 0.5, 0.01, 0.99);
        vec3 discColor = texture(DiscProbeSampler, probeUv).rgb * (0.35 + 0.65 * Diff);
        float selected = clamp(LoopSelected, 0.0, 1.0);
        float hovered = clamp(LoopHover, 0.0, 1.0);
        float pressed = clamp(LoopPressed, 0.0, 1.0);
        discColor = min(discColor * (1.0 + 1.75 * selected)
            + vec3(0.12, 0.22, 0.25) * selected, vec3(1.0));
        discColor = min(discColor * (1.0 + (0.80 - 0.42 * selected) * hovered)
            + vec3(0.15 - 0.08 * selected) * hovered, vec3(1.0));
        discColor = mix(discColor, vec3(1.0, 0.42, 0.10), 0.82 * pressed);
        float discOpacity = max(DiscAlpha, 0.48) + 0.34 * selected
            + 0.18 * hovered + 0.52 * pressed;
        ColorOUT = vec4(discColor, min(discOpacity, 1.0));
        return;
    }

    vec3 low = vec3(0.08, 0.42, 1.0);
    vec3 mid = vec3(0.04, 1.0, 0.35);
    vec3 warm = vec3(1.0, 0.84, 0.03);
    vec3 hot = vec3(1.0, 0.04, 0.02);
    vec3 baseColor = mix(low, mid, smoothstep(0.0, 0.38, Velocity));
    baseColor = mix(baseColor, warm, smoothstep(0.38, 0.56, Velocity));
    // Velocity 90/127 reaches the red end of the loop-grid editor palette.
    baseColor = mix(baseColor, hot, smoothstep(0.56, 0.70, Velocity));
    vec2 probeUv = clamp(ProbeNormal.xy * 0.49 + 0.5, 0.01, 0.99);
    vec3 probe = texture(TextureSampler, probeUv).rgb;
    vec3 noteColor = baseColor * (0.18 + 0.82 * probe) * (0.45 + 0.55 * Diff);
    noteColor *= 1.0 + 0.15 * clamp(LoopSelected, 0.0, 1.0);
    noteColor = min(noteColor * (1.0 + 0.20 * LoopHover)
        + vec3(0.04) * LoopHover, vec3(1.0));
    noteColor = mix(noteColor, vec3(1.0, 0.43, 0.10),
        0.75 * clamp(LoopPressed, 0.0, 1.0));
    ColorOUT = vec4(noteColor, 0.88);
}
