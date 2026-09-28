#version 330 core

in vec2 UV;
in float diff;
in vec3 ProbeNormal;
in vec3 ProbeRadial;
in vec3 ProbeUp;
in vec2 BevelEdge;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform sampler2D ProbeSampler;
uniform int LoopState;
uniform float LoopHover;

void main()
{
    float ambient = 0.04;
    float diffScale = 0.2 + diff;
    float loopState = LoopState;

    vec3 texColor = texture(TextureSampler, UV).xyz;
    float verticalBevel = 1.0 - smoothstep(0.0, 0.38, min(BevelEdge.x, 1.0 - BevelEdge.x));
    float radialBevel = 1.0 - smoothstep(0.0, 0.30, min(BevelEdge.y, 1.0 - BevelEdge.y));
    vec3 normal = normalize(ProbeNormal +
        ProbeUp * sign(BevelEdge.x - 0.5) * verticalBevel * 0.48 +
        ProbeRadial * sign(BevelEdge.y - 0.5) * radialBevel * 0.42);
    vec3 probe = texture(ProbeSampler, clamp(normal.xy * 0.49 + 0.5, 0.01, 0.99)).rgb;
    vec4 shadedColor = vec4(ambient + diffScale * texColor * (0.32 + 0.68 * probe), 1.0);
    vec4 recColor = shadedColor + vec4(diffScale * vec3(8.0, 3.0, 0.5), 1.0);
    vec4 muteColor = ambient + vec4(diffScale * vec3(0.6, 0.6, 0.6), 0.2);

    float muteFade = max(loopState - 1.0, 0.0);
    float recFade = 0.2 * (1.0 - mod(min(loopState, 1.0), 2.0));

    ColorOUT = recFade * recColor +
        muteFade * muteColor +
        max(1.0 - (muteFade + recFade), 0.0) * shadedColor +
        LoopHover * vec4(0.5, 0.6, 0.4, 1.0);
}
