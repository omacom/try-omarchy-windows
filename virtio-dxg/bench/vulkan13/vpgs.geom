#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
layout(location = 0) flat out vec3 c;
layout(location = 1) flat out int oid;
void main() {
  for (int v = 0; v < 2; v++) {
    for (int i = 0; i < 3; i++) {
      gl_Position = gl_in[i].gl_Position; gl_ViewportIndex = v;
      c = v == 0 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0); oid = 0; EmitVertex();
    }
    EndPrimitive();
  }
}
