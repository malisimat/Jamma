#version 330 core

in float Velocity;
in float Diff;
flat in float IsDisc;
flat in float IsEndCap;
in float EditorU;
in float EditorPitchRow;
in float EditorMorphV;

out vec4 ColorOUT;

uniform int ObjectId;
uniform float Highlight;
uniform float LoopHover;
uniform float LoopSelected;
uniform float LoopPressed;
uniform float DiscAlpha;
uniform int RenderMode;
uniform float EditorPlayFrac;
uniform float EditorHoverU;
uniform int EditorHoverPitch;
uniform int EditorBottomPitch;
uniform int EditorVisibleRows;
uniform float SceneDim;
uniform float EditorActive;
uniform float EditorTime;

const int RenderModeScene = 0;
const int RenderModePicker = 1;
const int RenderModeHighlight = 2;
const int RenderModeNotesOnly = 3;
const int RenderModeDiscOnly = 4;

void main()
{
    gl_FragDepth = gl_FragCoord.z;
    // Discard end-cap faces on full-circle disc instances: both caps map to
    // the same angle (0/2pi) and produce an ugly overlapping seam fin.
    if (IsDisc > 0.5 && IsEndCap > 0.5 && EditorMorphV < 0.999)
        discard;

    if (RenderMode == RenderModeNotesOnly && IsDisc > 0.5)
        discard;

    if (RenderMode == RenderModeDiscOnly && IsDisc < 0.5)
        discard;
    if (EditorMorphV > 0.999 && IsDisc < 0.5 &&
        (EditorPitchRow < 0.0 || EditorPitchRow >= float(EditorVisibleRows)))
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
        ColorOUT = vec4(vec3(Highlight * mix(SceneDim, 1.0, EditorMorphV)), Highlight);
        return;
    }

    if (IsDisc > 0.5)
    {
        float selected = clamp(LoopSelected, 0.0, 1.0);
        float hovered = clamp(LoopHover, 0.0, 1.0);
        float pressed = clamp(LoopPressed, 0.0, 1.0);
        vec3 discColor = mix(vec3(0.46, 0.53, 0.65),
            vec3(0.06, 0.91, 0.96), selected) * Diff;
        discColor = min(discColor * (1.0 + 1.75 * selected)
            + vec3(0.12, 0.22, 0.25) * selected, vec3(1.0));
        discColor = min(discColor * (1.0 + (0.80 - 0.42 * selected) * hovered)
            + vec3(0.15 - 0.08 * selected) * hovered, vec3(1.0));
        discColor = mix(discColor, vec3(1.0, 0.42, 0.10),
            0.82 * pressed);
        // The ring is blended over the scene; a press needs near-opaque coverage
        // to read as clearly as the solid station mesh.
        float discOpacity = max(DiscAlpha, 0.48) + 0.34 * selected
            + 0.18 * hovered + 0.52 * pressed;
        ColorOUT = vec4(discColor, min(discOpacity, 1.0));
    }
    else
    {
        vec3 low = vec3(0.1, 0.45, 0.95);
        vec3 mid = vec3(0.1, 0.9, 0.55);
        vec3 high = vec3(1.0, 0.72, 0.18);
        float upper = smoothstep(0.45, 1.0, Velocity);
        vec3 baseColor = mix(mix(low, mid, smoothstep(0.0, 0.55, Velocity)), high, upper);
        vec3 noteColor = baseColor * Diff;
        noteColor = mix(noteColor, noteColor * 1.30 + vec3(0.02, 0.12, 0.14),
            clamp(LoopSelected, 0.0, 1.0));
        noteColor = min(noteColor * (1.0 + 0.20 * LoopHover)
            + vec3(0.04) * LoopHover, vec3(1.0));
        noteColor = mix(noteColor, vec3(1.0, 0.43, 0.10),
            0.75 * clamp(LoopPressed, 0.0, 1.0));
        ColorOUT = vec4(noteColor, 0.88);
    }
    if (EditorActive > 0.5)
    {
        float ahead = fract(EditorU - EditorPlayFrac + 1.0);
        float phaseDistance = min(ahead, 1.0 - ahead);
        float core = exp(-pow(phaseDistance / 0.0025, 2.0))
            * (0.92 + 0.08 * sin(EditorTime * 3.0));
        float trail = exp(-ahead / 0.032);
        if (IsDisc > 0.5)
        {
            // Derivative-filtered row and time rulers stay crisp under camera motion.
            float rowPhase = fract(EditorPitchRow);
            float rowLine = 1.0 - smoothstep(0.0, fwidth(EditorPitchRow) * 1.4,
                min(rowPhase, 1.0 - rowPhase));
            float major = 1.0 - smoothstep(0.0, fwidth(EditorPitchRow) * 1.6,
                min(fract(EditorPitchRow / 12.0), 1.0 - fract(EditorPitchRow / 12.0)));
            ColorOUT.rgb = mix(ColorOUT.rgb, vec3(0.028, 0.055, 0.082), EditorMorphV);
            ColorOUT.rgb += EditorMorphV * (rowLine * vec3(0.025, 0.05, 0.07)
                + major * vec3(0.04, 0.08, 0.10));
            if (EditorHoverU >= 0.0 && EditorHoverPitch >= 0)
            {
                float hover = 1.0 - smoothstep(0.0, 0.015, abs(EditorU - EditorHoverU));
                float pitchRow = float(EditorHoverPitch - EditorBottomPitch) + 0.5;
                hover *= 1.0 - smoothstep(0.4, 0.55, abs(EditorPitchRow - pitchRow));
                ColorOUT.rgb += hover * EditorMorphV * vec3(0.10, 0.35, 0.40);
            }
            ColorOUT.a = mix(ColorOUT.a, 0.93, EditorMorphV);
        }
        ColorOUT.rgb += (0.22 + 0.78 * EditorMorphV)
            * (core * vec3(0.35, 0.86, 1.0) + trail * vec3(0.025, 0.11, 0.16));
    }
    ColorOUT.rgb *= mix(SceneDim, 1.0, EditorMorphV);
    if (EditorActive > 0.5)
        gl_FragDepth = gl_FragCoord.z * mix(1.0, 0.05, EditorMorphV);
}
