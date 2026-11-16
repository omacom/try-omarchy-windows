#version 450
layout(location = 0) flat in vec3 c;
layout(location = 1) flat in int id;
layout(location = 0) out vec4 o;
void main() { o = vec4(c, 1.0); }
