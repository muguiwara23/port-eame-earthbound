#include "eb/crt_filter.hpp"
#include "crt_shader_source.hpp"
#include "gl_es.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace eb {
// Port/eame: OpenGL ES calls are linked directly (gl_es.hpp); the fragment
// shader is the desktop one compiled as GLSL ES 1.00.
struct CrtFilter::Impl {
    GLuint program{}, copy{};
    int copy_width{}, copy_height{};
    ~Impl() { if (program) glDeleteProgram(program); if (copy) glDeleteTextures(1, &copy); }
    void initialize() { program = gles::program(crt_oled_fragment, "CRT"); }
};
CrtFilter::CrtFilter() : impl_(std::make_unique<Impl>()) { impl_->initialize(); }
CrtFilter::~CrtFilter() = default;
void CrtFilter::draw(unsigned native_texture, int source_width, int source_height, bool direct_scene) {
    auto& gl = *impl_;
    GLint viewport[4]{}; glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return;
    glBindTexture(GL_TEXTURE_2D, 0);
    if (direct_scene) {
        // GPU-to-GPU copy of the game viewport only; no CPU readback, and no
        // resampling of fractional scene coordinates to a native framebuffer.
        if (!gl.copy) glGenTextures(1, &gl.copy);
        glBindTexture(GL_TEXTURE_2D, gl.copy);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (gl.copy_width != viewport[2] || gl.copy_height != viewport[3]) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, viewport[2], viewport[3], 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gl.copy_width = viewport[2]; gl.copy_height = viewport[3];
        }
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1], viewport[2], viewport[3]);
    } else glBindTexture(GL_TEXTURE_2D, native_texture);
    glUseProgram(gl.program);
    glUniform1i(glGetUniformLocation(gl.program, "Image"), 0);
    glUniform1i(glGetUniformLocation(gl.program, "DirectScene"), direct_scene);
    glUniform2f(glGetUniformLocation(gl.program, "RasterSize"), source_width, source_height);
    glUniform2f(glGetUniformLocation(gl.program, "OutputSize"), viewport[2], viewport[3]);
    gles::quad(gles::uv_top_down);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    gles::check("OpenGL CRT filter failed");
}
} // namespace eb
