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

	vec3 frameColour  = mix(vec3(0.38, 0.84, 0.63), vec3(0.43, 0.56, 0.78), MidiColours);
	vec3 backingColour = mix(vec3(0.25, 0.49, 0.40), vec3(0.25, 0.31, 0.41), MidiColours);

	vec3 colour = frameColour;
	if (PartKind > 0.5)
		colour = backingColour;

	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour * SceneDim, OverlayAlpha * partAlpha * mix(1.0, 0.75, MidiColours));
}
