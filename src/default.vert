#version 320 es

uniform mat4 zMVP;

uniform mat4 local_model;

layout(location = 0) in vec4 position;
layout(location = 1) in vec3 norm;

out vec3 normal;

void
main()
{
  gl_Position = zMVP * local_model * position;
  normal = norm;
}
