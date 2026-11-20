// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// A mesh's surface (`k3d_vk.c`): its colour lit by the sun and the sky's
// ambient share, then hazed by its distance from the eye.
#version 450

layout(push_constant) uniform View {
    mat4 clip;
    vec4 eye;
    vec4 sun;
    vec4 haze;
    vec4 range;
} v;

layout(location = 0) in vec3 world;
layout(location = 1) in vec3 facing;
layout(location = 2) in vec4 albedo;

layout(location = 0) out vec4 colour;

void main()
{
    float lit = v.sun.w + (1.0 - v.sun.w) * max(dot(normalize(facing), v.sun.xyz), 0.0);
    float far = clamp((distance(world, v.eye.xyz) - v.range.x) / (v.range.y - v.range.x),
                      0.0, 1.0);

    colour = vec4(mix(albedo.rgb * lit, v.haze.rgb, far), 1.0);
}
