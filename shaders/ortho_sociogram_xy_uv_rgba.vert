#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "uniforms.glsl"

layout(location=0) in vec2 inXY;
layout(location=1) in vec2 inUV;
layout(location=2) in vec4 inRGBA;

layout(location=0) out vec2 outUV;
layout(location=1) out vec4 outRGBA;

void main() {
    gl_Position = uniforms.proj * uniforms.ortho_sociogram * vec4(inXY, 0.f, 1.f);
    outUV = inUV;
    outRGBA = inRGBA;
}
