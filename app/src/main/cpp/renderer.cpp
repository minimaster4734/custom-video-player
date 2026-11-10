#include "renderer.h"
#include <android/log.h>
#include <cstring>
#include <initializer_list>

#define LOG_TAG "customplayer_renderer"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace {

const char *kVertexShader = R"glsl(
attribute vec4 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = aPosition;
    vTexCoord = aTexCoord;
}
)glsl";

/* YUV->RGB. Y/U/V are each sampled from separate GL_LUMINANCE textures
 * (single-channel, returned in .r regardless of which of r/g/b/a you read --
 * standard GLES2 luminance-texture behavior).
 *
 * Two per-frame switches, set from the frame's own metadata:
 *   uLimited  1.0 for "TV range" video (Y 16..235, UV 16..240 -- nearly all
 *             video), expanded to full range here; without this blacks come
 *             out grey and the picture looks washed out.
 *   uBT709    1.0 for the BT.709 matrix (HD), 0.0 for BT.601 (SD). */
const char *kFragmentShader = R"glsl(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uTexY;
uniform sampler2D uTexU;
uniform sampler2D uTexV;
uniform float uLimited;
uniform float uBT709;
void main() {
    float y = texture2D(uTexY, vTexCoord).r;
    float u = texture2D(uTexU, vTexCoord).r - 0.5;
    float v = texture2D(uTexV, vTexCoord).r - 0.5;
    y = mix(y, (y - 16.0 / 255.0) * (255.0 / 219.0), uLimited);
    u = mix(u, u * (255.0 / 224.0), uLimited);
    v = mix(v, v * (255.0 / 224.0), uLimited);
    float vr = mix(1.402,    1.5748,   uBT709);
    float ug = mix(0.344136, 0.187324, uBT709);
    float vg = mix(0.714136, 0.468124, uBT709);
    float ub = mix(1.772,    1.8556,   uBT709);
    gl_FragColor = vec4(y + vr * v, y - ug * u - vg * v, y + ub * u, 1.0);
}
)glsl";

const GLfloat kQuadVertices[] = {
    // x, y,      u, v
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
};

} // namespace

Renderer::Renderer() = default;

Renderer::~Renderer() {
    release();
}

GLuint Renderer::compileShader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool Renderer::compileProgram() {
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);

    GLint linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        LOGE("Program link failed: %s", log);
        return false;
    }

    loc_position_ = glGetAttribLocation(program_, "aPosition");
    loc_texcoord_ = glGetAttribLocation(program_, "aTexCoord");
    loc_tex_y_ = glGetUniformLocation(program_, "uTexY");
    loc_tex_u_ = glGetUniformLocation(program_, "uTexU");
    loc_tex_v_ = glGetUniformLocation(program_, "uTexV");
    loc_limited_ = glGetUniformLocation(program_, "uLimited");
    loc_bt709_ = glGetUniformLocation(program_, "uBT709");
    return true;
}

bool Renderer::init(ANativeWindow *window) {
    release();
    window_ = window;

    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }
    if (!eglInitialize(display_, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display_, config_attribs, &config, 1, &num_configs) || num_configs < 1) {
        LOGE("eglChooseConfig failed");
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, config, window_, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx_attribs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }

    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }

    if (!compileProgram()) {
        return false;
    }

    glGenTextures(1, &tex_y_);
    glGenTextures(1, &tex_u_);
    glGenTextures(1, &tex_v_);
    for (GLuint tex : {tex_y_, tex_u_, tex_v_}) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    initialized_ = true;
    LOGI("Renderer initialized");
    return true;
}

void Renderer::release() {
    if (tex_y_) { glDeleteTextures(1, &tex_y_); tex_y_ = 0; }
    if (tex_u_) { glDeleteTextures(1, &tex_u_); tex_u_ = 0; }
    if (tex_v_) { glDeleteTextures(1, &tex_v_); tex_v_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }

    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;

    if (window_) {
        ANativeWindow_release(window_);
        window_ = nullptr;
    }

    tex_width_ = 0;
    tex_height_ = 0;
    initialized_ = false;
}

/* GLES2 has no GL_UNPACK_ROW_LENGTH (that's GLES3+), so a plane whose
 * linesize doesn't exactly match its width -- very common, FFmpeg pads
 * rows for alignment -- can't be uploaded in one glTexImage2D call
 * without corrupting the image (each row would read into the next row's
 * padding). Falling back to one glTexSubImage2D call per row is the
 * standard, correct way to handle this on GLES2. */
void Renderer::uploadPlane(GLuint tex, int width, int height, const uint8_t *data, int linesize) {
    glBindTexture(GL_TEXTURE_2D, tex);
    if (linesize == width) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, data);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, width, height, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
        for (int row = 0; row < height; row++) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width, 1,
                             GL_LUMINANCE, GL_UNSIGNED_BYTE, data + (size_t)row * linesize);
        }
    }
}

void Renderer::ensureTextures(int width, int height) {
    tex_width_ = width;
    tex_height_ = height;
}

void Renderer::renderFrame(AVFrame *frame) {
    if (!initialized_ || !frame) return;

    if (frame->width != tex_width_ || frame->height != tex_height_) {
        ensureTextures(frame->width, frame->height);
    }

    int w = frame->width, h = frame->height;
    int cw = (w + 1) / 2, ch = (h + 1) / 2; // chroma plane dims for 4:2:0

    uploadPlane(tex_y_, w, h, frame->data[0], frame->linesize[0]);
    uploadPlane(tex_u_, cw, ch, frame->data[1], frame->linesize[1]);
    uploadPlane(tex_v_, cw, ch, frame->data[2], frame->linesize[2]);

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program_);

    glVertexAttribPointer((GLuint)loc_position_, 2, GL_FLOAT, GL_FALSE,
                           4 * sizeof(GLfloat), kQuadVertices);
    glEnableVertexAttribArray((GLuint)loc_position_);
    glVertexAttribPointer((GLuint)loc_texcoord_, 2, GL_FLOAT, GL_FALSE,
                           4 * sizeof(GLfloat), kQuadVertices + 2);
    glEnableVertexAttribArray((GLuint)loc_texcoord_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    glUniform1i(loc_tex_y_, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_u_);
    glUniform1i(loc_tex_u_, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_v_);
    glUniform1i(loc_tex_v_, 2);

    // Per-frame colour handling (see the shader comment). YUVJ420P is
    // full-range by definition; otherwise trust the frame's flag, which is
    // "unspecified" (= limited) for the vast majority of files. An
    // unspecified matrix is guessed from size, like most players do.
    bool full_range = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;
    bool bt709;
    switch (frame->colorspace) {
        case AVCOL_SPC_BT709: bt709 = true; break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_SMPTE240M: bt709 = false; break;
        default: bt709 = (h >= 720 || w >= 1280); break;
    }
    glUniform1f(loc_limited_, full_range ? 0.0f : 1.0f);
    glUniform1f(loc_bt709_, bt709 ? 1.0f : 0.0f);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    eglSwapBuffers(display_, surface_);
}
