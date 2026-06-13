#version 330 core

out vec4 ColorOUT;

uniform float Highlight;
uniform float OverlayAlpha;

in float PartKind;
in float InstanceMode;
in float BandValue;
in vec3 Normal;
in vec3 LocalPosition;

void main()
{
	// Gate instances (InstanceMode == 0) render FramePart (0), BackingPart (1),
	// and HandlePart (3). Column instances (InstanceMode == 1) render only
	// ColumnPart (2). The 1.5..2.5 band exclusively identifies ColumnPart.
	bool columnOnlyPart = PartKind > 1.5 && PartKind < 2.5;
	bool columnInstance = InstanceMode > 0.5;
	if (columnOnlyPart != columnInstance)
		discard;

	float partAlpha = 0.34;
	if (PartKind > 2.5)
		partAlpha = 0.82;
	else if (PartKind > 1.5)
		partAlpha = 0.50;
	else if (PartKind > 0.5)
		partAlpha = 0.50;

	// Fixed palette — no Hue uniform needed.
	vec3 frameColour  = vec3(0.62, 0.70, 0.90);
	vec3 rectColour   = mix(vec3(0.16, 0.20, 0.38), vec3(0.22, 0.28, 0.50), BandValue);
	vec3 columnColour = vec3(0.80, 0.88, 1.00);
	vec3 handleColour = vec3(1.00, 0.93, 0.74);

	vec3 colour = frameColour;
	if (PartKind > 2.5)
		colour = handleColour;
	else if (PartKind > 1.5)
		colour = columnColour;
	else if (PartKind > 0.5)
		colour = rectColour;

	// Diffuse + specular lighting only on the outward-facing handle strips.
	if (PartKind > 2.5)
	{
		vec3 lightDir = normalize(vec3(-0.35, 0.55, 0.76));
		vec3 viewDir  = normalize(vec3(0.0, 0.0, 1.0));
		float diffuse  = max(dot(normalize(Normal), lightDir), 0.0);
		float specular = pow(max(dot(reflect(-lightDir, normalize(Normal)), viewDir), 0.0), 24.0);
		float verticalSheen = smoothstep(0.0, 0.9, abs(LocalPosition.y) / 138.0);
		colour *= mix(0.72, 1.16, diffuse);
		colour += vec3(specular * mix(0.10, 0.34, verticalSheen));
	}

	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour, OverlayAlpha * partAlpha);
}