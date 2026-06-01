#version 330 core

in vec3 Normal;
in vec2 Uv;
in vec3 WorldPos;

out vec4 ColorOUT;

uniform float Highlight;
uniform float StationHover;

// uv.x = radial fraction 0..1  (0=center, 1=outer edge)
// uv.y = part kind:  0=deck-top, 1=bevel, 2=side, 3=rib
void main()
{
	float radialFrac = Uv.x;
	float partKind   = Uv.y;

	// -- base colour by part --
	vec3 deckColour  = vec3(0.13, 0.15, 0.17);   // graphite
	vec3 bevelColour = vec3(0.18, 0.22, 0.28);   // slate-blue bevel
	vec3 sideColour  = vec3(0.10, 0.12, 0.14);   // dark fascia
	vec3 ribColour   = vec3(0.28, 0.36, 0.50);   // muted steel rib

	vec3 base = deckColour;
	if      (partKind > 2.5) base = ribColour;
	else if (partKind > 1.5) base = sideColour;
	else if (partKind > 0.5) base = bevelColour;

	// -- cheap normal-based diffuse (single overhead light) --
	vec3 lightDir  = normalize(vec3(0.3, 1.0, 0.4));
	float diffuse  = clamp(dot(normalize(Normal), lightDir), 0.0, 1.0);
	base *= (0.55 + 0.45 * diffuse);

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

	// -- rib accent glow --
	if (partKind > 2.5)
	{
		base += vec3(0.05, 0.12, 0.22) * (0.4 + StationHover * 0.6);
	}

	// -- highlight flash (selection) --
	float hi = clamp(Highlight, 0.0, 1.0);
	base = mix(base, base + vec3(0.18, 0.28, 0.38), hi);

	ColorOUT = vec4(base, 1.0);
}
