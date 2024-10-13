#version 330 core

in vec2 UV;
in vec3 Rgb;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;

void main()
{
    //ColorOUT = texture(TextureSampler, UV);
	ColorOUT.rgb = Rgb;
	ColorOUT.a = 1.0;
}