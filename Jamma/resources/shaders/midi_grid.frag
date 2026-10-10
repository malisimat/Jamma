#version 330 core

in float Weight;
in vec2 PreviewUv;
out vec4 ColorOUT;

uniform float EditorMorph;
uniform float SceneDim;

void main()
{
    gl_FragDepth = gl_FragCoord.z * mix(1.0, 0.05, EditorMorph);
    float reveal = smoothstep(0.58, 0.92, EditorMorph);
    if (Weight > 1.0)
    {
        // Subtle column lift preserves the existing black/white pitch bands.
        ColorOUT = vec4(vec3(0.23, 0.38, 0.48),
            (Weight > 2.5 ? 0.16 : 0.08) * reveal);
        return;
    }
    if (Weight < 0.0)
    {
        // Both add and remove paint share the down state, independent of hover.
        vec2 edgePixels = min(PreviewUv, vec2(1.0) - PreviewUv)
            / max(fwidth(PreviewUv), vec2(1e-6));
        float border = 1.0 - smoothstep(1.5, 2.5, min(edgePixels.x, edgePixels.y));
        if (Weight < -2.5)
        {
            // A destination ghost leaves the velocity body visible underneath.
            vec3 tint = mix(vec3(0.12, 0.65, 0.82), vec3(0.70, 0.96, 1.0), border);
            ColorOUT = vec4(tint, mix(0.12, 0.92, border) * reveal);
            return;
        }
        vec3 tint = mix(vec3(0.90, 0.40, 0.13), vec3(1.0, 0.72, 0.20), border);
        ColorOUT = vec4(tint, mix(0.82, 1.0, border) * reveal);
        return;
    }
    ColorOUT = vec4(vec3(0.15, 0.30, 0.38) * Weight
        * mix(SceneDim, 1.0, EditorMorph),
        0.55 * Weight * reveal);
}
