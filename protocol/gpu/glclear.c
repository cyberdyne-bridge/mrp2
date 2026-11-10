/* glclear.c — 最小驗證：EGL surfaceless + GLES2 清屏 + 讀回像素
 * 不編譯 shader，只證明「命令能送到 GL」。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef void *EGLDisplay; typedef void *EGLConfig; typedef void *EGLContext;
typedef void *EGLSurface; typedef unsigned int EGLBoolean; typedef unsigned int EGLenum;
typedef int EGLint;
extern EGLDisplay eglGetDisplay(void *);
extern EGLBoolean eglInitialize(EGLDisplay, EGLint *, EGLint *);
extern EGLBoolean eglBindAPI(EGLenum);
extern EGLBoolean eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
extern EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
extern EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
extern EGLint eglGetError(void);
extern void *eglGetProcAddress(const char *);
typedef EGLDisplay (*PFN)(unsigned int, void *, const intptr_t *);

extern void glViewport(int, int, int, int);
extern void glClearColor(float, float, float, float);
extern void glClear(unsigned int);
extern void glFinish(void);
extern void glReadPixels(int, int, int, int, unsigned int, unsigned int, void *);
extern const unsigned char *glGetString(unsigned int);
extern void glGenFramebuffers(int, unsigned int *);
extern void glBindFramebuffer(unsigned int, unsigned int);
extern void glGenRenderbuffers(int, unsigned int *);
extern void glBindRenderbuffer(unsigned int, unsigned int);
extern void glRenderbufferStorage(unsigned int, unsigned int, int, int);
extern void glFramebufferRenderbuffer(unsigned int, unsigned int, unsigned int, unsigned int);
extern unsigned int glCheckFramebufferStatus(unsigned int);

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "clear.ppm";
    const int W = 320, H = 180;
    PFN getpd = (PFN)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = getpd ? getpd(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL)
                           : eglGetDisplay(NULL);
    EGLint maj = 0, min = 0;
    if (!dpy || !eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "init fail 0x%x\n", eglGetError()); return 1; }
    fprintf(stderr, "[1] EGL %d.%d\n", maj, min);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg = NULL; EGLint n = 0;
    if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1) { fprintf(stderr, "no config\n"); return 1; }
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, NULL, xa);
    if (!ctx || !eglMakeCurrent(dpy, NULL, NULL, ctx)) { fprintf(stderr, "no context 0x%x\n", eglGetError()); return 1; }
    fprintf(stderr, "[2] context ok\n");
    const unsigned char *v = glGetString(GL_VERSION), *r = glGetString(GL_RENDERER);
    fprintf(stderr, "[3] version=%s renderer=%s\n", v ? (const char *)v : "?", r ? (const char *)r : "?");

    unsigned int fbo, rbo;
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rbo); glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo);
    fprintf(stderr, "[4] fbo status=0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));

    glViewport(0, 0, W, H);
    glClearColor(0.05f, 0.10f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    fprintf(stderr, "[5] cleared\n");

    unsigned char *px = malloc((size_t)W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(out, "wb");
    if (!f) { perror("fopen"); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; y--)
        for (int x = 0; x < W; x++) fwrite(px + ((size_t)y * W + x) * 4, 1, 3, f);
    fclose(f);
    printf("寫出 %s\n", out);
    return 0;
}
