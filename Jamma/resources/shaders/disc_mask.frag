#version 330 core

in vec2 UV;
out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform float DiscRepeatCount;
uniform float DiscSpiralPitch;
uniform float Opacity;
uniform float Highlight;
uniform float StationHover;
uniform float StationMuted;
uniform float SceneDim;
uniform float GroupPreview;
uniform vec3 GroupPreviewTint;

void main()
{
    // Each texture is one angular tile. Loop bands advance one twelfth of their
    // radial spacing per tile, reconnecting to the next band after a full turn.
    float turn = UV.x * DiscRepeatCount;
    float tileIndex = mod(floor(turn), DiscRepeatCount);
    vec4 surface = texture(TextureSampler,
        vec2(fract(turn), UV.y - tileIndex * DiscSpiralPitch));
    if (surface.a <= 0.001) discard;
    vec3 colour = surface.rgb;
    colour = mix(colour, min(colour * 1.3 + vec3(0.02, 0.12, 0.15), vec3(1.0)),
        clamp(Highlight, 0.0, 1.0));
    colour = mix(colour, min(colour * 1.25 + vec3(0.10, 0.09, 0.03), vec3(1.0)),
        clamp(StationHover, 0.0, 1.0) * 0.55);
    if (StationMuted > 0.5)
        colour = mix(colour, vec3(dot(colour, vec3(0.2126, 0.7152, 0.0722))), 0.65);
    if (GroupPreview < 0.0)
        colour *= 0.36;
    else if (GroupPreview > 0.0)
        colour = mix(colour, GroupPreviewTint, 0.72);
    ColorOUT = vec4(colour * SceneDim, surface.a * clamp(Opacity, 0.0, 1.0));
}
