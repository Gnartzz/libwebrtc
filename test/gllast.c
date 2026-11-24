// GL-Last für neuaufsetzen_test (23.11.2026): ein eigener Faden mit
// EGL/GLES 3 auf dem Render-Knoten (GBM, ohne Fenster), der wie eine
// Selbstansicht jedes Bild eine Kameratextur (1280x720 RGBA) aus dem
// Arbeitsspeicher hochlädt und mit einem rechenlastigen Shader in eine große
// Fläche zeichnet. Läuft IM Prüfprozess (dlopen), damit GL und VA-API
// dieselbe Mesa-/Winsys-Instanz teilen wie Qt Quick und der Encoder in
// honeycord-nativ.
//
// Bauen (Wirt, nicht im WebRTC-Baum):
//   gcc -O2 -fPIC -shared -o libgllast.so gllast.c -lEGL -lGLESv2 -lgbm -lpthread
//   gcc -O2 -DGLLAST_PROGRAMM -o gllast gllast.c -lEGL -lGLESv2 -lgbm -lpthread
// Stufe 1..8: Shader-Schleifen 24*Stufe, Fläche 2560x1440.
// Stufe + 100 (z. B. 102): wie die Selbstansicht von honeycord-nativ im
// CPU-Weg (VideoKachel.cpp: createTextureFromImage je Bild, alte löschen) —
// JE BILD eine NEUE Kameratextur anlegen, mit Pixeln füllen, die alte
// löschen; Takt 60/s. Das legt laufend neuen GPU-Speicher an und füllt ihn
// per CPU.

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <gbm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_t g_faden;
static atomic_int g_halt = 0;
static atomic_int g_bilder = 0;
static atomic_int g_bereit = 0;  // 1 = läuft, -1 = Fehler
static int g_stufe = 3;
static int g_neu_je_bild = 0;

static GLuint Shader(GLenum art, const char* q) {
  GLuint s = glCreateShader(art);
  glShaderSource(s, 1, &q, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "gllast: Shader: %s\n", log);
  }
  return s;
}

static void* Lauf(void* arg) {
  (void)arg;
  int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  struct gbm_device* gbm = fd >= 0 ? gbm_create_device(fd) : NULL;
  EGLDisplay dpy = gbm ? eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL) : EGL_NO_DISPLAY;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
    fprintf(stderr, "gllast: EGL geht nicht\n");
    g_bereit = -1;
    return NULL;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint ka[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ka);
  if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
    fprintf(stderr, "gllast: Kontext geht nicht (0x%x)\n", eglGetError());
    g_bereit = -1;
    return NULL;
  }
  const int W = 2560, H = 1440, KW = 1280, KH = 720;
  GLuint ziel, kamera, fbo;
  glGenTextures(1, &ziel);
  glBindTexture(GL_TEXTURE_2D, ziel);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ziel, 0);
  glGenTextures(1, &kamera);
  glBindTexture(GL_TEXTURE_2D, kamera);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, KW, KH);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  char fq[1024];
  snprintf(fq, sizeof fq,
           "#version 300 es\nprecision highp float;\nin vec2 p;\nout vec4 f;\n"
           "uniform sampler2D k;\nuniform float t;\n"
           "void main(){ vec2 q=p; vec4 c=vec4(0.0);\n"
           " for(int i=0;i<%d;i++){ q=q*1.0003+vec2(sin(q.y*7.0+t),cos(q.x*5.0-t))*0.002;"
           " c+=texture(k,fract(q))*0.02; }\n f=c; }\n",
           24 * g_stufe);
  const char* vq =
      "#version 300 es\nout vec2 p;\nvoid main(){ vec2 v=vec2(gl_VertexID&1,gl_VertexID>>1);"
      " p=v; gl_Position=vec4(v*2.0-1.0,0.0,1.0); }\n";
  GLuint prog = glCreateProgram();
  glAttachShader(prog, Shader(GL_VERTEX_SHADER, vq));
  glAttachShader(prog, Shader(GL_FRAGMENT_SHADER, fq));
  glLinkProgram(prog);
  glUseProgram(prog);
  GLint ut = glGetUniformLocation(prog, "t");
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glViewport(0, 0, W, H);

  unsigned char* pix = malloc((size_t)KW * KH * 4);
  g_bereit = 1;
  for (int n = 0; !g_halt; ++n) {
    // Kamerabild im Arbeitsspeicher ändern und hochladen (wie Selbstansicht).
    // Bildartiger Inhalt (Paare gleicher Bytes wie RGBA-Kamerapixel).
    for (int y = 0; y < KH; ++y) {
      unsigned char* z = pix + (size_t)y * KW * 4;
      for (int x = 0; x < KW; ++x) {
        z[4 * x] = z[4 * x + 1] = (unsigned char)(x + n);
        z[4 * x + 2] = z[4 * x + 3] = (unsigned char)(y - n);
      }
    }
    if (g_neu_je_bild) {
      GLuint neu;
      glGenTextures(1, &neu);
      glBindTexture(GL_TEXTURE_2D, neu);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, KW, KH, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glDeleteTextures(1, &kamera);
      kamera = neu;
    } else {
      glBindTexture(GL_TEXTURE_2D, kamera);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, KW, KH, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    }
    glUniform1f(ut, n * 0.01f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if ((n & 3) == 3) glFinish(); else glFlush();
    g_bilder++;
    if (g_neu_je_bild) usleep(16000);   // ~60/s wie ein Bildschirmtakt
  }
  glFinish();
  free(pix);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglTerminate(dpy);
  gbm_device_destroy(gbm);
  close(fd);
  return NULL;
}

int gllast_start(int stufe) {
  g_neu_je_bild = stufe >= 100;
  if (stufe >= 100) stufe -= 100;
  g_stufe = stufe < 1 ? 1 : stufe > 8 ? 8 : stufe;
  g_halt = 0;
  if (pthread_create(&g_faden, NULL, Lauf, NULL) != 0) return -1;
  while (g_bereit == 0) usleep(1000);
  return g_bereit == 1 ? 0 : -1;
}

void gllast_stop(void) {
  g_halt = 1;
  pthread_join(g_faden, NULL);
}

int gllast_bilder(void) { return g_bilder; }

#ifdef GLLAST_PROGRAMM
int main(int argc, char** argv) {
  int stufe = argc > 1 ? atoi(argv[1]) : 3;
  int sek = argc > 2 ? atoi(argv[2]) : 30;
  if (gllast_start(stufe) != 0) return 1;
  sleep(sek);
  gllast_stop();
  printf("gllast: %d Bilder in %d s\n", gllast_bilder(), sek);
  return 0;
}
#endif
