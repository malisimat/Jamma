#version 330 core

out vec4 ColorOUT;

flat in float MidiColours;

uniform float Highlight;
uniform float OverlayAlpha;
uniform float SceneDim;

in float PartKind;

void main()
{
	float partAlpha = 0.34;
	if (PartKind > 0.5)
		partAlpha = 0.48;

	vec3 frameColour  = mix(vec3(0.20, 0.78, 0.58), vec3(0.58, 0.40, 0.86), MidiColours);
	vec3 backingColour = mix(vec3(0.06, 0.27, 0.20), vec3(0.22, 0.12, 0.36), MidiColours);

	vec3 colour = frameColour;
	if (PartKind > 0.5)
		colour = backingColour;

	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour * SceneDim, OverlayAlpha * partAlpha);
}
