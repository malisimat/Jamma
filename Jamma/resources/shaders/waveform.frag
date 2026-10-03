#version 330 core

in vec2 UV;
in float diff;
in float EditorU;
in float EditorMorphV;
in vec3 EditorLocalPosition;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform int LoopState;
uniform float LoopHover;
uniform float LoopSelected;
uniform float LoopPressed;
uniform float Highlight;
uniform float HighlightPass;
uniform float EditorPlayFrac;
uniform float SceneDim;
uniform float EditorActive;
uniform float EditorTime;
uniform samplerCube ProbeSampler;
uniform float ProbeStrength;
uniform vec3 EditorProbeEye;

void main()
{
    gl_FragDepth = gl_FragCoord.z;
    if (HighlightPass > 0.5)
    {
        ColorOUT = vec4(Highlight);
        return;
    }

    float ambient = 0.04;
    float diffScale = 0.2 + diff;
    float loopState = LoopState;

    vec3 texColor = texture(TextureSampler, UV).xyz;
    vec4 shadedColor = ambient + vec4(diffScale * texColor, 1.0);
    vec4 recColor = shadedColor + vec4(diffScale * vec3(8.0, 3.0, 0.5), 1.0);
    vec4 muteColor = ambient + vec4(diffScale * vec3(0.6, 0.6, 0.6), 0.2);

    float muteFade = max(loopState - 1.0, 0.0);
    float recFade = 0.2 * (1.0 - mod(min(loopState, 1.0), 2.0));

    ColorOUT = recFade * recColor +
        muteFade * muteColor +
        max(1.0 - (muteFade + recFade), 0.0) * shadedColor;
    // Compress the recording peaks before applying interaction colours. The
    // old additive hover term clipped broad sections of the waveform white.
    ColorOUT.rgb = ColorOUT.rgb / (vec3(1.0) + 0.55 * ColorOUT.rgb);
    float selected = clamp(LoopSelected, 0.0, 1.0);
    float hovered = clamp(LoopHover, 0.0, 1.0);
    ColorOUT.rgb = min(ColorOUT.rgb * (1.0 + 1.80 * selected)
        + vec3(0.07, 0.19, 0.23) * selected, vec3(1.0));
    ColorOUT.rgb = min(ColorOUT.rgb * (1.0 + (0.80 - 0.43 * selected) * hovered)
        + vec3(0.12 - 0.07 * selected) * hovered, vec3(1.0));
    ColorOUT.rgb = mix(ColorOUT.rgb, vec3(0.86, 0.38, 0.12),
        0.70 * clamp(LoopPressed, 0.0, 1.0));
    if (EditorActive > 0.5)
    {
        float ahead = fract(EditorU - EditorPlayFrac + 1.0);
        float distanceU = min(ahead, 1.0 - ahead);
        float pulse = 0.92 + 0.08 * sin(EditorTime * 3.0);
        float core = exp(-pow(distanceU / 0.0025, 2.0)) * pulse;
        float trail = exp(-ahead / 0.035);
        ColorOUT.rgb = mix(ColorOUT.rgb, ColorOUT.rgb * 0.82 + vec3(0.10, 0.22, 0.28),
            EditorMorphV);
        ColorOUT.rgb += (0.25 + 0.75 * EditorMorphV)
            * (core * vec3(0.35, 0.85, 1.0) + trail * vec3(0.03, 0.12, 0.18));
        if (EditorMorphV > 0.0)
        {
			vec3 towardEye = normalize(EditorProbeEye - EditorLocalPosition);
			vec3 reflected = reflect(-towardEye, vec3(0.0, 1.0, 0.0));
			vec3 probe = texture(ProbeSampler, reflected).rgb;
			float brightBand = smoothstep(0.52, 0.82,
				dot(probe, vec3(0.2126, 0.7152, 0.0722)));
			ColorOUT.rgb += ProbeStrength * EditorMorphV * 0.16
				* brightBand * probe;
            float timeCoord = EditorU * 16.0;
            float timeLine = 1.0 - smoothstep(0.0, fwidth(timeCoord) * 1.4,
                min(fract(timeCoord), 1.0 - fract(timeCoord)));
            ColorOUT.rgb += EditorMorphV * timeLine * vec3(0.07, 0.12, 0.16);
        }
    }
    ColorOUT.rgb *= mix(SceneDim, 1.0, EditorMorphV);
    if (EditorActive > 0.5)
        gl_FragDepth = gl_FragCoord.z * mix(1.0, 0.05, EditorMorphV);
}
