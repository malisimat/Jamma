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

	vec3 frameColour  = mix(vec3(0.25, 0.27, 0.24), vec3(0.26, 0.25, 0.28), MidiColours);
	vec3 backingColour = mix(vec3(0.16, 0.18, 0.16), vec3(0.18, 0.17, 0.19), MidiColours);

	vec3 colour = frameColour;
	if (PartKind > 0.5)
		colour = backingColour;

	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour * SceneDim, OverlayAlpha * 0.20 * partAlpha * mix(1.0, 0.75, MidiColours));
}
