#version 330 core

in vec2 UV;
in float diff;
in float EditorU;
in float EditorMorphV;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform int LoopState;
uniform float LoopHover;
uniform float EditorPlayFrac;
uniform float SceneDim;
uniform float EditorActive;
uniform float EditorTime;

void main()
{
    gl_FragDepth = gl_FragCoord.z;
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
        max(1.0 - (muteFade + recFade), 0.0) * shadedColor +
        LoopHover * vec4(0.5, 0.6, 0.4, 1.0);
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
