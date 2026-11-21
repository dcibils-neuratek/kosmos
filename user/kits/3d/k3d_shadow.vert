// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The sun's map (`k3d_vk.c`, `docs/flight.md` *Shadows*): a caster's
// corners, put in the world by its matrix - three affine rows, identity
// for a mesh - and then into the sun's box, whose rows are affine too.
#version 450

layout(push_constant) uniform Caster {
    vec4 sun[3];        // the world to the sun's map
    vec4 model[3];      // the world from the caster's own coordinates
} c;

layout(location = 0) in vec3 position;

void main()
{
    vec4 p = vec4(position, 1.0);
    vec4 w = vec4(dot(c.model[0], p), dot(c.model[1], p), dot(c.model[2], p), 1.0);

    gl_Position = vec4(dot(c.sun[0], w), dot(c.sun[1], w), dot(c.sun[2], w), 1.0);
}
