#version 330 core

// Single attribute: x = circumferential position t in [0,1] around the loop,
// y = vertical edge selector (0 = curtain base, 1 = curtain top / crown).
layout(location = 0) in vec2 ParamIN;

out float vT;       // loop position 0..1
out float vEdge;    // 0 base .. 1 top
out float vHeight;  // sampled automation value 0..1

uniform mat4 MVP;

// Sparse control points (frac, value), piecewise-linear in loop space.
uniform vec2 AutoPoints[256];
uniform int  AutoPointCount;

uniform float LaneRadius;
uniform float LaneHeight;
uniform float PlayFrac;
uniform int   RenderMode; // 0 curtain, 1 crown, 2 playhead, 3 dot

const float TwoPi = 6.28318530718;

const int RenderModeCurtain = 0;
const int RenderModeCrown = 1;
const int RenderModePlayhead = 2;
const int RenderModeDot = 3;

float SampleAutomation(float t)
{
    if (AutoPointCount <= 0)
        return 0.0;
    if (t <= AutoPoints[0].x)
        return AutoPoints[0].y;

    int last = AutoPointCount - 1;
    if (t >= AutoPoints[last].x)
        return AutoPoints[last].y;

    for (int i = 0; i < 255; ++i)
    {
        if (i + 1 > last)
            break;

        vec2 lo = AutoPoints[i];
        vec2 hi = AutoPoints[i + 1];
        if (t >= lo.x && t <= hi.x)
        {
            float span = hi.x - lo.x;
            if (span <= 0.0)
                return hi.y;
            float f = (t - lo.x) / span;
            return mix(lo.y, hi.y, f);
        }
    }
    return AutoPoints[last].y;
}

void main()
{
    // Playhead and dot are pinned to the current play position; curtain and crown
    // sweep the whole loop using their per-vertex t.
    float t = (RenderMode >= RenderModePlayhead) ? PlayFrac : ParamIN.x;
    float edge = ParamIN.y;

    float h = SampleAutomation(t);
    vT = t;
    vEdge = edge;
    vHeight = h;

    float angle = TwoPi * t;
    float yTop = LaneHeight * h;
    float y = mix(0.0, yTop, edge);

    vec3 position = vec3(sin(angle) * LaneRadius, y, cos(angle) * LaneRadius);
    gl_Position = MVP * vec4(position, 1.0);

    if (RenderMode == RenderModeDot)
        gl_PointSize = 18.0;
}
