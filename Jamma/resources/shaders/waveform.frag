#version 330 core

in vec2 UV;
in vec3 ProbeNormal;
in vec3 ViewPosition;

out vec4 ColorOUT;

uniform sampler2D TextureSampler;
uniform sampler2D MaterialProbeSampler;
uniform mat4 ProbeView;
uniform int LoopState;
uniform float LoopHover;
uniform float LoopSelected;
uniform float LoopPressed;
uniform float Highlight;
uniform float HighlightPass;

void main()
{
    if (HighlightPass > 0.5)
    {
        ColorOUT = vec4(Highlight);
        return;
    }

    float ambient = 0.04;
    vec3 normal = normalize(ProbeNormal);
    // The sampled heights warp the top/bottom faces. Reflect from the actual
    // surface, without inventing bevels or dividing by the visual Y scale.
    vec3 surfaceNormal = cross(dFdx(ViewPosition), dFdy(ViewPosition));
    if (dot(surfaceNormal, surfaceNormal) > 1e-12)
    {
        surfaceNormal = normalize(surfaceNormal);
        normal = dot(surfaceNormal, normal) < 0.0 ? -surfaceNormal : surfaceNormal;
    }
    vec3 lightDir = normalize(vec3(0.0, 0.5, -0.3));
    float diffuse = max(dot(normal, lightDir), 0.0);
    float diffScale = 0.3 + 0.9 * diffuse;
    float loopState = LoopState;

    vec3 texColor = texture(TextureSampler, UV).xyz;
    float eyeDistance = length(ViewPosition);
    vec3 towardEye = eyeDistance > 0.0001 ? -ViewPosition / eyeDistance : vec3(0.0, 0.0, 1.0);
    vec3 reflected = reflect(-towardEye, normal);
    // Keep the environment fixed in the scene as the camera orbits. Looking
    // up the view-space ray directly makes reflections follow the camera.
    reflected = transpose(mat3(ProbeView)) * reflected;
    // The probe is a sphere image, so map the reflected view ray to its disc.
    float probeDenominator = 2.0 * length(reflected + vec3(0.0, 0.0, 1.0));
    vec2 probeUv = probeDenominator > 0.0001 ? reflected.xy / probeDenominator + 0.5 : vec2(0.5);
    probeUv = clamp(probeUv, 0.01, 0.99);
    vec3 probe = texture(MaterialProbeSampler, probeUv).rgb;
    float brightBand = smoothstep(0.20, 0.70, dot(probe, vec3(0.2126, 0.7152, 0.0722)));
    float fresnel = 0.65 + 0.35 * pow(1.0 - clamp(dot(normal, towardEye), 0.0, 1.0), 5.0);
    // Level colours keep a strong diffuse floor; the environment only adds light.
    vec4 shadedColor = vec4(ambient + (0.78 + 0.22 * diffuse) * texColor
        + 0.55 * fresnel * brightBand * probe, 1.0);
    vec4 recColor = shadedColor + vec4(diffScale * vec3(8.0, 3.0, 0.5), 1.0);
    vec4 muteColor = ambient + vec4(diffScale * vec3(0.6, 0.6, 0.6), 0.2);

    float muteFade = max(loopState - 1.0, 0.0);
    float recFade = 0.2 * (1.0 - mod(min(loopState, 1.0), 2.0));

    ColorOUT = recFade * recColor +
        muteFade * muteColor +
        max(1.0 - (muteFade + recFade), 0.0) * shadedColor;
    float selected = clamp(LoopSelected, 0.0, 1.0);
    if (selected > 0.0)
        ColorOUT.rgb = min(ColorOUT.rgb * (1.0 + 1.80 * selected)
            + vec3(0.07, 0.19, 0.23) * selected, vec3(1.0));
    ColorOUT += LoopHover * vec4(0.5, 0.6, 0.4, 1.0);
    ColorOUT.rgb = mix(ColorOUT.rgb, vec3(0.86, 0.38, 0.12),
        0.70 * clamp(LoopPressed, 0.0, 1.0));
}
