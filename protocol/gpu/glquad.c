/* glquad.c — 用 EGL(surfaceless) + GLES2 畫一張圖，證明「命令怎麼送到 GPU」
 *
 * iSH 上沒有 GPU，走 Mesa llvmpipe（軟體實作）；到了 Artix 換成 nvidia 驅動，
 * 同一份程式碼、同一組 API，只是工作被送到真的顯示卡。
 *
 * 沒有 EGL/GLES 標頭也能編：直接宣告 ABI（EGL/GLES 的函式簽名是穩定的）。
 *   gcc -O2 -o /var/tmp/glquad glquad.c /usr/lib/libEGL.so.1 /usr/lib/libGLESv2.so.2
 *   LIBGL_DRIVERS_PATH=/usr/lib/xorg/modules/dri /var/tmp/glquad out.ppm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── EGL ABI ── */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int EGLint;

extern EGLDisplay eglGetDisplay(void *);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
extern EGLBoolean eglBindAPI(EGLenum);
extern EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
extern EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
extern EGLint eglGetError(void);
extern void *eglGetProcAddress(const char *);

typedef EGLDisplay (*PFN_getplatformdisplay)(unsigned int, void *, const intptr_t *);

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098

/* ── GLES2 ABI ── */
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef unsigned char GLboolean;
typedef float GLfloat;

extern void glViewport(GLint, GLint, GLsizei, GLsizei);
extern void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
extern void glClear(GLbitfield);
extern GLuint glCreateShader(GLenum);
extern void glShaderSource(GLuint, GLsizei, const char *const *, const GLint *);
extern void glCompileShader(GLuint);
extern void glGetShaderiv(GLuint, GLenum, GLint *);
extern void glGetShaderInfoLog(GLuint, GLsizei, GLsizei *, char *);
extern GLuint glCreateProgram(void);
extern void glAttachShader(GLuint, GLuint);
extern void glLinkProgram(GLuint);
extern void glGetProgramiv(GLuint, GLenum, GLint *);
extern void glGetProgramInfoLog(GLuint, GLsizei, GLsizei *, char *);
extern void glUseProgram(GLuint);
extern GLint glGetAttribLocation(GLuint, const char *);
extern void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
extern void glEnableVertexAttribArray(GLuint);
extern void glDrawArrays(GLenum, GLint, GLsizei);
extern void glGenFramebuffers(GLsizei, GLuint *);
extern void glBindFramebuffer(GLenum, GLuint);
extern void glGenRenderbuffers(GLsizei, GLuint *);
extern void glBindRenderbuffer(GLenum, GLuint);
extern void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
extern void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
extern GLenum glCheckFramebufferStatus(GLenum);
extern void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
extern void glFinish(void);
extern const unsigned char *glGetString(GLenum);

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TRIANGLES 0x0004
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FLOAT 0x1406
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01

static GLuint mk(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader 編譯失敗：%s\n%s\n", log, src);
        exit(1);
    }
    return s;
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "out.ppm";
    const int W = 320, H = 180;

    PFN_getplatformdisplay getpd =
        (PFN_getplatformdisplay)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getpd ? getpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                           : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (!dpy || !eglInitialize(dpy, &maj, &min)) {
        fprintf(stderr, "eglInitialize 失敗：0x%x\n", eglGetError());
        return 1;
    }
    printf("EGL %d.%d\n", maj, min);
    fprintf(stderr,"[1] init ok, bind api\n");
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_NONE};
    EGLConfig cfg = NULL;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "找不到 EGLConfig\n");
        return 1;
    }
    fprintf(stderr,"[2] chose config n=%d\n", n);
    EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, NULL, ctx_attr);
    fprintf(stderr,"[3] context=%p\n", (void*)ctx);
    if (!ctx || !eglMakeCurrent(dpy, NULL, NULL, ctx)) {
        fprintf(stderr, "context 建立失敗：0x%x\n", eglGetError());
        return 1;
    }
    fprintf(stderr,"[4] make current ok\n");
    printf("GL_VERSION  = %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER = %s\n", glGetString(GL_RENDERER));

    /* 離屏目標 */
    GLuint fbo, rbo;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO 不完整\n");
        return 1;
    }

    const char *vs =
        "attribute vec2 a_pos;\n"
        "varying vec2 v_uv;\n"
        "void main(){ v_uv = a_pos * 0.5 + 0.5; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    const char *fs =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "void main(){ gl_FragColor = vec4(v_uv, 0.55, 1.0); }\n";

    GLuint prog = glCreateProgram();
    glAttachShader(prog, mk(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, mk(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "link 失敗：%s\n", log);
        return 1;
    }
    glUseProgram(prog);

    const GLfloat verts[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
    GLint loc = glGetAttribLocation(prog, "a_pos");
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, 0, 0, verts);

    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glFinish();

    unsigned char *px = malloc((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--)               /* GL 原點在左下 */
        for (int x = 0; x < W; x++) {
            unsigned char *p = px + ((size_t)y * W + x) * 4;
            fwrite(p, 1, 3, f);
        }
    fclose(f);
    printf("已寫出 %s（%dx%d，%d 個頂點、1 次 draw call）\n", out, W, H, 6);
    return 0;
}
