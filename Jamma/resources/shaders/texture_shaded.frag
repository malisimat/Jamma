#version 330 core

in vec2 UV;
in float diff;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform int LoopState;

void main()
{
    vec4 texColor = 0.04 + vec4((0.2 + diff) * texture(TextureSampler, UV).xyz, 1.0);

	float muteFade = max(LoopState - 1.0, 0.0);
	float recFade = 0.2 * (1.0 - mod(LoopState, 2.0));
	ColorOUT = recFade * vec4(1.0f, 0.0f, 0.0f, 1.0) + 
		muteFade * vec4(0.3, 0.3, 0.3, 1.0) +
		(1.0 - muteFade) * texColor;
}