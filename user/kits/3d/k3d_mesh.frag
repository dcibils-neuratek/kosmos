// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// A mesh's surface (`k3d_vk.c`): its colour lit by the sun where the sun
// reaches it and by the sky's ambient share, then hazed by its distance
// from the eye (`k3d_view.glsl`).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "k3d_view.glsl"

layout(push_constant) uniform View { K3D_VIEW_MEMBERS } v;

#define K3D_LIT
#include "k3d_view.glsl"

layout(location = 0) in vec3 world;
layout(location = 1) in vec3 facing;
layout(location = 2) in vec4 albedo;

layout(location = 0) out vec4 colour;

void main()
{
    vec3 n = normalize(facing);

    colour = vec4(k3d_hazed(world, albedo.rgb * k3d_light(world, n)), 1.0);
}
