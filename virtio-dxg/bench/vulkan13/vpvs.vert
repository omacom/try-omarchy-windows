#version 450
#extension GL_ARB_shader_viewport_layer_array : require
layout(location = 0) in vec3 pos;
layout(location = 0) flat out vec3 c;
layout(location = 1) flat out int oid;
void main() { gl_Position = vec4(pos, 1.0); gl_ViewportIndex = gl_InstanceIndex; c = gl_InstanceIndex == 0 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0); oid = 0; }
