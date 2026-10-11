#version 450
// One textured rectangle, as GLES's draw()/drawRegion() draw one: four vertices of a triangle strip spanning
// `rect` (x0, y0, x1, y1 in Vulkan's clip space, y pointing down) with texture coordinates spanning `uv`.
layout(push_constant) uniform Quad {
    vec4 rect;
    vec4 uv;
    uint flags;
} quad;
layout(location = 0) out vec2 texCoord;

void main() {
    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    gl_Position = vec4(mix(quad.rect.xy, quad.rect.zw, corner), 0.0, 1.0);
    texCoord = mix(quad.uv.xy, quad.uv.zw, corner);
}
