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
	// Gate instances (InstanceMode == 0) render frame/backing geometry.
	// Column instances (InstanceMode == 1) render the origin marker.
	bool columnOnlyPart = PartKind > 1.5 && PartKind < 2.5;
	bool columnInstance = InstanceMode > 0.5;
	if (columnOnlyPart != columnInstance)
		discard;

	float partAlpha = 0.34;
	if (PartKind > 1.5)
		partAlpha = 0.58;
	else if (PartKind > 0.5)
		partAlpha = 0.48;

	vec3 frameColour  = vec3(0.62, 0.70, 0.90);
	vec3 backingColour = vec3(0.18, 0.22, 0.40);
	vec3 columnColour = vec3(0.80, 0.88, 1.00);

	vec3 colour = frameColour;
	if (PartKind > 1.5)
		colour = columnColour;
	else if (PartKind > 0.5)
		colour = backingColour;

	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour, OverlayAlpha * partAlpha);
}