#version 330 core

in vec3 Normal;
in vec2 Uv;
in vec3 WorldPos;
flat in float StationLevelOut;

out vec4 ColorOUT;

uniform float Highlight;
uniform float StationHover;
uniform vec3 StationStateColor;

void main()
{
    const float brightPart = 4.0;
    vec3 lightDir = normalize(vec3(0.35, 0.82, 0.44));
    vec3 viewDir = normalize(vec3(0.0, 0.30, 1.0));
    vec3 normal = normalize(Normal);
    float diffuse = max(dot(normal, lightDir), 0.0);
    float partKind = Uv.y;

    if (partKind < brightPart + 0.5)
    {
        float radialDistance = length(WorldPos.xz);
        float outerHighlight = smoothstep(13.8, 16.2, radialDistance);
        vec3 halfDir = normalize(lightDir + viewDir);
        float specular = pow(max(dot(normal, halfDir), 0.0), 26.0);
        vec3 colour = StationStateColor * (0.62 + 0.48 * diffuse);
        colour += StationStateColor * (0.30 + 0.35 * StationLevelOut);
        colour += vec3(0.70) * specular * 0.50;
        colour += vec3(0.20) * outerHighlight * 0.18;
        colour += vec3(0.10, 0.16, 0.20) * clamp(Highlight, 0.0, 1.0);
        ColorOUT = vec4(colour, 1.0);
        return;
    }

    vec3 charcoal = vec3(0.10, 0.12, 0.14) * (0.58 + 0.40 * diffuse);
    charcoal += vec3(0.08, 0.11, 0.14) * min(0.08, 0.08 * max(Highlight, StationHover));
    ColorOUT = vec4(charcoal, 1.0);
}
