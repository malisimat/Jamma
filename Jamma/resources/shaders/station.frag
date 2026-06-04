#version 330 core

in vec3 Normal;
in vec2 Uv;
in vec3 WorldPos;
flat in float StationLevelOut;

out vec4 ColorOUT;

uniform float Highlight;
uniform float StationHover;

// uv.x = radial fraction on top/bevel, vertical fraction on side (0=bottom,1=top)
// uv.y = part kind:  0=deck-top, 1=bevel, 2=side
void main()
{
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
	if (partKind > 1.5)
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

	// -- luminous rim: thin bright ring near outer edge on bevel --
	float rimGlow = 0.0;
	if (partKind < 1.5)  // top and bevel only
	{
		// sharpen at radius ~0.93, fade in and out
		float rimMask = 1.0 - abs(radialFrac - 0.93) / 0.07;
		rimMask = clamp(rimMask, 0.0, 1.0);
		rimMask = rimMask * rimMask;  // square for sharper band

		vec3 rimColour = mix(vec3(0.2, 0.75, 0.9), vec3(0.9, 0.65, 0.2), radialFrac);
		rimGlow = rimMask * 0.55 * (1.0 + StationHover * 0.7);
		base = mix(base, rimColour, rimGlow);
	}

	// -- highlight flash (selection) --
	float hi = clamp(Highlight, 0.0, 1.0);
	base = mix(base, base + vec3(0.18, 0.28, 0.38), hi);

	ColorOUT = vec4(base, 1.0);
}
