#version 450
layout(location = 0) flat in vec3 c;
layout(location = 1) flat in int id;
layout(location = 0) out vec4 o;
void main() { o = vec4(float(id == 0 || id == 3), float(id == 1 || id == 3), float(id == 2 || id == 3), 1.0); }
