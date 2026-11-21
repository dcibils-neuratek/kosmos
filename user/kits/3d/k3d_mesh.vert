// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The 3D Kit on Vulkan (`k3d_vk.c`, `docs/flight.md` F3): a mesh's
// corners through the view, and where each is in the world, for its light
// and its haze.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "k3d_view.glsl"

layout(push_constant) uniform View { K3D_VIEW_MEMBERS } v;

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 colour;

layout(location = 0) out vec3 world;
layout(location = 1) out vec3 facing;
layout(location = 2) out vec4 albedo;

void main()
{
    world = position;
    facing = normal;
    albedo = colour;
    gl_Position = v.clip * vec4(position, 1.0);
}
