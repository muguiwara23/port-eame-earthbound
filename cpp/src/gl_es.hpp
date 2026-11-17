#pragma once

// Port/eame: the presentation's OpenGL, as OpenGL ES 2.0 calls (which an
// OpenGL ES 3 context, iOS's, runs unchanged). The desktop renderer used the
// fixed-function pipeline (glBegin, ftransform, alpha test); here one small
// vertex shader passes positions through untransformed (the desktop code kept
// both matrices at identity), and the GLSL 1.20 fragment shaders are compiled
// as GLSL ES 1.00, which differs from them only in its header.

#include <SDL3/SDL_opengles2.h>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_OES
#endif

namespace eb::gles {
enum : GLuint { position_attribute = 0, texcoord_attribute = 1 };

inline constexpr char vertex_source[] =
    "#version 100\n"
    "attribute vec4 Position;\n"
    "attribute vec2 TexCoord;\n"
    "varying vec2 UV;\n"
    "void main() { gl_Position = Position; UV = TexCoord; }\n";

// The textured quad the fixed-function pipeline drew (GL_MODULATE by white),
// with its alpha test (GL_GREATER 0) when AlphaTest is set.
inline constexpr char plain_fragment[] =
    "#version 120\n"
    "uniform sampler2D Image;\n"
    "uniform bool AlphaTest;\n"
    "varying vec2 UV;\n"
    "void main() {\n"
    "    vec4 pixel = texture2D(Image, UV);\n"
    "    if (AlphaTest && pixel.a == 0.0) discard;\n"
    "    gl_FragColor = pixel;\n"
    "}\n";

// A desktop "#version 120" fragment shader as GLSL ES 1.00.
inline std::string es_fragment(std::string source) {
    const std::string desktop = "#version 120";
    if (source.compare(0, desktop.size(), desktop) == 0)
        source.replace(0, desktop.size(), "#version 100\nprecision highp float;");
    return source;
}

inline GLuint compile(GLenum type, const std::string& source, const char* what) {
    const GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok{};
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length{};
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(std::size_t(length) + 1);
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string(what) + " shader: " + log.data());
    }
    return shader;
}

// The shared vertex shader with a desktop fragment shader, its attributes at
// fixed locations so one client-array setup serves every program.
inline GLuint program(const std::string& fragment_source, const char* what) {
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, es_fragment(fragment_source), what);
    GLuint vertex{};
    try {
        vertex = compile(GL_VERTEX_SHADER, vertex_source, what);
    } catch (...) {
        glDeleteShader(fragment);
        throw;
    }
    const GLuint id = glCreateProgram();
    glAttachShader(id, vertex);
    glAttachShader(id, fragment);
    glBindAttribLocation(id, position_attribute, "Position");
    glBindAttribLocation(id, texcoord_attribute, "TexCoord");
    glLinkProgram(id);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok{};
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint length{};
        glGetProgramiv(id, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(std::size_t(length) + 1);
        glGetProgramInfoLog(id, length, nullptr, log.data());
        glDeleteProgram(id);
        throw std::runtime_error(std::string(what) + " program: " + log.data());
    }
    return id;
}

// The full-viewport triangle strip (-1,-1) (1,-1) (-1,1) (1,1) with the given
// texture coordinates, from client arrays.
inline void quad(const float (&uv)[8]) {
    static constexpr float positions[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(position_attribute);
    glEnableVertexAttribArray(texcoord_attribute);
    glVertexAttribPointer(position_attribute, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glVertexAttribPointer(texcoord_attribute, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(position_attribute);
    glDisableVertexAttribArray(texcoord_attribute);
}

// Source row zero at the top of the picture, opposite GL's screen origin.
inline constexpr float uv_top_down[8] = {0, 1, 1, 1, 0, 0, 1, 0};
// A GL-rendered (bottom-up) image.
inline constexpr float uv_bottom_up[8] = {0, 0, 1, 0, 0, 1, 1, 1};
} // namespace eb::gles
