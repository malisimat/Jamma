#version 330 core

out vec4 ColorOUT;

uniform float Highlight;
uniform float OverlayAlpha;

in float PartKind;
in float InstanceMode;

void main()
{
	// Gate and column geometry share the same VBO. Each instance is flagged
	// as either GateInstance or ColumnInstance. Discard fragments where the
	// geometry type (PartKind) does not match the instance type (InstanceMode)
	// to prevent gate instances rendering column faces and vice versa.
	bool columnPart = PartKind > 1.5;
	bool columnInstance = InstanceMode > 0.5;
	if (columnPart != columnInstance)
		discard;

	float partAlpha = 0.34;
	if (PartKind > 1.5)
		partAlpha = 0.46;
	else if (PartKind > 0.5)
		partAlpha = 0.58;

	vec3 frameColour = vec3(0.40, 0.48, 1.0);
	vec3 rectColour = vec3(0.22, 0.28, 0.72);
	vec3 columnColour = vec3(0.52, 0.58, 1.0);
	vec3 colour = frameColour;
	if (PartKind > 1.5)
		colour = columnColour;
	else if (PartKind > 0.5)
		colour = rectColour;

	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour, OverlayAlpha * partAlpha);
}