#version 330 core

in vec3 Normal;
in vec2 Uv;
in vec3 WorldPos;
flat in float StationLevelOut;

out vec4 ColorOUT;

uniform float Highlight;
uniform float HighlightPass;
uniform float StationHover;
uniform float StationPressed;
uniform vec3 StationStateColor;
uniform float RingScale;
uniform float SceneDim;

void main()
{
    if (HighlightPass > 0.5)
    {
        float alpha = clamp(Highlight, 0.0, 1.0);
        if (alpha <= 0.0) discard;
		ColorOUT = vec4(vec3(alpha * SceneDim), alpha);
		return;
	}
    const float brightPart = 4.0;
    vec3 lightDir = normalize(vec3(0.35, 0.82, 0.44));
    vec3 viewDir = normalize(vec3(0.0, 0.30, 1.0));
    vec3 normal = normalize(Normal);
    float diffuse = max(dot(normal, lightDir), 0.0);
    float partKind = Uv.y;

    if (partKind < brightPart + 0.5)
    {
        float radialDistance = length(WorldPos.xz);
        float outerHighlight = smoothstep(13.8 * RingScale, 16.2 * RingScale, radialDistance);
        vec3 halfDir = normalize(lightDir + viewDir);
        float specular = pow(max(dot(normal, halfDir), 0.0), 26.0);
        vec3 colour = StationStateColor * (0.62 + 0.48 * diffuse);
        colour += StationStateColor * (0.30 + 0.35 * StationLevelOut);
        colour += vec3(0.70) * specular * 0.50;
        colour += vec3(0.20) * outerHighlight * 0.18;
        colour = mix(colour, colour * 1.15 + vec3(0.05, 0.31, 0.36),
            clamp(Highlight, 0.0, 1.0));
        colour = min(colour * (1.0 + 0.25 * StationHover)
            + vec3(0.045) * StationHover, vec3(1.0));
        colour = mix(colour, vec3(1.0, 0.42, 0.11),
            0.72 * clamp(StationPressed, 0.0, 1.0));
        ColorOUT = vec4(colour * SceneDim, 1.0);
        return;
    }

    vec3 charcoal = vec3(0.10, 0.12, 0.14) * (0.58 + 0.40 * diffuse);
    charcoal += vec3(0.04, 0.14, 0.17) * clamp(Highlight, 0.0, 1.0);
    charcoal += vec3(0.07) * clamp(StationHover, 0.0, 1.0);
    charcoal = mix(charcoal, vec3(0.50, 0.21, 0.07),
        0.65 * clamp(StationPressed, 0.0, 1.0));
    ColorOUT = vec4(charcoal * SceneDim, 1.0);
}
