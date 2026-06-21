#version 330 core

in float vT;       // loop position 0..1
in float vEdge;    // 0 base .. 1 top
in float vHeight;  // sampled automation value 0..1

out vec4 ColorOUT;

uniform vec3  LaneColor;
uniform float RecordGlow; // 0..1, lifts brightness while recording
uniform float PlayFrac;
uniform int   RenderMode; // 0 curtain, 1 crown, 2 playhead, 3 dot

const int RenderModeCurtain = 0;
const int RenderModeCrown = 1;
const int RenderModePlayhead = 2;
const int RenderModeDot = 3;

// Wrapped distance from the play head in loop space, mapped so the curve is
// brightest at the current play position and fades around the ring (loop time).
float PlayTrail(float t)
{
    float d = abs(t - PlayFrac);
    d = min(d, 1.0 - d);          // shortest way around the circle, 0..0.5
    return 1.0 - smoothstep(0.0, 0.5, d);
}

void main()
{
    float trail = PlayTrail(vT);

    if (RenderMode == RenderModeDot)
    {
        // Soft glowing play marker with a bright core and falloff halo.
        vec2 d = gl_PointCoord - vec2(0.5);
        float r = length(d) * 2.0;
        if (r > 1.0)
            discard;
        float core = 1.0 - smoothstep(0.0, 0.35, r);
        float halo = 1.0 - smoothstep(0.35, 1.0, r);
        vec3 col = mix(LaneColor, vec3(1.0), core * 0.8) + RecordGlow * 0.4;
        float a = clamp(core + halo * 0.5, 0.0, 1.0);
        ColorOUT = vec4(col, a);
        return;
    }

    if (RenderMode == RenderModePlayhead)
    {
        // Thin bright vertical line at the play position, fading toward the base.
        vec3 col = mix(LaneColor * 1.4, vec3(1.0), 0.5) + RecordGlow * 0.5;
        float a = mix(0.15, 0.95, vEdge);
        ColorOUT = vec4(col, a);
        return;
    }

    if (RenderMode == RenderModeCrown)
    {
        // Glowing top ring crown: bright, pulses up while recording.
        vec3 col = LaneColor * 1.6 + vec3(0.25) + RecordGlow * 0.6;
        float a = clamp(0.45 + 0.55 * trail + RecordGlow * 0.2, 0.0, 1.0);
        ColorOUT = vec4(col, a);
        return;
    }

    // Curtain: height-tinted body, brightest near the play head, with a bright
    // top edge band and a recording lift.
    vec3 low = LaneColor * 0.35;
    vec3 body = mix(low, LaneColor, vHeight);
    float topBand = smoothstep(0.82, 1.0, vEdge);
    vec3 col = body + topBand * (LaneColor * 0.6 + vec3(0.35));
    col += RecordGlow * 0.35 * (0.4 + 0.6 * trail);

    float alpha = mix(0.10, 0.55, trail);
    alpha = clamp(alpha + topBand * 0.4 + RecordGlow * 0.15, 0.0, 0.9);
    ColorOUT = vec4(col, alpha);
}
