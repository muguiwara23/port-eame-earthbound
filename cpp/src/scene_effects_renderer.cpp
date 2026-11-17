#include "eb/scene_effects_renderer.hpp"
#include "scene_draw_batch.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace eb {
namespace {
const std::string window_source = R"GLSL(
uniform sampler2D Windows;
uniform vec2 ViewSize, ViewOrigin;
uniform float SourceWidth;
uniform bool Invert;
bool windowMask() {
    vec2 position = gl_FragCoord.xy - ViewOrigin;
    float x = clamp(floor(position.x * SourceWidth / ViewSize.x - (SourceWidth - 256.0) / 2.0), 0.0, 255.0);
    float y = clamp(floor((ViewSize.y - position.y) * 224.0 / ViewSize.y), 0.0, 223.0);
    vec4 row = floor(texture2D(Windows, vec2(0.5, (y + 0.5) / 224.0)) * 255.0 + 0.5);
    bool inside = (x >= row.r && x <= row.g) || (x >= row.b && x <= row.a);
    return Invert ? inside : !inside;
}
)GLSL";
const std::string layer_source = std::string("#version 120\n") + window_source + R"GLSL(
uniform sampler2D Atlas;
uniform bool Masked;
uniform float PixelAlpha;
varying vec2 UV;
void main() {
    vec4 pixel = texture2D(Atlas, UV);
    // Discard source transparency before depth/stencil. Output alpha is separate
    // semantic metadata: ineligible main pixels must still occlude lower layers.
    if (pixel.a == 0.0 || (Masked && windowMask())) discard;
    gl_FragColor = vec4(pixel.rgb, PixelAlpha);
}
)GLSL";
const std::string resolve_source = std::string("#version 120\n") + window_source + R"GLSL(
uniform sampler2D MainImage, SubImage;
uniform bool Masked, UseSubscreen, Subtract, Half;
uniform int Clip, Prevent;
uniform vec3 Fixed;
uniform float Brightness;
varying vec2 UV;
bool affected(int policy, bool inside) {
    return policy == 3 || (policy == 1 && !inside) || (policy == 2 && inside);
}
vec3 rgb5(vec3 rgb) { return floor(floor(rgb * 255.0 + 0.5) / 8.0); }
void main() {
    vec4 mainPixel = texture2D(MainImage, UV);
    vec4 subPixel = texture2D(SubImage, UV);
    bool inside = Masked && windowMask();
    bool clipped = affected(Clip, inside);
    vec3 color = clipped ? vec3(0.0) : rgb5(mainPixel.rgb);
    if (mainPixel.a > 0.5 && !affected(Prevent, inside)) {
        vec3 other = UseSubscreen ? rgb5(subPixel.rgb) : Fixed;
        color = Subtract ? max(vec3(0.0), color - other) : color + other;
        if (Half && !clipped && (!UseSubscreen || subPixel.a > 0.5)) color = floor(color / 2.0);
        color = min(vec3(31.0), color);
    }
    color = floor((color * Brightness + 7.0) / 15.0);
    gl_FragColor = vec4((color * 8.0 + floor(color / 4.0)) / 255.0, 1.0);
}
)GLSL";
void texture_parameters() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}
}
struct SceneEffectsRenderer::Impl {
    struct Program {
        GLuint id{};
        GLint windows{}, size{}, origin{}, source_width{}, invert{}, masked{}, alpha{};
    } layer, resolve;
    GLuint framebuffer{}, depth_stencil{}, images[2]{}, windows{};
    int width{}, height{};
    // Port/eame: the framebuffer to draw the result into. On iOS that is the
    // window's own framebuffer object, never 0 (EAGL has no default one).
    GLint screen{};
    std::vector<float> vertices;
    ~Impl() {
        if (layer.id) glDeleteProgram(layer.id);
        if (resolve.id) glDeleteProgram(resolve.id);
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (depth_stencil) glDeleteRenderbuffers(1, &depth_stencil);
        glDeleteTextures(2, images);
        if (windows) glDeleteTextures(1, &windows);
    }
    // Port/eame: OpenGL ES calls are linked directly (gl_es.hpp).
    Program program(const std::string& fragment) {
        Program p; p.id = gles::program(fragment, "Scene effect");
        p.windows = glGetUniformLocation(p.id, "Windows"); p.size = glGetUniformLocation(p.id, "ViewSize");
        p.origin = glGetUniformLocation(p.id, "ViewOrigin"); p.source_width = glGetUniformLocation(p.id, "SourceWidth");
        p.invert = glGetUniformLocation(p.id, "Invert"); p.masked = glGetUniformLocation(p.id, "Masked");
        p.alpha = glGetUniformLocation(p.id, "PixelAlpha");
        return p;
    }
    void initialize() {
        layer = program(layer_source); resolve = program(resolve_source);
        glGenFramebuffers(1, &framebuffer); glGenRenderbuffers(1, &depth_stencil);
        glGenTextures(2, images); glGenTextures(1, &windows);
        glBindTexture(GL_TEXTURE_2D, windows); texture_parameters();
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 224, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    void storage(int w, int h) {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        if (w != width || h != height) {
            for (auto image : images) {
                glBindTexture(GL_TEXTURE_2D, image); texture_parameters();
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
            glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_stencil);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_stencil);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            width = w; height = h;
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[0], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("Scene effect framebuffer is incomplete");
    }
    void common(const Program& p, const DirectSceneFrame& scene, int x, int y) {
        glUseProgram(p.id); glUniform1i(p.windows, 2); glUniform2f(p.size, width, height);
        glUniform2f(p.origin, x, y); glUniform1f(p.source_width, scene.width);
        glUniform1i(p.invert, scene.effects->invert);
    }
    void cleanup(const GLint* viewport) {
        glUseProgram(0); glBindFramebuffer(GL_FRAMEBUFFER, GLuint(screen));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        for (int unit : {2, 1}) {
            glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, 0);
        }
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_DEPTH_TEST);
    }
};

SceneEffectsRenderer::SceneEffectsRenderer() : impl_(std::make_unique<Impl>()) { impl_->initialize(); }
SceneEffectsRenderer::~SceneEffectsRenderer() = default;
void SceneEffectsRenderer::draw(const DirectScenePicture& picture, unsigned atlas_texture) {
    if (!picture.artwork || !picture.artwork->effects || !atlas_texture)
        throw std::invalid_argument("Scene effect draw requires an effect scene and atlas");
    const auto& scene = *picture.artwork; const auto& effect = *scene.effects;
    if (effect.brightness > 15 || std::any_of(effect.fixed.begin(), effect.fixed.end(), [](auto c) { return c > 31; }))
        throw std::invalid_argument("Invalid scene effect color range");
    for (const auto& quad : scene.quads)
        if (unsigned(quad.layer) > 5 || quad.u > scene.atlas_width || quad.width > scene.atlas_width - quad.u ||
            quad.v > scene.atlas_height || quad.height > scene.atlas_height - quad.v)
            throw std::invalid_argument("Scene effect primitive exceeds its atlas");
    auto& gl = *impl_; GLint viewport[4]; glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &gl.screen);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    try {
        glActiveTexture(GL_TEXTURE0); gl.storage(viewport[2], viewport[3]);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gl.windows);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 224, GL_RGBA, GL_UNSIGNED_BYTE, effect.windows.data());
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, atlas_texture);
        glViewport(0, 0, viewport[2], viewport[3]);
        glDisable(GL_BLEND); glDisable(GL_DITHER);
        glDepthMask(GL_TRUE); glStencilMask(255); glClearDepthf(1); glClearStencil(0);
        gl.common(gl.layer, scene, 0, 0); glUniform1i(glGetUniformLocation(gl.layer.id, "Atlas"), 0);
        for (unsigned pass = 0; pass < 2; ++pass) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl.images[pass], 0);
            const auto expand = [](unsigned c) { return float((c << 3) | (c >> 2)) / 255; };
            if (pass) glClearColor(expand(effect.fixed[0]), expand(effect.fixed[1]), expand(effect.fixed[2]), 0);
            else glClearColor(float((effect.backdrop >> 16) & 255) / 255, float((effect.backdrop >> 8) & 255) / 255,
                              float(effect.backdrop & 255) / 255, effect.math[5] ? 1 : 0);
            glDisable(GL_SCISSOR_TEST); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
            glStencilFunc(GL_EQUAL, 0, 255); glStencilOp(GL_KEEP, GL_INCR, GL_INCR);
            SceneDrawBatch batch(gl.vertices);
            bool state_valid = false, objects = false, masked = false, clipped = false;
            float alpha = 0;
            std::array<int, 4> scissor{};
            for (const auto& quad : scene.quads) {
                const auto layer = unsigned(quad.layer);
                if (layer == 5 || !(pass ? effect.sub[layer] : effect.main[layer])) continue;
                const bool next_objects = quad.object, next_masked = effect.masked[layer];
                const float next_alpha = pass ? (quad.priority >= 0 ? 1 : 0) :
                    (quad.color_math_eligible && effect.math[layer] ? 1 : 0);
                const auto offset = quad.motion < picture.offsets.size() ? picture.offsets[quad.motion] : DirectScenePicture::Offset{};
                const float l = std::max(0.f, quad.clip.left), r = std::min(float(scene.width), quad.clip.right),
                            t = std::max(0.f, quad.clip.top), b = std::min(224.f, quad.clip.bottom);
                if (l >= r || t >= b) continue;
                const bool next_clipped = l > 0 || r < scene.width || t > 0 || b < 224;
                std::array<int, 4> next_scissor{};
                if (next_clipped) {
                    const int x0 = int(std::ceil(l * gl.width / scene.width - .5f)), x1 = int(std::ceil(r * gl.width / scene.width - .5f)),
                              y0 = int(std::ceil(t * gl.height / 224 - .5f)), y1 = int(std::ceil(b * gl.height / 224 - .5f));
                    next_scissor = {x0, gl.height - y1, x1 - x0, y1 - y0};
                }
                if (!state_valid || objects != next_objects || masked != next_masked || alpha != next_alpha ||
                    clipped != next_clipped || (next_clipped && scissor != next_scissor)) {
                    batch.flush();
                    if (next_objects) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
                    glUniform1i(gl.layer.masked, next_masked);
                    glUniform1f(gl.layer.alpha, next_alpha);
                    if (next_clipped) {
                        glEnable(GL_SCISSOR_TEST);
                        glScissor(next_scissor[0], next_scissor[1], next_scissor[2], next_scissor[3]);
                    } else glDisable(GL_SCISSOR_TEST);
                    state_valid = true; objects = next_objects; masked = next_masked; alpha = next_alpha;
                    clipped = next_clipped; scissor = next_scissor;
                }
                batch.append(quad, offset, scene);
            }
            batch.flush();
        }
        glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, GLuint(gl.screen));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gl.images[1]);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gl.images[0]);
        gl.common(gl.resolve, scene, viewport[0], viewport[1]);
        const auto uniform = [&](const char* name, int value) { glUniform1i(glGetUniformLocation(gl.resolve.id, name), value); };
        uniform("MainImage", 0); uniform("SubImage", 1); uniform("Masked", effect.masked[5]);
        uniform("UseSubscreen", effect.use_subscreen); uniform("Subtract", effect.subtract); uniform("Half", effect.half);
        uniform("Clip", int(effect.clip)); uniform("Prevent", int(effect.prevent));
        glUniform3f(glGetUniformLocation(gl.resolve.id, "Fixed"), effect.fixed[0], effect.fixed[1], effect.fixed[2]);
        glUniform1f(glGetUniformLocation(gl.resolve.id, "Brightness"), effect.brightness);
        gles::quad(gles::uv_bottom_up);
        if (glGetError() != GL_NO_ERROR) throw std::runtime_error("OpenGL scene effect composition failed");
    } catch (...) { gl.cleanup(viewport); throw; }
    gl.cleanup(viewport);
}
}
