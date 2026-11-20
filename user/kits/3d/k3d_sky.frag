// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The sky (`k3d_vk.c`): the way each pixel looks, back through the view,
// and the horizon's colour turning to the zenith's as it rises.
#version 450

layout(push_constant) uniform Sky {
    mat4 unclip;        // clip space back to the world
    vec4 eye;           // xyz: the eye
    vec4 zenith;        // rgb
    vec4 horizon;       // rgb
} s;

layout(location = 0) in vec2 ndc;
layout(location = 0) out vec4 colour;

void main()
{
    vec4 far = s.unclip * vec4(ndc, 1.0, 1.0);
    vec3 look = normalize(far.xyz / far.w - s.eye.xyz);

    colour = vec4(mix(s.horizon.rgb, s.zenith.rgb, clamp(look.y, 0.0, 1.0)), 1.0);
}
