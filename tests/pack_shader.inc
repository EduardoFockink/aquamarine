inline const std::string VERT_SRC_PACK = R"#(#version 300 es
void main() {
    // one triangle over the whole viewport: the scissor picks the rect
    vec2 pos = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
})#";

static std::string packFragSrc(bool external) {
    return std::string("#version 300 es\n") + (external ? "#extension GL_OES_EGL_image_external_essl3 : require\n" : "") + R"#(
precision highp float;
precision highp int;

uniform highp )#" + (external ? "samplerExternalOES" : "sampler2D") + R"#( tex;
uniform vec2 invSize;
uniform int swapRB;
out vec4 fragColor;

void main() {
    // the image's first row is GL's y = 0 both in tex and in the target: no flip
    vec3  c = texture(tex, gl_FragCoord.xy * invSize).rgb;
    uvec3 v = uvec3(round(clamp(c, 0.0, 1.0) * 1023.0));
    if (swapRB == 1)
        v = v.bgr;
    // the X*2101010 word, split into its 4 bytes (lowest first) so an RGBA8 readback stores it as is
    uint w = v.r | (v.g << 10u) | (v.b << 20u) | (3u << 30u);
    fragColor = vec4(uvec4(w, w >> 8u, w >> 16u, w >> 24u) & 255u) / 255.0;
})#";
}

