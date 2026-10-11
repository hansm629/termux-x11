#version 450
// GLES's fragment programs in one: bit 0 of `flags` swaps red and blue (the .bgra program, for the X server's
// byte order in a buffer whose declared format is not BGRA), bit 1 writes alpha one (the root on screen is
// opaque, as GLES's surface without alpha is).
layout(push_constant) uniform Quad {
    vec4 rect;
    vec4 uv;
    uint flags;
} quad;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 color;

void main() {
    vec4 c = texture(tex, texCoord);
    if ((quad.flags & 1u) != 0u)
        c = c.bgra;
    if ((quad.flags & 2u) != 0u)
        c.a = 1.0;
    color = c;
}
