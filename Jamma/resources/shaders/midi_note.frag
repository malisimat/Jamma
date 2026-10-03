#version 330 core

in float Velocity;
in float Diff;
flat in float IsDisc;
flat in float IsEndCap;
in float EditorU;
in float EditorPitchRow;
in float EditorMorphV;
in float EditorCrossNote;
in vec3 EditorLocalPosition;
flat in float EditorTopFace;
flat in vec3 EditorNoteHit;
flat in int EditorNoteInstance;

out vec4 ColorOUT;

uniform int ObjectId;
uniform float Highlight;
uniform float LoopHover;
uniform float LoopSelected;
uniform float LoopPressed;
uniform float DiscAlpha;
uniform int RenderMode;
uniform int GeometryPass;
uniform float EditorPlayFrac;
uniform float EditorHoverU;
uniform int EditorHoverPitch;
uniform float EditorTargetStart;
uniform float EditorTargetEnd;
uniform int EditorTargetInstance;
uniform int EditorHeldInstance;
uniform int EditorBottomPitch;
uniform int EditorVisibleRows;
uniform float SceneDim;
uniform float EditorActive;
uniform float EditorGridRadius;
uniform float EditorWrapCopy;
uniform float EditorTime;
uniform samplerCube ProbeSampler;
uniform float ProbeStrength;
uniform vec3 EditorProbeEye;

const int RenderModeScene = 0;
const int RenderModePicker = 1;
const int RenderModeHighlight = 2;
const int RenderModeNotesOnly = 3;
const int RenderModeDiscOnly = 4;

void main()
{
    gl_FragDepth = gl_FragCoord.z;
    if (EditorWrapCopy != 0.0 && IsDisc > 0.5)
        discard;
    if (EditorMorphV > 0.999 && IsDisc < 0.5 &&
        abs(EditorLocalPosition.x) > EditorGridRadius)
        discard;
    // The ring draw range has no end caps. Back faces of its translucent
    // shell must not shine through the outward-facing surfaces.
    if (GeometryPass == 1 && (IsDisc < 0.5 ||
        (EditorMorphV < 0.5 && !gl_FrontFacing)))
        discard;
    if (GeometryPass == 2 && IsDisc > 0.5)
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
        vec3 low = vec3(0.08, 0.42, 1.0);
        vec3 mid = vec3(0.04, 1.0, 0.35);
        vec3 warm = vec3(1.0, 0.84, 0.03);
        vec3 hot = vec3(1.0, 0.04, 0.02);
        vec3 baseColor = mix(low, mid, smoothstep(0.0, 0.38, Velocity));
        baseColor = mix(baseColor, warm, smoothstep(0.38, 0.56, Velocity));
        // Velocity 90/127 is already at the red end of the palette.
        baseColor = mix(baseColor, hot, smoothstep(0.56, 0.70, Velocity));
        float diffuse = clamp((Diff - 0.15) / 0.85, 0.0, 1.0);
        vec3 noteColor = baseColor * (0.10 + 1.05 * pow(diffuse, 0.72));
        noteColor *= 1.0 + 0.15 * clamp(LoopSelected, 0.0, 1.0);
        noteColor = min(noteColor * (1.0 + 0.20 * LoopHover)
            + vec3(0.04) * LoopHover, vec3(1.0));
        noteColor = mix(noteColor, vec3(1.0, 0.43, 0.10),
            0.75 * clamp(LoopPressed, 0.0, 1.0));
        bool noteHovered = EditorActive > 0.5 && EditorTargetInstance == EditorNoteInstance
            && EditorHoverPitch == int(EditorNoteHit.z);
        bool noteHeld = EditorActive > 0.5 && EditorHeldInstance == EditorNoteInstance;
        float noteHover = (noteHovered && !noteHeld) ? EditorMorphV : 0.0;
        float noteDown = noteHeld ? EditorMorphV : 0.0;
        // Keep the changing velocity hue vivid, with warm pressed-state edges.
        vec3 heldColor = baseColor * (0.65 + 0.35 * diffuse);
        noteColor = mix(noteColor, heldColor, noteDown);
        // Let the top clip to white while the sides retain a little depth.
        float whiteLift = mix(0.55, 0.92, EditorTopFace);
        noteColor = min(noteColor * (1.0 + noteHover)
            + vec3(whiteLift * noteHover), vec3(1.0));
        float timeEdge = 1.0 - smoothstep(0.0, max(2.5 * fwidth(EditorU), 1e-6),
            min(EditorU - EditorNoteHit.x, EditorNoteHit.y - EditorU));
        float rowEdge = 1.0 - smoothstep(0.0, max(2.5 * fwidth(EditorCrossNote), 1e-6),
            1.0 - abs(EditorCrossNote));
        float outline = max(timeEdge, rowEdge) * EditorTopFace * max(noteHover, noteDown);
        vec3 edgeColor = noteHeld ? vec3(1.0, 0.63, 0.18) : vec3(0.08, 0.16, 0.20);
        noteColor = mix(noteColor, edgeColor, 0.92 * outline);
        ColorOUT = vec4(noteColor, 0.88 + 0.12 * max(noteHover, noteDown));
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
            // Use absolute MIDI pitch so bands and B/C boundaries follow pitch scrolling.
            float midiPitch = float(EditorBottomPitch) + EditorPitchRow;
            int pitchClass = int(mod(floor(midiPitch), 12.0));
            bool blackKey = pitchClass == 1 || pitchClass == 3 || pitchClass == 6
                || pitchClass == 8 || pitchClass == 10;
            vec3 rowColor = blackKey ? vec3(0.025, 0.049, 0.074)
                : vec3(0.038, 0.069, 0.098);
            ColorOUT.rgb = mix(ColorOUT.rgb, rowColor, EditorMorphV);
            ColorOUT.rgb += EditorMorphV * rowLine * vec3(0.025, 0.05, 0.07);
            // A solid ~2-pixel core with a narrow antialiased edge, never an octave tint.
            float octavePhase = mod(midiPitch, 12.0);
            float octavePixels = min(octavePhase, 12.0 - octavePhase)
                / max(fwidth(midiPitch), 1e-6);
            float octaveLine = 1.0 - smoothstep(1.0, 1.5, octavePixels);
            ColorOUT.rgb = mix(ColorOUT.rgb, vec3(0.15, 0.30, 0.38),
                octaveLine * EditorMorphV);
            if (EditorHoverU >= 0.0 && EditorHoverPitch >= 0)
            {
                // A negative target start means free timing: glow at the pointer, not a cell.
                float hover = EditorTargetInstance >= 0 ? 0.0
                    : EditorTargetStart < 0.0 ? 1.0 - smoothstep(0.0, 0.015, abs(EditorU - EditorHoverU))
                    : (fract(EditorU) >= EditorTargetStart && fract(EditorU) < EditorTargetEnd ? 1.0 : 0.0);
                float pitchRow = float(EditorHoverPitch - EditorBottomPitch) + 0.5;
                // Resolved cells fill the complete row with constant intensity.
                hover *= EditorTargetStart < 0.0
                    ? 1.0 - smoothstep(0.4, 0.55, abs(EditorPitchRow - pitchRow))
                    : (EditorPitchRow >= pitchRow - 0.5 && EditorPitchRow < pitchRow + 0.5 ? 1.0 : 0.0);
                ColorOUT.rgb += hover * EditorMorphV * vec3(0.10, 0.35, 0.40);
            }
            ColorOUT.a = mix(ColorOUT.a, 1.0, EditorMorphV);
			// A bright part of the sky probe glances across the editor's flat surface.
			vec3 towardEye = normalize(EditorProbeEye - EditorLocalPosition);
			vec3 reflected = reflect(-towardEye, vec3(0.0, 1.0, 0.0));
			vec3 probe = texture(ProbeSampler, reflected).rgb;
			float brightBand = smoothstep(0.52, 0.82,
				dot(probe, vec3(0.2126, 0.7152, 0.0722)));
			ColorOUT.rgb += ProbeStrength * EditorMorphV * 0.16
				* brightBand * probe;
        }
        float playheadTint = IsDisc > 0.5 ? 1.0 : 0.15;
        ColorOUT.rgb += playheadTint * (0.22 + 0.78 * EditorMorphV)
            * (core * vec3(0.35, 0.86, 1.0) + trail * vec3(0.025, 0.11, 0.16));
    }
    ColorOUT.rgb *= mix(SceneDim, 1.0, EditorMorphV);
    if (EditorActive > 0.5)
    {
        float editorDepth = gl_FragCoord.z * mix(1.0, 0.05, EditorMorphV);
        // Keep the captured low-velocity note legible over higher overlapping notes.
        if (IsDisc < 0.5 && EditorHeldInstance == EditorNoteInstance)
            editorDepth *= mix(1.0, 0.5, EditorMorphV);
        gl_FragDepth = editorDepth;
    }
}
