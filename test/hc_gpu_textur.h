// GPU-Bild → RGBA-Textur für Flutter (Empfang ohne Umweg, 10.11.2026).
//
// ★ GEMESSEN (Intel UHD 630, 1080p): Der alte Weg kostete je fremdem Bild
// ~5,9 ms CPU — Zurückkopieren aus der Grafikkarte, zwei Kopien und
// I420→RGBA im libwebrtc-Wrapper, dann `glTexImage2D` in Flutters
// Pixelpuffer-Textur. Hier bleibt das Bild auf der Grafikkarte: die beiden
// NV12-Ebenen kommen als DMA-BUF (`honeycord_gpu_bild` aus libwebrtc.so),
// werden als EGLImage gebunden und per Shader in eine RGBA-Textur gezeichnet.
//
// Farbformel BT.601 begrenzt — dieselbe wie libyuv `I420ToABGR` im alten
// Weg, damit das Bild nicht anders aussieht.
//
// Nur Header, keine Bindung an GL-Bibliotheken: alle Funktionen kommen über
// `eglGetProcAddress` (die Flatpak-Laufzeit und der Bau-Container haben
// verschiedene GL-Pakete). Diese Datei liegt identisch auch im
// libwebrtc-Fork (test/hc_gpu_textur.h) für das Prüfprogramm.
#pragma once

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hcgpu {

// Muss zu `libwebrtc::nullkopie::HcGpuBild` passen (Version 1).
struct Bild {
  uint32_t version = 1;
  int32_t breite = 0, hoehe = 0;
  struct {
    int32_t fd = -1;
    uint32_t drm_format = 0, offset = 0, pitch = 0;
    uint64_t modifier = 0;
  } ebene[2];
};

inline void FdsSchliessen(Bild& b) {
  for (auto& e : b.ebene) {
    if (e.fd >= 0) ::close(e.fd);
    e.fd = -1;
  }
}

using GetProc = void* (*)(const char*);
using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLchar = char;
using GLfloat = float;
using GLbitfield = unsigned int;
using GLsizeiptr = long;

constexpr GLenum kTexture2D = 0x0DE1, kTexture0 = 0x84C0, kMinFilter = 0x2801,
                 kMagFilter = 0x2800, kLinear = 0x2601, kClampToEdge = 0x812F,
                 kWrapS = 0x2802, kWrapT = 0x2803, kRGBA = 0x1908, kUByte = 0x1401,
                 kFramebuffer = 0x8D40, kColorAttachment0 = 0x8CE0,
                 kFramebufferComplete = 0x8CD5, kVertexShader = 0x8B31,
                 kFragmentShader = 0x8B30, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82,
                 kTriangleStrip = 0x0005, kFloat = 0x1406, kArrayBuffer = 0x8892,
                 kStaticDraw = 0x88E4, kUnpackAlignment = 0x0CF5,
                 kFramebufferBinding = 0x8CA6, kTextureBinding2D = 0x8069;

// EGL-Konstanten für den DMA-BUF-Import.
constexpr int kEglWidth = 0x3057, kEglHeight = 0x3056, kEglNone = 0x3038,
              kEglFourcc = 0x3271, kEglPlane0Fd = 0x3272, kEglPlane0Offset = 0x3273,
              kEglPlane0Pitch = 0x3274, kEglModLo = 0x3443, kEglModHi = 0x3444;
constexpr unsigned kEglLinuxDmaBuf = 0x3270;
constexpr uint64_t kModInvalid = 0x00ffffffffffffffULL;

enum class Dialekt { kES, kCore, kLegacy };

class Zeichner {
 public:
  // Lädt die Funktionen und baut das Programm — im Kontext, in dem später
  // gezeichnet wird. false = dieser Weg geht hier nicht.
  bool Einrichten(GetProc gp, void* egl_anzeige, Dialekt d) {
    anzeige_ = egl_anzeige;
    dialekt_ = d;
#define HC_LADE(n)                                              \
  n = reinterpret_cast<decltype(n)>(gp(#n));                    \
  if (!n) {                                                     \
    std::fprintf(stderr, "hc_gpu_textur: %s fehlt\n", #n);      \
    return false;                                               \
  }
    HC_LADE(glGenTextures) HC_LADE(glBindTexture) HC_LADE(glTexParameteri)
    HC_LADE(glTexImage2D) HC_LADE(glDeleteTextures) HC_LADE(glActiveTexture)
    HC_LADE(glGenFramebuffers) HC_LADE(glBindFramebuffer) HC_LADE(glFramebufferTexture2D)
    HC_LADE(glCheckFramebufferStatus) HC_LADE(glViewport) HC_LADE(glCreateShader)
    HC_LADE(glShaderSource) HC_LADE(glCompileShader) HC_LADE(glGetShaderiv)
    HC_LADE(glGetShaderInfoLog) HC_LADE(glCreateProgram) HC_LADE(glAttachShader)
    HC_LADE(glBindAttribLocation) HC_LADE(glLinkProgram) HC_LADE(glGetProgramiv)
    HC_LADE(glUseProgram) HC_LADE(glGetUniformLocation) HC_LADE(glUniform1i) HC_LADE(glUniform2f)
    HC_LADE(glGenBuffers) HC_LADE(glBindBuffer) HC_LADE(glBufferData)
    HC_LADE(glEnableVertexAttribArray) HC_LADE(glVertexAttribPointer) HC_LADE(glDrawArrays)
    HC_LADE(glPixelStorei) HC_LADE(glGetIntegerv)
    HC_LADE(glEGLImageTargetTexture2DOES)
    HC_LADE(eglCreateImageKHR) HC_LADE(eglDestroyImageKHR)
    if (d == Dialekt::kCore) {
      HC_LADE(glGenVertexArrays) HC_LADE(glBindVertexArray)
    }
#undef HC_LADE
    if (!ProgrammBauen()) return false;
    const GLfloat ecken[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    if (d == Dialekt::kCore) {
      glGenVertexArrays(1, &vao_);
      glBindVertexArray(vao_);
    }
    glGenBuffers(1, &vbo_);
    glBindBuffer(kArrayBuffer, vbo_);
    glBufferData(kArrayBuffer, sizeof ecken, ecken, kStaticDraw);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, kFloat, 0, 0, nullptr);
    glGenFramebuffers(1, &fbo_);
    glGenTextures(2, ebenen_tex_);
    for (GLuint t : ebenen_tex_) {
      glBindTexture(kTexture2D, t);
      Filter();
    }
    bereit_ = true;
    return true;
  }

  bool bereit() const { return bereit_; }

  // Zeichnet [b] in [ziel] (wird bei Bedarf angelegt oder neu bemessen) —
  // verkleinert auf höchstens [max_b]×[max_h] (Seitenverhältnis bleibt, nie
  // vergrößert; 0 = Originalgröße). Schließt die Deskriptoren in jedem Fall.
  bool Zeichnen(Bild& b, GLuint* ziel, int* zb, int* zh, int max_b = 0, int max_h = 0) {
    if (!bereit_ || b.breite < 2 || b.hoehe < 2) {
      FdsSchliessen(b);
      return false;
    }
    void* bilder[2] = {nullptr, nullptr};
    const int bw[2] = {b.breite, (b.breite + 1) / 2};
    const int bh[2] = {b.hoehe, (b.hoehe + 1) / 2};
    bool ok = true;
    for (int i = 0; i < 2 && ok; ++i) {
      int a[20];
      int n = 0;
      a[n++] = kEglWidth; a[n++] = bw[i];
      a[n++] = kEglHeight; a[n++] = bh[i];
      a[n++] = kEglFourcc; a[n++] = static_cast<int>(b.ebene[i].drm_format);
      a[n++] = kEglPlane0Fd; a[n++] = b.ebene[i].fd;
      a[n++] = kEglPlane0Offset; a[n++] = static_cast<int>(b.ebene[i].offset);
      a[n++] = kEglPlane0Pitch; a[n++] = static_cast<int>(b.ebene[i].pitch);
      if (b.ebene[i].modifier != kModInvalid) {
        a[n++] = kEglModLo; a[n++] = static_cast<int>(b.ebene[i].modifier & 0xffffffffu);
        a[n++] = kEglModHi; a[n++] = static_cast<int>(b.ebene[i].modifier >> 32);
      }
      a[n++] = kEglNone;
      bilder[i] = eglCreateImageKHR(anzeige_, nullptr, kEglLinuxDmaBuf, nullptr, a);
      ok = bilder[i] != nullptr;
    }
    // Die EGLImages halten den Puffer selbst — die Deskriptoren nicht mehr.
    FdsSchliessen(b);
    if (!ok) letzter_fehler_ = "EGLImage";
    // Zielgröße: so groß wie die Kachel, nie größer als das Bild.
    int w = b.breite, h = b.hoehe;
    if (max_b > 0 && max_h > 0 && (w > max_b || h > max_h)) {
      const double f = std::min(static_cast<double>(max_b) / w, static_cast<double>(max_h) / h);
      w = std::max(2, static_cast<int>(w * f + 0.5));
      h = std::max(2, static_cast<int>(h * f + 0.5));
    }
    if (ok) {
      for (int i = 0; i < 2; ++i) {
        glBindTexture(kTexture2D, ebenen_tex_[i]);
        glEGLImageTargetTexture2DOES(kTexture2D, bilder[i]);
      }
      ok = ZielBereit(ziel, zb, zh, w, h);
      if (!ok) letzter_fehler_ = "Zieltextur";
    }
    if (ok) {
      glBindFramebuffer(kFramebuffer, fbo_);
      glFramebufferTexture2D(kFramebuffer, kColorAttachment0, kTexture2D, *ziel, 0);
      ok = glCheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
      if (!ok) letzter_fehler_ = "Framebuffer";
    }
    if (ok) {
      glViewport(0, 0, w, h);
      glUseProgram(programm_);
      // Beim Verkleinern vier Abtastungen je Zielpixel (je ein Viertel
      // Zielpixel versetzt) — sonst flimmert Schrift wie beim Ausdünnen.
      // Ab Faktor 1,5; darunter eine Abtastung, sonst würde es weich.
      const bool klein = b.breite >= w * 3 / 2 || b.hoehe >= h * 3 / 2;
      glUniform2f(versatz_ort_, klein ? 0.25f / w : 0.f, klein ? 0.25f / h : 0.f);
      if (dialekt_ == Dialekt::kCore) glBindVertexArray(vao_);
      glBindBuffer(kArrayBuffer, vbo_);
      glEnableVertexAttribArray(0);
      glVertexAttribPointer(0, 2, kFloat, 0, 0, nullptr);
      for (int i = 0; i < 2; ++i) {
        glActiveTexture(kTexture0 + i);
        glBindTexture(kTexture2D, ebenen_tex_[i]);
      }
      glDrawArrays(kTriangleStrip, 0, 4);
      glActiveTexture(kTexture0);
      glBindFramebuffer(kFramebuffer, 0);
      // Kein glFlush: der Kontextwechsel danach spült ohnehin.
    }
    for (void* bi : bilder) {
      if (bi) eglDestroyImageKHR(anzeige_, bi);
    }
    return ok;
  }

  // Der Pixelweg (kein GPU-Bild): RGBA hochladen wie Flutters Pixelpuffer.
  bool Hochladen(const uint8_t* rgba, int w, int h, GLuint* ziel, int* zb, int* zh) {
    if (!bereit_ || !rgba || w < 1 || h < 1) return false;
    if (!*ziel) {
      glGenTextures(1, ziel);
      glBindTexture(kTexture2D, *ziel);
      Filter();
    }
    glBindTexture(kTexture2D, *ziel);
    glPixelStorei(kUnpackAlignment, 4);
    glTexImage2D(kTexture2D, 0, kRGBA, w, h, 0, kRGBA, kUByte, rgba);
    *zb = w;
    *zh = h;
    return true;
  }

  // Woran das letzte Zeichnen scheiterte (für eine einzige Protokollzeile).
  const char* letzter_fehler() const { return letzter_fehler_; }

  void TexturWeg(GLuint* t) {
    if (bereit_ && t && *t) glDeleteTextures(1, t);
    if (t) *t = 0;
  }

  // Für das Prüfprogramm.
  GLuint fbo() const { return fbo_; }
  void (*glBindFramebuffer)(GLenum, GLuint) = nullptr;
  void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;

 private:
  void Filter() {
    glTexParameteri(kTexture2D, kMinFilter, kLinear);
    glTexParameteri(kTexture2D, kMagFilter, kLinear);
    glTexParameteri(kTexture2D, kWrapS, kClampToEdge);
    glTexParameteri(kTexture2D, kWrapT, kClampToEdge);
  }

  bool ZielBereit(GLuint* ziel, int* zb, int* zh, int w, int h) {
    if (*ziel && *zb == w && *zh == h) return true;
    if (!*ziel) glGenTextures(1, ziel);
    glBindTexture(kTexture2D, *ziel);
    Filter();
    glTexImage2D(kTexture2D, 0, kRGBA, w, h, 0, kRGBA, kUByte, nullptr);
    *zb = w;
    *zh = h;
    return *ziel != 0;
  }

  GLuint Shader(GLenum art, const char* text) {
    GLuint s = glCreateShader(art);
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, kCompileStatus, &ok);
    if (!ok) {
      char log[512] = {};
      glGetShaderInfoLog(s, sizeof log, nullptr, log);
      std::fprintf(stderr, "hc_gpu_textur: Shader: %s\n", log);
      return 0;
    }
    return s;
  }

  bool ProgrammBauen() {
    // Je Dialekt dieselbe Rechnung. t läuft von unten nach oben wie die
    // Zeilen der Zieltextur — Zeile 0 der Quelle landet in Zeile 0 des Ziels,
    // genau wie beim Hochladen mit glTexImage2D.
    // Vier Abtastungen (bei versatz 0 alle am selben Ort = eine).
    static const char* kRechnung =
        "  vec2 a = vec2(versatz.x, versatz.y), b = vec2(versatz.x, -versatz.y);\n"
        "  float y = 0.25 * (HC_TEX(yt, t + a).r + HC_TEX(yt, t - a).r +\n"
        "                    HC_TEX(yt, t + b).r + HC_TEX(yt, t - b).r);\n"
        "  vec2 c = 0.25 * (HC_TEX(uvt, t + a).rg + HC_TEX(uvt, t - a).rg +\n"
        "                   HC_TEX(uvt, t + b).rg + HC_TEX(uvt, t - b).rg) - vec2(0.5);\n"
        "  float Y = 1.164383 * (y - 0.0627451);\n"
        "  HC_AUS = vec4(Y + 1.596027 * c.y, Y - 0.391762 * c.x - 0.812968 * c.y,\n"
        "                Y + 2.017232 * c.x, 1.0);\n";
    char vs[512], fs[2048];
    if (dialekt_ == Dialekt::kCore) {
      std::snprintf(vs, sizeof vs,
                    "#version 150\nin vec2 p;\nout vec2 t;\n"
                    "void main() { t = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n");
      std::snprintf(fs, sizeof fs,
                    "#version 150\n#define HC_TEX texture\n#define HC_AUS farbe\n"
                    "in vec2 t;\nout vec4 farbe;\nuniform sampler2D yt;\nuniform sampler2D uvt;\n"
                    "uniform vec2 versatz;\n"
                    "void main() {\n%s}\n", kRechnung);
    } else {
      // highp, wo es geht: mediump (16 Bit) trifft bei 1920 Pixeln Breite
      // die Texel nicht mehr genau.
      const char* kopf = dialekt_ == Dialekt::kES
                             ? "#version 100\n#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n"
                               "#else\nprecision mediump float;\n#endif\n"
                             : "#version 120\n";
      std::snprintf(vs, sizeof vs,
                    "%sattribute vec2 p;\nvarying vec2 t;\n"
                    "void main() { t = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n",
                    kopf);
      std::snprintf(fs, sizeof fs,
                    "%s#define HC_TEX texture2D\n#define HC_AUS gl_FragColor\n"
                    "varying vec2 t;\nuniform sampler2D yt;\nuniform sampler2D uvt;\n"
                    "uniform vec2 versatz;\n"
                    "void main() {\n%s}\n", kopf, kRechnung);
    }
    GLuint v = Shader(kVertexShader, vs), f = Shader(kFragmentShader, fs);
    if (!v || !f) return false;
    programm_ = glCreateProgram();
    glAttachShader(programm_, v);
    glAttachShader(programm_, f);
    glBindAttribLocation(programm_, 0, "p");
    glLinkProgram(programm_);
    GLint ok = 0;
    glGetProgramiv(programm_, kLinkStatus, &ok);
    if (!ok) return false;
    glUseProgram(programm_);
    glUniform1i(glGetUniformLocation(programm_, "yt"), 0);
    glUniform1i(glGetUniformLocation(programm_, "uvt"), 1);
    versatz_ort_ = glGetUniformLocation(programm_, "versatz");
    return versatz_ort_ >= 0;
  }

  void* anzeige_ = nullptr;
  Dialekt dialekt_ = Dialekt::kES;
  bool bereit_ = false;
  GLuint programm_ = 0, vbo_ = 0, vao_ = 0, fbo_ = 0;
  GLuint ebenen_tex_[2] = {0, 0};
  GLint versatz_ort_ = -1;
  const char* letzter_fehler_ = "";

  void (*glGenTextures)(GLsizei, GLuint*) = nullptr;
  void (*glBindTexture)(GLenum, GLuint) = nullptr;
  void (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
  void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
  void (*glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
  void (*glActiveTexture)(GLenum) = nullptr;
  void (*glGenFramebuffers)(GLsizei, GLuint*) = nullptr;
  GLenum (*glCheckFramebufferStatus)(GLenum) = nullptr;
  void (*glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
  GLuint (*glCreateShader)(GLenum) = nullptr;
  void (*glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
  void (*glCompileShader)(GLuint) = nullptr;
  void (*glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
  void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
  GLuint (*glCreateProgram)() = nullptr;
  void (*glAttachShader)(GLuint, GLuint) = nullptr;
  void (*glBindAttribLocation)(GLuint, GLuint, const GLchar*) = nullptr;
  void (*glLinkProgram)(GLuint) = nullptr;
  void (*glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
  void (*glUseProgram)(GLuint) = nullptr;
  GLint (*glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
  void (*glUniform1i)(GLint, GLint) = nullptr;
  void (*glUniform2f)(GLint, GLfloat, GLfloat) = nullptr;
  void (*glGenBuffers)(GLsizei, GLuint*) = nullptr;
  void (*glBindBuffer)(GLenum, GLuint) = nullptr;
  void (*glBufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
  void (*glEnableVertexAttribArray)(GLuint) = nullptr;
  void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
  void (*glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
  void (*glPixelStorei)(GLenum, GLint) = nullptr;
  void (*glGetIntegerv)(GLenum, GLint*) = nullptr;
  void (*glEGLImageTargetTexture2DOES)(GLenum, void*) = nullptr;
  void (*glGenVertexArrays)(GLsizei, GLuint*) = nullptr;
  void (*glBindVertexArray)(GLuint) = nullptr;
  void* (*eglCreateImageKHR)(void*, void*, unsigned, void*, const int*) = nullptr;
  unsigned (*eglDestroyImageKHR)(void*, void*) = nullptr;
};

}  // namespace hcgpu
