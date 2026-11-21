// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The sky (`k3d_vk.c`): the way each pixel looks, back through the view,
// and the horizon's colour turning to the zenith's as it rises - and,
// when the view asks for a glow (Flight's time of day, F6a), the sun's
// disc and the light around it in the sun's colour, strongest along the
// horizon under it, which is what a dusk is.
#version 450

layout(push_constant) uniform Sky {
    mat4 unclip;        // clip space back to the world
    vec4 eye;           // xyz: the eye
    vec4 zenith;        // rgb
    vec4 horizon;       // rgb
    vec4 sun;           // xyz: towards the sun; w: the glow, 0 none
    vec4 light;         // rgb: the sun's colour
} s;

layout(location = 0) in vec2 ndc;
layout(location = 0) out vec4 colour;

void main()
{
    vec4 far = s.unclip * vec4(ndc, 1.0, 1.0);
    vec3 look = normalize(far.xyz / far.w - s.eye.xyz);
    float up = clamp(look.y, 0.0, 1.0);

    // A low sun's glow (above the day's 0.3) makes the horizon's colour a
    // band, the zenith's coming within twenty degrees, rather than a
    // straight blend over the whole quarter; the day's sky keeps the
    // straight blend.
    up = mix(up, 1.0 - pow(1.0 - up, 3.0), clamp((s.sun.w - 0.3) * 2.0, 0.0, 1.0));
    vec3 sky = mix(s.horizon.rgb, s.zenith.rgb, up);

    if (s.sun.w > 0.0) {
        float toward = max(dot(look, s.sun.xyz), 0.0);
        float low = 1.0 - clamp(look.y * 3.0, 0.0, 1.0);    // near the horizon

        sky += s.light.rgb * s.sun.w * (0.9 * pow(toward, 4.0) * low + 0.35 * pow(toward, 64.0));
        sky = mix(sky, s.light.rgb + 0.25, smoothstep(0.9994, 0.9997, toward) * s.sun.w);
    }
    colour = vec4(min(sky, vec3(1.0)), 1.0);
}
