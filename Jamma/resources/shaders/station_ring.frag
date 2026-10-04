#version 330 core

in vec3 Normal;
in vec3 ProbeNormal;
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
uniform float SelectionActive;
uniform sampler2D MaterialProbeSampler;

// Keep the normal palette when selection is empty; retain a little hue otherwise.
vec3 selectionColour(vec3 colour, float selected)
{
    float dull = clamp(SelectionActive, 0.0, 1.0) * (1.0 - clamp(selected, 0.0, 1.0));
    float luminance = dot(colour, vec3(0.2126, 0.7152, 0.0722));
    return mix(colour, mix(vec3(luminance), colour, 0.25) * 0.72, dull);
}

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
        float selected = clamp(Highlight, 0.0, 1.0);
        float hovered = clamp(StationHover, 0.0, 1.0);
        colour = min(colour * (1.0 + 1.20 * selected)
            + vec3(0.12, 0.29, 0.33) * selected, vec3(1.0));
        colour = selectionColour(colour, selected);
        colour = min(colour * (1.0 + (0.70 - 0.38 * selected) * hovered)
            + vec3(0.13 - 0.07 * selected) * hovered, vec3(1.0));
        colour = mix(colour, vec3(1.0, 0.42, 0.11),
            0.72 * clamp(StationPressed, 0.0, 1.0));
        ColorOUT = vec4(colour * SceneDim, 1.0);
        return;
    }

    // Keep the dark diffuse material and add the same reflection response
    // as the narrow end caps. Black probe pixels contribute no light.
    vec3 charcoal = vec3(0.10, 0.12, 0.14) * (0.58 + 0.40 * diffuse);
    vec3 probe = texture(MaterialProbeSampler, clamp(normalize(ProbeNormal).xy * 0.49 + 0.5, 0.01, 0.99)).rgb;
    charcoal += vec3(0.24, 0.27, 0.32) * probe;
    charcoal += vec3(0.09, 0.30, 0.34) * clamp(Highlight, 0.0, 1.0);
    charcoal = selectionColour(charcoal, Highlight);
    charcoal += vec3(0.16) * clamp(StationHover, 0.0, 1.0);
    charcoal = mix(charcoal, vec3(0.50, 0.21, 0.07),
        0.65 * clamp(StationPressed, 0.0, 1.0));
    ColorOUT = vec4(charcoal * SceneDim, 1.0);
}
