// Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
// The sky (`k3d_vk.c`): one triangle over the whole picture, at the far
// plane, carrying each pixel's place in clip space to the fragment shader.
#version 450

layout(location = 0) out vec2 ndc;

void main()
{
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));

    ndc = corner * 2.0 - 1.0;
    gl_Position = vec4(ndc, 1.0, 1.0);
}
