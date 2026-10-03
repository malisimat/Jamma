#version 330 core

in vec3 Normal;
in vec3 ProbeNormal;
in vec2 Uv;
in vec3 WorldPos;
flat in float StationLevelOut;

out vec4 ColorOUT;

uniform float Highlight;
uniform float HighlightPass;
uniform float StationPressed;
uniform float StationHover;
uniform vec3 StationStateColor;
uniform sampler2D MaterialProbeSampler;

// uv.x = radial fraction on top/bevel, vertical fraction on side (0=bottom,1=top)
// uv.y = part kind:  0=deck-top, 1=bevel, 2=side
void main()
{
    if (HighlightPass > 0.5)
    {
        float alpha = clamp(Highlight, 0.0, 1.0);
        if (alpha <= 0.0) discard;
        ColorOUT = vec4(vec3(alpha), alpha);
        return;
    }
	float radialFrac = Uv.x;
	float partKind   = Uv.y;
	// accentuate small values
    float stationLevel = clamp(StationLevelOut, 0.0, 1.0);

	// -- base colour by part --
	vec3 deckColour  = vec3(0.22, 0.24, 0.27);
	vec3 bevelColour = vec3(0.28, 0.33, 0.40);
	vec3 sideColour  = vec3(0.26, 0.30, 0.36);

	vec3 base = deckColour;
	if      (partKind > 1.5) base = sideColour;
	else if (partKind > 0.5) base = bevelColour;

	// -- side-wall level gradient bands --
	if (partKind > 1.5 && partKind < 2.5)
	{
		float heightFrac = clamp(radialFrac, 0.0, 1.0);
		vec3 lowHue = mix(vec3(0.20, 0.90, 0.22), vec3(0.98, 0.92, 0.18), heightFrac);
		vec3 midHue = mix(vec3(0.98, 0.92, 0.18), vec3(0.98, 0.55, 0.08), heightFrac);
		vec3 highHue = mix(vec3(0.98, 0.55, 0.08), vec3(0.95, 0.10, 0.08), heightFrac);

		float yellowToOrange = smoothstep(0.25, 0.66, stationLevel);
		float orangeToRed = smoothstep(0.58, 1.00, stationLevel);

		vec3 lowMid = mix(lowHue, midHue, yellowToOrange);
		base = mix(lowMid, highHue, orangeToRed);
	}
	// -- cheap normal-based diffuse (single overhead light) --
	vec3 lightDir  = normalize(vec3(0.3, 1.0, 0.4));
	float diffuse  = clamp(dot(normalize(Normal), lightDir), 0.0, 1.0);
	base *= (0.90 + 0.35 * diffuse);
	if (partKind < 1.5 || partKind > 3.5)
	{
		vec3 probe = texture(MaterialProbeSampler, clamp(normalize(ProbeNormal).xy * 0.49 + 0.5, 0.01, 0.99)).rgb;
        // Reflected light is additive: dark probe regions leave the
        // diffuse material intact, and highlights brighten neutral caps.
        base *= 0.34;
        base += vec3(0.24, 0.27, 0.32) * probe;
	}

	// -- highlight flash (selection) --
	float hi = clamp(Highlight, 0.0, 1.0);
    float hover = clamp(StationHover, 0.0, 1.0);
    base = min(base * (1.0 + 1.65 * hi) + vec3(0.08, 0.27, 0.32) * hi,
        vec3(1.0));
    base = min(base * (1.0 + (0.70 - 0.35 * hi) * hover)
        + vec3(0.14 - 0.07 * hi) * hover, vec3(1.0));
    base = mix(base, vec3(0.90, 0.40, 0.13),
        0.68 * clamp(StationPressed, 0.0, 1.0));

    ColorOUT = vec4(base, 1.0);
}
