#version 450
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) out vec4 o;
void main() { o = texture(t, vec2(0.5)); }
