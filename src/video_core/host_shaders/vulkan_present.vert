// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460 core

layout (location = 0) out vec2 frag_tex_coord;

struct ScreenRectVertex {
    vec2 position;
    vec2 tex_coord;
};

layout (push_constant) uniform PushConstants {
    mat4 modelview_matrix;
    ScreenRectVertex vertices[4];
};

// Vulkan spec 15.8.1:
//   Any member of a push constant block that is declared as an
//   array must only be accessed with dynamically uniform indices.
// The quad is always an axis-aligned rect (see Layer::SetVertexData), so every
// corner can be derived from vertices[0] (top-left) and vertices[3]
// (bottom-right) with constant indices. The PowerVR proprietary compiler
// miscompiles the previous per-index branch chain and corrupts vertices[3],
// smearing the lower-right triangle.
void main() {
    const vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    const vec2 position = mix(vertices[0].position, vertices[3].position, corner);
    gl_Position = modelview_matrix * vec4(position, 0.0, 1.0);
    frag_tex_coord = mix(vertices[0].tex_coord, vertices[3].tex_coord, corner);
}
