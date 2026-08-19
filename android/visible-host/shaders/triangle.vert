#version 450

layout(location = 0) out vec3 color;

layout(push_constant) uniform Rotation {
    float angle_radians;
    float aspect_ratio;
} rotation;

const vec2 positions[3] = vec2[](
    vec2(0.0, -0.72),
    vec2(0.72, 0.62),
    vec2(-0.72, 0.62)
);

const vec3 colors[3] = vec3[](
    vec3(1.0, 0.15, 0.10),
    vec3(0.10, 1.0, 0.20),
    vec3(0.10, 0.30, 1.0)
);

void main() {
    float cosine = cos(rotation.angle_radians);
    float sine = sin(rotation.angle_radians);
    vec2 pixel_space = vec2(
        positions[gl_VertexIndex].x * rotation.aspect_ratio,
        positions[gl_VertexIndex].y
    );
    vec2 rotated = mat2(cosine, sine, -sine, cosine) * pixel_space;
    gl_Position = vec4(
        rotated.x / rotation.aspect_ratio,
        rotated.y,
        0.0,
        1.0
    );
    color = colors[gl_VertexIndex];
}
