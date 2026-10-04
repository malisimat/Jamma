#version 330 core

in vec2 UV;
in vec3 ProbeNormal;
in vec3 ViewPosition;
in float EditorU;
in float EditorMorphV;
in vec3 EditorLocalPosition;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform sampler2D MaterialProbeSampler;
uniform mat4 ProbeView;
uniform int LoopState;
uniform float LoopHover;
uniform float LoopSelected;
uniform float LoopPressed;
uniform float LoopMuted;
uniform float Highlight;
uniform float HighlightPass;
uniform float EditorPlayFrac;
uniform float SceneDim;
uniform float SelectionActive;
uniform float EditorActive;
uniform float EditorTime;
uniform samplerCube ProbeSampler;
uniform float ProbeStrength;
uniform vec3 EditorProbeEye;

// Keep the normal palette when selection is empty; retain a little hue otherwise.
vec3 selectionColour(vec3 colour, float selected)
{
    float dull = clamp(SelectionActive, 0.0, 1.0) * (1.0 - clamp(selected, 0.0, 1.0));
    float luminance = dot(colour, vec3(0.2126, 0.7152, 0.0722));
    return mix(colour, mix(vec3(luminance), colour, 0.25) * 0.72, dull);
}

void main()
{
    gl_FragDepth = gl_FragCoord.z;
    if (HighlightPass > 0.5)
    {
        ColorOUT = vec4(Highlight);
        return;
    }

    float ambient = 0.04;
    vec3 normal = normalize(ProbeNormal);
    // The sampled heights warp the top/bottom faces. Reflect from the actual
    // surface, without inventing bevels or dividing by the visual Y scale.
    vec3 surfaceNormal = cross(dFdx(ViewPosition), dFdy(ViewPosition));
    if (dot(surfaceNormal, surfaceNormal) > 1e-12)
    {
        surfaceNormal = normalize(surfaceNormal);
        normal = dot(surfaceNormal, normal) < 0.0 ? -surfaceNormal : surfaceNormal;
    }
    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    float diffuse = max(dot(normal, lightDir), 0.0);
    float diffScale = 0.3 + 0.9 * diffuse;
    float loopState = LoopState;

    vec3 texColor = texture(TextureSampler, UV).xyz;
    float eyeDistance = length(ViewPosition);
    vec3 towardEye = eyeDistance > 0.0001 ? -ViewPosition / eyeDistance : vec3(0.0, 0.0, 1.0);
    vec3 reflected = reflect(-towardEye, normal);
    // Keep the environment fixed in the scene as the camera orbits. Looking
    // up the view-space ray directly makes reflections follow the camera.
    reflected = transpose(mat3(ProbeView)) * reflected;
    // The probe is a sphere image, so map the reflected view ray to its disc.
    float probeDenominator = 2.0 * length(reflected + vec3(0.0, 0.0, 1.0));
    vec2 probeUv = probeDenominator > 0.0001 ? reflected.xy / probeDenominator + 0.5 : vec2(0.5);
    probeUv = clamp(probeUv, 0.01, 0.99);
    vec3 probe = texture(MaterialProbeSampler, probeUv).rgb;
    float brightBand = smoothstep(0.20, 0.70, dot(probe, vec3(0.2126, 0.7152, 0.0722)));
    float fresnel = 0.65 + 0.35 * pow(1.0 - clamp(dot(normal, towardEye), 0.0, 1.0), 5.0);
    // Level colours keep a strong diffuse floor; the environment only adds light.
    // Keep material light subordinate to the interaction palette. Both probe
    // paths enter before peak compression and selected/hovered/pressed styling.
    vec3 materialLight = 0.12 * fresnel * brightBand * clamp(probe, 0.0, 1.0);
    if (EditorActive > 0.5 && EditorMorphV > 0.0)
    {
        vec3 editorTowardEye = normalize(EditorProbeEye - EditorLocalPosition);
        vec3 editorReflected = reflect(-editorTowardEye, vec3(0.0, 1.0, 0.0));
        vec3 editorProbe = texture(ProbeSampler, editorReflected).rgb;
        float editorBrightBand = smoothstep(0.52, 0.82,
            dot(editorProbe, vec3(0.2126, 0.7152, 0.0722)));
        materialLight += clamp(ProbeStrength, 0.0, 1.0) * EditorMorphV * 0.16
            * editorBrightBand * clamp(editorProbe, 0.0, 1.0);
    }
    vec4 shadedColor = vec4(ambient + (0.78 + 0.22 * diffuse) * texColor
        + materialLight, 1.0);
    vec4 recColor = shadedColor + vec4(diffScale * vec3(8.0, 3.0, 0.5), 1.0);
    vec4 muteColor = vec4(ambient + diffScale * vec3(0.6), 1.0);

    float muteFade = max(loopState - 1.0, 0.0);
    float recFade = 0.2 * (1.0 - mod(min(loopState, 1.0), 2.0));

    ColorOUT = recFade * recColor +
        muteFade * muteColor +
        max(1.0 - (muteFade + recFade), 0.0) * shadedColor;

    float selected = clamp(LoopSelected, 0.0, 1.0);
    float hovered = clamp(LoopHover, 0.0, 1.0);
    // Selection intensifies the level hue instead of dimming it or tinting cyan.
    float colourFloor = min(min(ColorOUT.r, ColorOUT.g), ColorOUT.b);
    // Removing too much of the neutral floor cancels the selection gain on
    // muted waveforms, making them identical to their idle state.
    ColorOUT.rgb -= vec3(colourFloor * 0.10 * selected);
    ColorOUT.rgb *= mix(1.35, 1.85, selected);
    // Scalar limiting preserves saturation, with more headroom for selection.
    float peak = max(max(ColorOUT.r, ColorOUT.g), ColorOUT.b);
    float ceiling = mix(0.82, 0.94, selected);
    ColorOUT.rgb *= min(1.0, ceiling / max(peak, 0.0001));
    ColorOUT.rgb = selectionColour(ColorOUT.rgb, selected);
    // Reserve the brightest hover for selected waveforms, including muted ones.
    ColorOUT.rgb = min(ColorOUT.rgb * (1.0 + 0.45 * hovered)
        + vec3(0.18 * hovered), vec3(1.0));
    peak = max(max(ColorOUT.r, ColorOUT.g), ColorOUT.b);
    float hoverCeiling = mix(ceiling, mix(0.90, 1.0, selected), hovered);
    ColorOUT.rgb *= min(1.0, hoverCeiling / max(peak, 0.0001));
    if (LoopMuted > 0.5)
    {
        // Preserve lighting/selection/hover variation inside a dark blue-grey palette.
        float luminance = clamp(0.60 * dot(ColorOUT.rgb, vec3(0.2126, 0.7152, 0.0722))
            + 0.16 * selected + 0.24 * hovered, 0.0, 1.0);
        ColorOUT.rgb = vec3(0.07, 0.10, 0.15) + vec3(0.18, 0.23, 0.30) * luminance;
    }
    vec3 pressColour = LoopPressed > 1.5 ? vec3(0.12, 0.42, 0.72) : vec3(0.86, 0.38, 0.12);
    ColorOUT.rgb = mix(ColorOUT.rgb, pressColour,
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
