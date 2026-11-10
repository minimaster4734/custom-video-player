#ifndef CUSTOMPLAYER_RENDERER_H
#define CUSTOMPLAYER_RENDERER_H

#include <vector>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>

extern "C" {
#include <libavutil/frame.h>
}

/* EGL + OpenGL ES 2.0 YUV420P renderer. EGL contexts are thread-affine --
 * every method here must be called from the same thread (the render
 * thread owns this object and never shares it). */
class Renderer {
public:
    Renderer();
    ~Renderer();

    /* Takes ownership of a reference to window (caller must have already
     * ANativeWindow_acquire'd it); Renderer releases it in release(). */
    bool init(ANativeWindow *window);
    void release();

    /* Uploads and draws one YUV420P (or NV12 -- see note in .cpp) frame.
     * Safe to call repeatedly; (re)allocates GL textures only when the
     * frame size actually changes. */
    void renderFrame(AVFrame *frame);

    bool isInitialized() const { return initialized_; }

private:
    bool compileProgram();
    static GLuint compileShader(GLenum type, const char *src);
    void ensureTextures(int width, int height);
    void uploadPlane(GLuint tex, int width, int height, const uint8_t *data, int linesize);

    ANativeWindow *window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;

    GLuint program_ = 0;
    GLint loc_position_ = -1;
    GLint loc_texcoord_ = -1;
    GLint loc_tex_y_ = -1;
    GLint loc_tex_u_ = -1;
    GLint loc_tex_v_ = -1;
    GLint loc_limited_ = -1;
    GLint loc_bt709_ = -1;

    GLuint tex_y_ = 0, tex_u_ = 0, tex_v_ = 0;
    int tex_width_ = 0, tex_height_ = 0;
    std::vector<uint8_t> scratch_;   // tight copy of a padded plane, for a single upload

    bool initialized_ = false;
};

#endif /* CUSTOMPLAYER_RENDERER_H */
