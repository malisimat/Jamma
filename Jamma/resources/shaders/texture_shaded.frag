#version 330 core

in vec2 UV;
in float diff;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform int LoopState;
uniform float LoopHover;

void main()
{
	float ambient = 0.04;
	float diffScale = 0.2 + diff;
	float loopState = LoopState;
    vec3 texColor = texture(TextureSampler, UV).xyz;
    vec4 shadedColor = ambient + vec4(diffScale * texColor, 1.0);
	vec4 recColor = shadedColor + vec4(diffScale * vec3(8.0, 3.0, 0.5), 1.0);
	vec4 muteColor = ambient + vec4(diffScale * vec3(0.6, 0.6, 0.6), 0.2);

	float muteFade = max(loopState - 1.0, 0.0);
	float recFade = 0.2 * (1.0 - mod(min(loopState, 1.0), 2.0));
	ColorOUT = recFade * recColor + 
		muteFade * muteColor +
		max(1.0 - (muteFade + recFade), 0.0) * shadedColor +
		LoopHover * vec4(0.5, 0.6, 0.4, 1.0);
}