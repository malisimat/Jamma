#version 330 core

in vec2 UV;
in vec3 Rgb;

out vec4 ColorOUT;
uniform float Opacity;
uniform float SceneDim;

void main()
{
	ColorOUT.rgb = Rgb * SceneDim;
	ColorOUT.a = Opacity;
}
