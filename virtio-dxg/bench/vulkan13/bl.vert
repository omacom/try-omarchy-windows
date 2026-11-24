#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 col;
layout(location = 2) in int id;
layout(location = 0) flat out vec3 c;
layout(location = 1) flat out int oid;
void main() { gl_Position = vec4(pos, 1.0); c = col; oid = id; }
