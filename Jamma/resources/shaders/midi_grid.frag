#version 330 core

in float Weight;
out vec4 ColorOUT;

uniform float EditorMorph;
uniform float SceneDim;

void main()
{
    gl_FragDepth = gl_FragCoord.z * mix(1.0, 0.05, EditorMorph);
    float reveal = smoothstep(0.58, 0.92, EditorMorph);
    if (Weight < 0.0)
    {
        vec3 tint = Weight < -1.5 ? vec3(1.0, 0.24, 0.32)
            : vec3(0.26, 0.92, 0.88);
        ColorOUT = vec4(tint, 0.44 * reveal);
        return;
    }
    ColorOUT = vec4(vec3(0.15, 0.30, 0.38) * Weight
        * mix(SceneDim, 1.0, EditorMorph),
        0.55 * Weight * reveal);
}
