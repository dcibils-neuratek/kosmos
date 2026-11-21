// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// A model's surface (`k3d_vk.c`): its texture times its tint, lit by the
// sun where the sun reaches it and by the ambient share, and hazed by
// distance exactly as a mesh's colour is (`k3d_view.glsl`), so an aircraft
// sits in the same light as the ground under it.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "k3d_view.glsl"

layout(push_constant) uniform Model {
    K3D_VIEW_MEMBERS
    vec4 model[3];
    vec4 tint;
} v;

#define K3D_LIT
#include "k3d_view.glsl"

layout(set = 0, binding = 0) uniform sampler2D skin;

layout(location = 0) in vec3 world;
layout(location = 1) in vec3 facing;
layout(location = 2) in vec2 across;

layout(location = 0) out vec4 colour;

void main()
{
    vec4 albedo = texture(skin, across) * v.tint;

    // Nothing is blended yet, so a texel mostly transparent is a hole -
    // glTF's MASK at its default cutoff - rather than an opaque pane.
    if (albedo.a < 0.5) {
        discard;
    }
    // Nothing is culled, and a model's thin parts - a wing's skin, a
    // control surface - are seen from behind: there the surface faces the
    // other way, and is lit as the side the eye is on.
    vec3 n = normalize(facing);
    if (!gl_FrontFacing) {
        n = -n;
    }
    colour = vec4(k3d_hazed(world, albedo.rgb * k3d_light(world, n)), 1.0);
}
