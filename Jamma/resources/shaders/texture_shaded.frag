#version 330 core

in vec2 UV;
in float diff;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;

void main()
{
    ColorOUT = 0.04 + vec4((0.2 + diff) * texture(TextureSampler, UV).xyz, 1.0);
	//ColorOUT = vec4(1.0, 0.3, 0.3, 1.0);
}