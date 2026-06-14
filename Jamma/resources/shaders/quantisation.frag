#version 330 core

out vec4 ColorOUT;

uniform float Highlight;
uniform float OverlayAlpha;

in float PartKind;

void main()
{
	float partAlpha = 0.34;
	if (PartKind > 0.5)
		partAlpha = 0.48;

	vec3 frameColour  = vec3(0.62, 0.70, 0.90);
	vec3 backingColour = vec3(0.18, 0.22, 0.40);

	vec3 colour = frameColour;
	if (PartKind > 0.5)
		colour = backingColour;

	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour, OverlayAlpha * partAlpha);
}