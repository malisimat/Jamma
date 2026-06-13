#version 330 core

out vec4 ColorOUT;

uniform float Highlight;
uniform float OverlayAlpha;

in float BandValue;
in vec3 Normal;
in vec3 LocalPosition;

void main()
{
	vec3 baseA = vec3(0.98, 0.86, 0.60);
	vec3 baseB = vec3(0.92, 0.72, 0.40);
	vec3 colour = mix(baseA, baseB, BandValue);

	vec3 lightDir = normalize(vec3(-0.35, 0.55, 0.76));
	vec3 viewDir  = normalize(vec3(0.0, 0.0, 1.0));
	float diffuse  = max(dot(normalize(Normal), lightDir), 0.0);
	float specular = pow(max(dot(reflect(-lightDir, normalize(Normal)), viewDir), 0.0), 20.0);
	float verticalSheen = smoothstep(0.0, 0.9, abs(LocalPosition.y) / 138.0);
	colour *= mix(0.70, 1.20, diffuse);
	colour += vec3(specular * mix(0.12, 0.40, verticalSheen));

	colour *= mix(0.82, 1.18, clamp(Highlight, 0.0, 1.0));
	ColorOUT = vec4(colour, OverlayAlpha * 0.62);
}
