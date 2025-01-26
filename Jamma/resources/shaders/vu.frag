#version 330 core

in vec2 UV;
in vec3 Rgb;

out vec4 ColorOUT;

void main()
{
	ColorOUT.rgb = Rgb;
	ColorOUT.a = 1.0;
}