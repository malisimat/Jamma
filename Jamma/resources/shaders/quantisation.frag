#version 330 core

out vec4 ColorOUT;

uniform float Highlight;
uniform float OverlayAlpha;
uniform float Hue;

in float PartKind;
in float InstanceMode;
in float BandValue;
in vec3 Normal;
in vec3 LocalPosition;

vec3 HsvToRgb(vec3 hsv)
{
	vec3 p = abs(fract(hsv.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
	return hsv.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), hsv.y);
}

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
	if (PartKind > 2.5)
		partAlpha = 0.82;
	else if (PartKind > 1.5)
		partAlpha = 0.50;
	else if (PartKind > 0.5)
		partAlpha = 0.50;

	float hue = fract(Hue);
	vec3 frameColour = HsvToRgb(vec3(fract(hue + 0.07), 0.48, 0.92));
	vec3 rectColour = HsvToRgb(vec3(fract(hue - 0.06), 0.46, mix(0.30, 0.42, BandValue)));
	vec3 columnColour = HsvToRgb(vec3(hue, 0.42, 0.88));
	vec3 handleColour = HsvToRgb(vec3(hue, 0.24, 1.0));
	vec3 colour = frameColour;
	if (PartKind > 2.5)
		colour = handleColour;
	else if (PartKind > 1.5)
		colour = columnColour;
	else if (PartKind > 0.5)
		colour = rectColour;

	vec3 lightDir = normalize(vec3(-0.35, 0.55, 0.76));
	vec3 viewDir = normalize(vec3(0.0, 0.0, 1.0));
	float diffuse = max(dot(normalize(Normal), lightDir), 0.0);
	float specular = pow(max(dot(reflect(-lightDir, normalize(Normal)), viewDir), 0.0), 24.0);
	float verticalSheen = smoothstep(0.0, 0.9, abs(LocalPosition.y) / 138.0);
	colour *= mix(0.72, 1.16, diffuse);
	colour += vec3(specular * mix(0.10, 0.34, verticalSheen));
	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour, OverlayAlpha * partAlpha);
}