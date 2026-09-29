#version 330 core

in float Weight;
out vec4 ColorOUT;

uniform float EditorMorph;
uniform float SceneDim;

void main()
{
    gl_FragDepth = gl_FragCoord.z * mix(1.0, 0.05, EditorMorph);
    float reveal = smoothstep(0.58, 0.92, EditorMorph);
    ColorOUT = vec4(vec3(0.15, 0.30, 0.38) * Weight
        * mix(SceneDim, 1.0, EditorMorph),
        0.55 * Weight * reveal);
}
