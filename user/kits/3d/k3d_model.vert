// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// A model (`k3d_vk.c`, `docs/flight.md` F5d): an aircraft's part, in its
// own coordinates, put in the world by its own matrix - so the whole moves
// with the aircraft and an aileron or a propeller with its hinge - then
// through the view as a mesh is (`k3d_mesh.vert`).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "k3d_view.glsl"

// The view, then the model's matrix as three affine rows - its last row
// is (0, 0, 0, 1), so 48 bytes rather than 64 keep the push at 256 - and
// its tint.
layout(push_constant) uniform Model {
    K3D_VIEW_MEMBERS
    vec4 model[3];
    vec4 tint;
} v;

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;

layout(location = 0) out vec3 world;
layout(location = 1) out vec3 facing;
layout(location = 2) out vec2 across;

void main()
{
    vec4 p = vec4(position, 1.0);

    world = vec3(dot(v.model[0], p), dot(v.model[1], p), dot(v.model[2], p));
    // The matrix's own 3 by 3 turns a normal as it turns the surface,
    // which holds while the matrix only turns, moves and scales evenly -
    // what an aircraft and its parts do. A matrix that stretched one way
    // more than another would want its inverse's transpose here.
    facing = vec3(dot(v.model[0].xyz, normal), dot(v.model[1].xyz, normal),
                  dot(v.model[2].xyz, normal));
    across = uv;
    gl_Position = v.clip * vec4(world, 1.0);
}
