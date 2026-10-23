/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "src/internal/vcm_capturer.h"

#include <stdint.h>

#include <algorithm>
#include <memory>
#include <string>

#include "api/units/time_delta.h"
#include "modules/video_capture/video_capture_factory.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"

#ifdef _WIN32
// Kamera-Telemetrie (2026-07-03, Abendtest "Win-Kamera 30->24->15 fps"):
// loggt verhandeltes Format + tatsaechlich gelieferte fps 1x/min nach
// %LOCALAPPDATA%\HoneyCord\cam.log (rotiert). Beantwortet im Feld sofort:
// WAS wurde mit der Kamera verhandelt (Format! MJPEG vs RAW = USB-Bandbreite)
// und WAS liefert sie wirklich.
#include <windows.h>
#include <dshow.h>  // id34: IAMCameraControl, ICreateDevEnum, Moniker-Enum
#include <dvdmedia.h>  // VIDEOINFOHEADER2 (Ratenliste je Medientyp)
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
namespace {
void CamRotateIfNeeded(const char* path) {
  char old_path[MAX_PATH + 8];
  std::snprintf(old_path, sizeof(old_path), "%s.old", path);
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExA(old_path, GetFileExInfoStandard, &fad)) {
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_u{}, old_u{};
    now_u.LowPart = now_ft.dwLowDateTime;
    now_u.HighPart = now_ft.dwHighDateTime;
    old_u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    old_u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    if (now_u.QuadPart > old_u.QuadPart &&
        now_u.QuadPart - old_u.QuadPart > 14ULL * 24 * 3600 * 10000000ULL) {
      DeleteFileA(old_path);
    }
  }
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
    unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
        fad.nFileSizeLow;
    if (size > 1024ULL * 1024ULL)
      MoveFileExA(path, old_path, MOVEFILE_REPLACE_EXISTING);
  }
}
void CamLog(const char* fmt, ...) {
  char path[MAX_PATH];
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  std::strncat(path, "\\HoneyCord", MAX_PATH - n - 1);
  CreateDirectoryA(path, nullptr);
  std::strncat(path, "\\cam.log", MAX_PATH - std::strlen(path) - 1);
  static bool rotated = false;
  if (!rotated) {
    CamRotateIfNeeded(path);
    rotated = true;
  }
  if (FILE* f = std::fopen(path, "a")) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f, "[cam %02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
  }
}
const char* CamVideoTypeName(webrtc::VideoType t) {
  switch (t) {
    case webrtc::VideoType::kI420: return "I420";
    case webrtc::VideoType::kMJPEG: return "MJPEG";
    case webrtc::VideoType::kYUY2: return "YUY2";
    case webrtc::VideoType::kNV12: return "NV12";
    case webrtc::VideoType::kRGB24: return "RGB24";
    case webrtc::VideoType::kARGB: return "ARGB";
    case webrtc::VideoType::kUYVY: return "UYVY";
    default: return "andere";
  }
}

// id34: Toleranter Vergleich der DirectShow-DevicePaths (case-insensitiv, einer
// darf den anderen enthalten — webrtcs unique_name kann sich in Nuancen von der
// Moniker-„DevicePath"-Property unterscheiden).
bool ExpPathMatch(const char* a, const char* b) {
  if (!a || !b) return false;
  if (_stricmp(a, b) == 0) return true;
  size_t la = std::strlen(a), lb = std::strlen(b);
  const char* hay = la >= lb ? a : b;
  const char* need = la >= lb ? b : a;
  if (*need == '\0') return false;
  for (const char* p = hay; *p; ++p) {
    size_t i = 0;
    while (need[i] && p[i] &&
           std::tolower((unsigned char)need[i]) ==
               std::tolower((unsigned char)p[i]))
      ++i;
    if (need[i] == '\0') return true;
  }
  return false;
}

// id34 „Framerate <-> Helligkeit": stellt die Kamera-Belichtung ein.
//  hold=true : deckelt auf Manual <= 1/32 s (log2-Wert -5), damit die
//              Auto-Belichtung bei wenig Licht die Belichtungszeit nicht ueber
//              1/30 s ziehen und Frames droppen kann (15-fps-Fall).
//  hold=false: setzt auf Auto ZURUECK — WICHTIG, weil UVC-Cams eine Manual-
//              Belichtung in der Hardware behalten (bis Auto/USB-Reset); ohne
//              diesen Restore bliebe die Cam nach dem Ausschalten gedrosselt.
// Oeffnet das Geraet separat per Moniker (DevicePath == unique_name), holt
// IAMCameraControl. Best effort — jeder Fehler wird nur geloggt, nie geworfen.
void ApplyExposure(const char* device_unique_name, bool hold) {
  if (!device_unique_name || !device_unique_name[0]) {
    CamLog("[hc-exp] kein DevicePath -> uebersprungen");
    return;
  }
  bool com_inited = false;
  HRESULT hrco = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hrco)) com_inited = true;  // RPC_E_CHANGED_MODE = schon init

  ICreateDevEnum* dev_enum = nullptr;
  IEnumMoniker* mon_enum = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                       IID_ICreateDevEnum, reinterpret_cast<void**>(&dev_enum));
  if (SUCCEEDED(hr) && dev_enum) {
    hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                         &mon_enum, 0);
  }
  bool applied = false, found = false;
  if (hr == S_OK && mon_enum) {
    IMoniker* mon = nullptr;
    while (!applied && mon_enum->Next(1, &mon, nullptr) == S_OK) {
      IPropertyBag* bag = nullptr;
      if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                       reinterpret_cast<void**>(&bag))) &&
          bag) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(bag->Read(L"DevicePath", &v, nullptr)) &&
            v.vt == VT_BSTR && v.bstrVal) {
          char path[512] = {0};
          WideCharToMultiByte(CP_ACP, 0, v.bstrVal, -1, path, sizeof(path),
                              nullptr, nullptr);
          if (ExpPathMatch(path, device_unique_name)) {
            found = true;
            IBaseFilter* filter = nullptr;
            if (SUCCEEDED(mon->BindToObject(
                    nullptr, nullptr, IID_IBaseFilter,
                    reinterpret_cast<void**>(&filter))) &&
                filter) {
              IAMCameraControl* cam = nullptr;
              if (SUCCEEDED(filter->QueryInterface(
                      IID_IAMCameraControl,
                      reinterpret_cast<void**>(&cam))) &&
                  cam) {
                long mn = 0, mx = 0, step = 0, def = 0, caps = 0;
                if (SUCCEEDED(cam->GetRange(CameraControl_Exposure, &mn, &mx,
                                            &step, &def, &caps))) {
                  HRESULT hs;
                  if (hold) {
                    long target = -5;  // 2^-5 = 1/32 s ~ 31 fps Deckel
                    if (target < mn) target = mn;
                    if (target > mx) target = mx;
                    if (step > 0) {  // nach unten (=kuerzer) aufs Raster snappen
                      long off = target - mn;
                      target = mn + (off / step) * step;
                    }
                    hs = cam->Set(CameraControl_Exposure, target,
                                  CameraControl_Flags_Manual);
                    if (SUCCEEDED(hs)) {
                      applied = true;
                      CamLog(
                          "[hc-exp] Belichtung gedeckelt: Exposure=%ld "
                          "(Range %ld..%ld step %ld def %ld) Flag=Manual",
                          target, mn, mx, step, def);
                    } else {
                      CamLog(
                          "[hc-exp] Set(Exposure) FEHLGESCHLAGEN hr=0x%08lx "
                          "(Ziel %ld, Range %ld..%ld)",
                          (unsigned long)hs, target, mn, mx);
                    }
                  } else {
                    // Auf Auto zurueck (Standardwert def, Flag Auto).
                    hs = cam->Set(CameraControl_Exposure, def,
                                  CameraControl_Flags_Auto);
                    if (SUCCEEDED(hs)) {
                      applied = true;
                      CamLog(
                          "[hc-exp] Belichtung auf Auto zurueckgesetzt "
                          "(def=%ld, Range %ld..%ld)",
                          def, mn, mx);
                    } else {
                      CamLog(
                          "[hc-exp] Auto-Restore FEHLGESCHLAGEN hr=0x%08lx",
                          (unsigned long)hs);
                    }
                  }
                } else {
                  CamLog("[hc-exp] GetRange(Exposure) nicht unterstuetzt");
                }
                cam->Release();
              } else {
                CamLog("[hc-exp] IAMCameraControl nicht verfuegbar");
              }
              filter->Release();
            } else {
              CamLog("[hc-exp] BindToObject(IBaseFilter) fehlgeschlagen");
            }
          }
        }
        VariantClear(&v);
        bag->Release();
      }
      mon->Release();
      mon = nullptr;
    }
  }
  if (!found)
    CamLog("[hc-exp] Geraet nicht gefunden (DevicePath-Match) -> kein Cap");
  if (mon_enum) mon_enum->Release();
  if (dev_enum) dev_enum->Release();
  if (com_inited) CoUninitialize();
}

// --- Ratenliste je Medientyp (2026-10-23, jimtonics-Mikroruckler) ----------
// webrtcs DeviceInfoDS (device_info_ds.cc CreateCapabilityMap) fragt je
// DirectShow-Medientyp IAMVideoControl::GetFrameRateList ab, behaelt aber nur
// die HOECHSTE Rate als maxFPS. Ob die Kamera z. B. 60 neben 90 kann, sieht man
// darum nur, wenn man die Liste selbst liest. Das passiert hier NUR LESEND an
// einem eigenen Filter-Exemplar (wie ApplyExposure; webrtcs Kerncode bleibt
// unberuehrt). Jeder Fehler wird nur protokolliert.
enum class CamRaten {
  kNichtLesbar,        // Geraet/Pin/IAMStreamConfig nicht erreichbar
  kKeineRatenliste,    // passender Typ da, aber keine Ratenliste (keine
                       // Ratensteuerung — webrtc setzt dann auch kein
                       // AvgTimePerFrame, Senken waere wirkungslos)
  kKeinPassenderTyp,   // kein Medientyp passt zur gewaehlten Faehigkeit
  kWunschFehlt,        // Ratenliste da, Wunschrate nicht enthalten
  kWunschDa,           // Ratenliste da, Wunschrate enthalten
};

const char* CamRatenName(CamRaten r) {
  switch (r) {
    case CamRaten::kNichtLesbar: return "nicht lesbar";
    case CamRaten::kKeineRatenliste: return "keine Ratenliste";
    case CamRaten::kKeinPassenderTyp: return "kein passender Medientyp";
    case CamRaten::kWunschFehlt: return "Wunschrate nicht in der Liste";
    case CamRaten::kWunschDa: return "Wunschrate in der Liste";
  }
  return "?";
}

constexpr uint32_t HcFourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

// Video-Subtypes nach FOURCC-Schema (XXXXXXXX-0000-0010-8000-00AA00389B71)
// liefern Data1 = FOURCC; alles andere (z. B. RGB24) -> 0.
uint32_t HcFourccAus(const GUID& g) {
  static const unsigned char kRest[8] = {0x80, 0x00, 0x00, 0xAA,
                                         0x00, 0x38, 0x9B, 0x71};
  if (g.Data2 != 0x0000 || g.Data3 != 0x0010) return 0;
  if (std::memcmp(g.Data4, kRest, 8) != 0) return 0;
  return g.Data1;
}

// Gleiche Zuordnung wie device_info_ds.cc (nur die fuer uns relevanten Typen).
webrtc::VideoType HcTypAusFourcc(uint32_t f) {
  if (f == HcFourcc('M', 'J', 'P', 'G')) return webrtc::VideoType::kMJPEG;
  if (f == HcFourcc('Y', 'U', 'Y', '2')) return webrtc::VideoType::kYUY2;
  if (f == HcFourcc('N', 'V', '1', '2')) return webrtc::VideoType::kNV12;
  if (f == HcFourcc('I', '4', '2', '0')) return webrtc::VideoType::kI420;
  if (f == HcFourcc('I', 'Y', 'U', 'V')) return webrtc::VideoType::kIYUV;
  if (f == HcFourcc('U', 'Y', 'V', 'Y')) return webrtc::VideoType::kUYVY;
  if (f == HcFourcc('H', 'D', 'Y', 'C')) return webrtc::VideoType::kUYVY;
  return webrtc::VideoType::kUnknown;
}

void HcMedientypFrei(AM_MEDIA_TYPE* pmt) {
  if (!pmt) return;
  if (pmt->cbFormat != 0 && pmt->pbFormat) CoTaskMemFree(pmt->pbFormat);
  if (pmt->pUnk) pmt->pUnk->Release();
  CoTaskMemFree(pmt);
}

// Erster Ausgangs-Pin des Filters — dieselbe Wahl wie webrtcs
// GetOutputPin(filter, GUID_NULL) in help_functions_ds.cc.
IPin* HcErsterAusgangsPin(IBaseFilter* filter) {
  IEnumPins* pins = nullptr;
  if (FAILED(filter->EnumPins(&pins)) || !pins) return nullptr;
  IPin* gefunden = nullptr;
  IPin* pin = nullptr;
  while (!gefunden && pins->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION dir;
    if (SUCCEEDED(pin->QueryDirection(&dir)) && dir == PINDIR_OUTPUT) {
      gefunden = pin;  // Referenz uebernehmen
    } else {
      pin->Release();
    }
    pin = nullptr;
  }
  pins->Release();
  return gefunden;
}

// Liest alle Medientypen samt Ratenliste, schreibt sie ins cam.log und
// beurteilt, ob die gewaehlte Faehigkeit (want_*) die Wunschrate kann.
// „Passend" = gleiche Breite/Hoehe/Format UND gleiche Hoechstrate, wie
// webrtc sie berechnet (int(1e7 / kuerzeste Dauer)).
CamRaten CamRatenPruefen(const char* unique_name, int want_w, int want_h,
                         webrtc::VideoType want_typ, int want_max,
                         int wunsch_fps) {
  const int64_t t0 = static_cast<int64_t>(GetTickCount64());
  if (!unique_name || !unique_name[0]) {
    CamLog("Raten: kein DevicePath -> nicht lesbar");
    return CamRaten::kNichtLesbar;
  }
  bool com_inited = false;
  HRESULT hrco = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hrco)) com_inited = true;  // RPC_E_CHANGED_MODE = schon init

  IBaseFilter* filter = nullptr;
  ICreateDevEnum* dev_enum = nullptr;
  IEnumMoniker* mon_enum = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                       IID_ICreateDevEnum, reinterpret_cast<void**>(&dev_enum));
  if (SUCCEEDED(hr) && dev_enum) {
    hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                         &mon_enum, 0);
  }
  if (hr == S_OK && mon_enum) {
    IMoniker* mon = nullptr;
    while (!filter && mon_enum->Next(1, &mon, nullptr) == S_OK) {
      IPropertyBag* bag = nullptr;
      if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                       reinterpret_cast<void**>(&bag))) &&
          bag) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(bag->Read(L"DevicePath", &v, nullptr)) &&
            v.vt == VT_BSTR && v.bstrVal) {
          char path[512] = {0};
          WideCharToMultiByte(CP_ACP, 0, v.bstrVal, -1, path, sizeof(path),
                              nullptr, nullptr);
          if (ExpPathMatch(path, unique_name)) {
            IBaseFilter* f = nullptr;
            if (SUCCEEDED(mon->BindToObject(nullptr, nullptr, IID_IBaseFilter,
                                            reinterpret_cast<void**>(&f))) &&
                f) {
              filter = f;
            }
          }
        }
        VariantClear(&v);
        bag->Release();
      }
      mon->Release();
      mon = nullptr;
    }
  }
  if (mon_enum) mon_enum->Release();
  if (dev_enum) dev_enum->Release();

  CamRaten urteil = CamRaten::kNichtLesbar;
  IPin* pin = filter ? HcErsterAusgangsPin(filter) : nullptr;
  IAMStreamConfig* sc = nullptr;
  if (pin) {
    pin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&sc));
  }
  IAMVideoControl* vc = nullptr;
  if (filter) {
    filter->QueryInterface(IID_IAMVideoControl, reinterpret_cast<void**>(&vc));
  }
  int count = 0, size = 0;
  if (!filter) {
    CamLog("Raten: Geraet nicht geoeffnet (DevicePath-Match) -> nicht lesbar");
  } else if (!pin || !sc) {
    CamLog("Raten: kein Ausgangs-Pin/IAMStreamConfig -> nicht lesbar");
  } else if (FAILED(sc->GetNumberOfCapabilities(&count, &size)) ||
             count <= 0 || size <= 0 || size > 4096) {
    CamLog("Raten: GetNumberOfCapabilities fehlgeschlagen -> nicht lesbar");
  } else {
    // VIDEO_STREAM_CONFIG_CAPS ist 128 Byte; Puffer grosszuegig.
    alignas(8) BYTE caps_buf[4096];
    bool passend_mit_liste = false, passend_ohne_liste = false;
    bool wunsch_da = false;
    CamLog("Raten: %d DirectShow-Medientypen, IAMVideoControl=%s", count,
           vc ? "ja" : "NEIN");
    for (int i = 0; i < count; ++i) {
      AM_MEDIA_TYPE* pmt = nullptr;
      if (sc->GetStreamCaps(i, &pmt, caps_buf) != S_OK || !pmt) continue;
      if (pmt->majortype != MEDIATYPE_Video || !pmt->pbFormat ||
          (pmt->formattype != FORMAT_VideoInfo &&
           pmt->formattype != FORMAT_VideoInfo2)) {
        HcMedientypFrei(pmt);
        continue;
      }
      int w = 0, h = 0;
      LONGLONG avg = 0;
      const bool vi2 = pmt->formattype == FORMAT_VideoInfo2;
      if (vi2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        auto* vh = reinterpret_cast<VIDEOINFOHEADER2*>(pmt->pbFormat);
        w = vh->bmiHeader.biWidth;
        h = vh->bmiHeader.biHeight;
        avg = vh->AvgTimePerFrame;
      } else if (!vi2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        auto* vh = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
        w = vh->bmiHeader.biWidth;
        h = vh->bmiHeader.biHeight;
        avg = vh->AvgTimePerFrame;
      } else {
        HcMedientypFrei(pmt);
        continue;
      }
      const uint32_t fcc = HcFourccAus(pmt->subtype);
      char fcc_txt[5] = {'?', '?', '?', '?', 0};
      if (fcc) {
        for (int k = 0; k < 4; ++k) {
          char ch = static_cast<char>((fcc >> (8 * k)) & 0xFF);
          fcc_txt[k] = (ch >= 32 && ch < 127) ? ch : '?';
        }
      }
      const webrtc::VideoType typ = HcTypAusFourcc(fcc);

      char raten[192] = {0};
      size_t rp = 0;
      int list_max = 0;
      bool hat_liste = false, enthaelt_wunsch = false;
      if (vc) {
        LONGLONG* dauern = nullptr;
        long n = 0;
        SIZE sz;
        sz.cx = w;
        sz.cy = h;
        if (vc->GetFrameRateList(pin, i, sz, &n, &dauern) == S_OK &&
            dauern && n > 0) {
          LONGLONG kuerzeste = 0;
          for (long k = 0; k < n; ++k) {
            const LONGLONG d = dauern[k];
            if (d <= 0) continue;
            if (kuerzeste == 0 || d < kuerzeste) kuerzeste = d;
            const double fps = 10000000.0 / static_cast<double>(d);
            if (wunsch_fps > 0 && fps > wunsch_fps - 1.0 &&
                fps < wunsch_fps + 1.0)
              enthaelt_wunsch = true;
            if (rp < sizeof(raten) - 12) {
              int m = std::snprintf(raten + rp, sizeof(raten) - rp, "%s%.4g",
                                    rp ? " " : "", fps);
              if (m > 0) rp += static_cast<size_t>(m);
            }
          }
          if (kuerzeste > 0) {
            hat_liste = true;
            list_max = static_cast<int>(10000000 / kuerzeste);
          }
        }
        if (dauern) CoTaskMemFree(dauern);
      }
      const int avg_fps = avg > 0 ? static_cast<int>(10000000 / avg) : 0;
      CamLog("  ds#%d %dx%d %s %s avg=%d raten: %s", i, w, h, fcc_txt,
             vi2 ? "VI2" : "VI", avg_fps, hat_liste ? raten : "(keine Liste)");
      const int vergleich_max = hat_liste ? list_max : avg_fps;
      if (w == want_w && h == want_h && typ == want_typ &&
          vergleich_max == want_max) {
        if (hat_liste) {
          passend_mit_liste = true;
          if (enthaelt_wunsch) wunsch_da = true;
        } else {
          passend_ohne_liste = true;
        }
      }
      HcMedientypFrei(pmt);
    }
    if (wunsch_da)
      urteil = CamRaten::kWunschDa;
    else if (passend_mit_liste)
      urteil = CamRaten::kWunschFehlt;
    else if (passend_ohne_liste)
      urteil = CamRaten::kKeineRatenliste;
    else
      urteil = CamRaten::kKeinPassenderTyp;
  }
  if (vc) vc->Release();
  if (sc) sc->Release();
  if (pin) pin->Release();
  if (filter) filter->Release();
  if (com_inited) CoUninitialize();
  CamLog("Raten: Urteil fuer %dx%d %s @%d (Wunsch %d): %s (%lldms)", want_w,
         want_h, CamVideoTypeName(want_typ), want_max, wunsch_fps,
         CamRatenName(urteil),
         (long long)(static_cast<int64_t>(GetTickCount64()) - t0));
  return urteil;
}

int64_t HcQpc() {
  LARGE_INTEGER li;
  QueryPerformanceCounter(&li);
  return static_cast<int64_t>(li.QuadPart);
}
}  // namespace
#endif  // _WIN32

namespace webrtc {
namespace internal {

VcmCapturer::VcmCapturer(webrtc::Thread* worker_thread)
    : vcm_(nullptr), worker_thread_(worker_thread) {}

bool VcmCapturer::Init(size_t width, size_t height, size_t target_fps,
                       size_t capture_device_index) {
  std::unique_ptr<VideoCaptureModule::DeviceInfo> device_info(
      VideoCaptureFactory::CreateDeviceInfo());

  char device_name[256];
  char unique_name[256];
  if (device_info->GetDeviceName(static_cast<uint32_t>(capture_device_index),
                                 device_name, sizeof(device_name), unique_name,
                                 sizeof(unique_name)) != 0) {
    Destroy();
    return false;
  }

  // id34: DevicePath merken, um die Kamera in StartCapture separat per Moniker
  // zu oeffnen und (opt-in) die Belichtung zu deckeln.
  unique_name_ = unique_name;

  vcm_ = webrtc::VideoCaptureFactory::Create(unique_name);

  if (!vcm_) {
    return false;
  }

  vcm_->RegisterCaptureDataCallback(this);

  const char* uid = vcm_->CurrentDeviceName();
  const int32_t wantW = static_cast<int32_t>(width);
  const int32_t wantH = static_cast<int32_t>(height);
  const int32_t wantF = static_cast<int32_t>(target_fps);

  // Format-Wahl (2026-07-04): FRUEHER wurde stur Capability[0] genommen und
  // videoType hart auf kI420 gezwungen. Fuer USB-Webcams ist das die
  // Bandbreiten-Falle: 720p60 als RAW (I420/YUY2) = ~660 Mbps -> Kamera oeffnet
  // langsam (#5) und faellt unter Last auf 24/15 fps (Abendtest). Native
  // Webcams liefern hohe Aufloesung/fps als MJPEG (~10x kleiner, von libwebrtc
  // intern billig nach I420 dekodiert) — genau das nutzt auch die Windows-
  // Kamera-App (die deshalb fluessig lief). Also: alle Geraete-Capabilities
  // durchsuchen und die beste fuer (Wunsch-Aufloesung, -fps, Format) waehlen.
  const bool prefer_mjpeg = (wantW * wantH) >= (1280 * 720);
  // Timing (#5 „Kamera oeffnet langsam"): NumberOfCapabilities baut auf Windows
  // die DirectShow-Capability-Map (verbindet sich mit dem Geraet, enumeriert
  // Medientypen) — klassischer Zeitfresser. Getrennt messen, um Enumeration vs
  // Geraete-Open vs erster Frame im Feld zu unterscheiden.
#ifdef _WIN32
  const int64_t t_caps0 = static_cast<int64_t>(GetTickCount64());
#endif
  const int32_t ncaps = device_info->NumberOfCapabilities(uid);
#ifdef _WIN32
  cap_map_ms_ = static_cast<int64_t>(GetTickCount64()) - t_caps0;
#endif
  VideoCaptureCapability best;
  bool have_best = false;
  int64_t best_score = 0;
  [[maybe_unused]] int32_t best_index = -1;  // nur Windows-Log
#ifdef _WIN32
  // Faehigkeitsliste einmal je Kamera-Start ins cam.log (2026-10-23): ohne sie
  // war im Feld nicht zu sehen, WARUM z. B. „@90" gewaehlt wurde.
  CamLog("Kamera \"%s\": %d Faehigkeiten (webrtc-Sicht, maxFPS = hoechste Rate "
         "je Medientyp)", device_name, ncaps);
#endif
  for (int32_t i = 0; i < ncaps; ++i) {
    VideoCaptureCapability c;
    if (device_info->GetCapability(uid, i, c) != 0) continue;
#ifdef _WIN32
    CamLog("  cap#%d %dx%d %s maxFPS=%d%s", i, c.width, c.height,
           CamVideoTypeName(c.videoType), c.maxFPS,
           c.interlaced ? " interlaced" : "");
#endif
    if (c.width <= 0 || c.height <= 0) continue;
    const int32_t dw = c.width > wantW ? c.width - wantW : wantW - c.width;
    const int32_t dh = c.height > wantH ? c.height - wantH : wantH - c.height;
    // Aufloesung dominiert; darunter fps (unter Ziel STARK bestrafen, ueber Ziel
    // nur leicht); Format entscheidet nur noch Gleichstaende.
    int64_t score = (static_cast<int64_t>(dw) + dh) * 100000;
    if (c.maxFPS >= wantF)
      score += (c.maxFPS - wantF);
    else
      score += static_cast<int64_t>(wantF - c.maxFPS) * 1000;
    int type_pref;
    if (prefer_mjpeg) {
      type_pref = (c.videoType == VideoType::kMJPEG)  ? 0
                  : (c.videoType == VideoType::kNV12)  ? 20
                  : (c.videoType == VideoType::kYUY2 ||
                     c.videoType == VideoType::kUYVY)  ? 30
                  : (c.videoType == VideoType::kI420)  ? 40
                                                       : 60;
    } else {
      // Niedrige Aufloesung: RAW bevorzugen (kein MJPEG-Decode noetig).
      type_pref = (c.videoType == VideoType::kNV12)   ? 0
                  : (c.videoType == VideoType::kI420)  ? 10
                  : (c.videoType == VideoType::kYUY2 ||
                     c.videoType == VideoType::kUYVY)  ? 20
                  : (c.videoType == VideoType::kMJPEG) ? 40
                                                       : 60;
    }
    score += type_pref;
    if (!have_best || score < best_score) {
      best = c;
      best_score = score;
      have_best = true;
      best_index = i;
    }
  }

  if (have_best) {
    // Echte Geraete-Capability uebernehmen (inkl. korrektem videoType) — NICHT
    // mehr Aufloesung/fps ueberschreiben, sonst verhandelt DirectShow neu.
    capability_ = best;
#ifdef _WIN32
    CamLog("Init: Wunsch %dx%d@%d prefMJPEG=%d -> gewaehlt %dx%d@%d Format=%s (aus %d Caps, CapMap=%lldms)",
           wantW, wantH, wantF, prefer_mjpeg ? 1 : 0, capability_.width,
           capability_.height, capability_.maxFPS,
           CamVideoTypeName(capability_.videoType), ncaps, (long long)cap_map_ms_);

    // Bildrate senken (2026-10-23, jimtonics: Wunsch 720p60 -> „720p@90"
    // MJPEG gewaehlt -> Kamera lief auf 90, FramerateController warf jedes
    // dritte Bild weg -> 11/22-ms-Abstaende = Mikroruckler).
    //
    // Warum der Kommentar oben („NICHT ... fps ueberschreiben") trotzdem gilt:
    // Damals wurden Aufloesung UND fps auf den WUNSCH gesetzt und videoType auf
    // I420 gezwungen; webrtcs GetBestMatchedCapability (device_info_impl.cc)
    // waehlte daraufhin in VideoCaptureDS::SetCameraOutput einen ANDEREN
    // Medientyp (z. B. RAW statt MJPEG = USB-Bandbreiten-Falle, langsames
    // Oeffnen #5, 24/15 fps). Genau das schliessen wir hier aus:
    //  1. Nur maxFPS wird gesenkt, Aufloesung/Format bleiben die der Kamera.
    //  2. Nur wenn die Kamera die Wunschrate laut eigener Ratenliste kann
    //     (sonst setzt webrtc ohnehin kein AvgTimePerFrame, siehe
    //     supportFrameRateControl) — ist die Liste gar nicht lesbar: Versuch.
    //  3. Nur wenn GetBestMatchedCapability mit der gesenkten Rate DENSELBEN
    //     Faehigkeits-Index liefert wie mit der unveraenderten (derselbe
    //     Abgleich laeuft spaeter in SetCameraOutput; gleiche Enumeration).
    //     Liefert er einen anderen (z. B. YUY2 720p@60, das bei
    //     „Bild/Rate erfuellt" den Zuschlag bekaeme), bleibt alles wie bisher.
    //  4. Rueckfall: StartCapture scheitert oder die Kamera liefert in den
    //     ersten Sekunden < 80 % der Wunschrate -> einmal mit der alten
    //     Faehigkeit neu starten (Kameras, die bei erzwungener Rate schwarz
    //     bleiben oder nur 5 fps liefern).
    rueckfall_cap_ = capability_;
    rate_gesenkt_ = false;
    wunsch_fps_ = wantF;
    if (wantF > 0 && best.maxFPS > wantF) {
      const CamRaten raten =
          CamRatenPruefen(unique_name_.c_str(), best.width, best.height,
                          best.videoType, best.maxFPS, wantF);
      const bool versuchen =
          raten == CamRaten::kWunschDa || raten == CamRaten::kNichtLesbar;
      if (!versuchen) {
        CamLog("Rate unveraendert %d (Wunsch %d): %s", best.maxFPS, wantF,
               CamRatenName(raten));
      } else {
        VideoCaptureCapability probe = best;
        probe.maxFPS = wantF;
        VideoCaptureCapability res_alt, res_neu;
        const int32_t idx_alt =
            device_info->GetBestMatchedCapability(uid, best, res_alt);
        const int32_t idx_neu =
            device_info->GetBestMatchedCapability(uid, probe, res_neu);
        if (idx_alt >= 0 && idx_neu == idx_alt) {
          capability_.maxFPS = wantF;
          rate_gesenkt_ = true;
          CamLog("Rate auf %d gesetzt (Kamera bot %d)%s [Abgleich idx %d, gewaehlt cap#%d]",
                 wantF, best.maxFPS,
                 raten == CamRaten::kNichtLesbar
                     ? " — Ratenliste nicht lesbar, Versuch mit Rueckfall"
                     : "",
                 idx_neu, best_index);
        } else {
          CamLog("Rate unveraendert %d: Abgleich mit %d waehlt anderes Format "
                 "(idx %d = %dx%d@%d %s statt idx %d = %dx%d@%d %s)",
                 best.maxFPS, wantF, idx_neu, res_neu.width, res_neu.height,
                 res_neu.maxFPS, CamVideoTypeName(res_neu.videoType), idx_alt,
                 res_alt.width, res_alt.height, res_alt.maxFPS,
                 CamVideoTypeName(res_alt.videoType));
        }
      }
    }
#endif
  } else {
    // Fallback: alte Heuristik (Index 0 + Wunschwerte, I420-Konvertierung).
    device_info->GetCapability(uid, 0, capability_);
    capability_.width = wantW;
    capability_.height = wantH;
    capability_.maxFPS = wantF;
    capability_.videoType = VideoType::kI420;
#ifdef _WIN32
    CamLog("Init: keine Caps gefunden (%d) -> Fallback %dx%d@%d I420", ncaps,
           wantW, wantH, wantF);
#endif
  }

  return true;
}

std::shared_ptr<VcmCapturer> VcmCapturer::Create(webrtc::Thread* worker_thread,
                                                 size_t width, size_t height,
                                                 size_t target_fps,
                                                 size_t capture_device_index) {
  std::shared_ptr<VcmCapturer> vcm_capturer(
      std::make_shared<VcmCapturer>(worker_thread));
  if (!vcm_capturer->Init(width, height, target_fps, capture_device_index)) {
    RTC_LOG(LS_WARNING) << "Failed to create VcmCapturer(w = " << width
                        << ", h = " << height << ", fps = " << target_fps
                        << ")";
    return nullptr;
  }
  return vcm_capturer;
}

bool VcmCapturer::StartCapture() {
#ifdef _WIN32
  const int64_t t_start0 = static_cast<int64_t>(GetTickCount64());
#endif
  int32_t result = worker_thread_->BlockingCall(
      [&] { return vcm_->StartCapture(capability_); });

#ifdef _WIN32
  // Rueckfall Teil 1: mit gesenkter Rate gescheitert -> einmal mit der
  // unveraenderten Faehigkeit (Rate wie von der Kamera gemeldet).
  if (result != 0 && rate_gesenkt_) {
    CamLog("Rueckfall auf %d: StartCapture mit %d FEHLGESCHLAGEN",
           rueckfall_cap_.maxFPS, capability_.maxFPS);
    rate_gesenkt_ = false;
    capability_ = rueckfall_cap_;
    result = worker_thread_->BlockingCall(
        [&] { return vcm_->StartCapture(capability_); });
  }
#endif

  if (result != 0) {
#ifdef _WIN32
    CamLog("StartCapture FEHLGESCHLAGEN (angefordert %dx%d@%d)",
           capability_.width, capability_.height, capability_.maxFPS);
#endif
    Destroy();
    return false;
  }

#ifdef _WIN32
  // capability_ ist jetzt die ECHTE, in Init() gewaehlte Geraete-Capability
  // (inkl. wahrem videoType) — kein CaptureSettings-Echo mehr noetig.
  // StartCapture-Dauer = DirectShow-Graph bauen + Geraet hochfahren (der zweite
  // grosse Zeitblock beim Kamera-Oeffnen). Erster Frame folgt separat (OnFrame).
  const int64_t start_ms = static_cast<int64_t>(GetTickCount64()) - t_start0;
  CamLog("Start OK: %dx%d@%d Format=%s (StartCapture=%lldms, CapMap war %lldms)",
         capability_.width, capability_.height, capability_.maxFPS,
         CamVideoTypeName(capability_.videoType), (long long)start_ms,
         (long long)cap_map_ms_);
  // Rueckfall Teil 2: gesenkte Rate lief an -> nach ~2+2 s nachmessen.
  // Frischer Lebt-Merker je Start; StopCapture setzt ihn auf dem
  // worker_thread_ auf false (dort laufen auch die verzoegerten Aufgaben).
  worker_thread_->BlockingCall([&] {
    if (pruef_lebt_) pruef_lebt_->store(false);  // Reste frueherer Starts
    pruef_lebt_ = std::make_shared<std::atomic<bool>>(true);
    if (rate_gesenkt_) PlaneRatenPruefung(pruef_lebt_, 0, 0, 0);
  });
  // id34 „Framerate <-> Helligkeit" (opt-in): flutter_webrtc setzt die
  // Prozess-Umgebungsvariable via Win32-SetEnvironmentVariableA — im selben
  // Prozess hier per GetEnvironmentVariableA sichtbar (CRT-/DLL-unabhaengig).
  //  „1" -> Belichtung Manual deckeln (haelt fps bei wenig Licht).
  //  „0" -> auf Auto ZURUECK. Noetig, weil eine Manual-Belichtung in der Cam-
  //         Hardware klebt: ohne Restore bliebe die Cam nach dem Ausschalten
  //         (oder nach einem Crash mit gesetztem Manual) dauerhaft gedrosselt.
  // Kosten sparen: den (teuren) Auto-Restore nur laufen, wenn wir in diesem
  // Prozess Manual gesetzt haben ODER noch nie Auto erzwungen wurde (deckt eine
  // aus einer frueheren/gecrashten Session haengende Manual-Belichtung beim
  // ERSTEN Kamera-Start ab). Danach ist „0" ein billiger No-op.
  static std::atomic<bool> s_manual_active{false};
  static std::atomic<bool> s_auto_ensured{false};
  char hf[8] = {0};
  DWORD hfn = GetEnvironmentVariableA("HONEYCORD_HOLD_FRAMERATE", hf, sizeof(hf));
  if (hfn > 0 && hf[0] == '1') {
    ApplyExposure(unique_name_.c_str(), /*hold=*/true);
    s_manual_active.store(true);
    s_auto_ensured.store(false);
  } else if (hfn > 0 && hf[0] == '0') {
    if (s_manual_active.load() || !s_auto_ensured.load()) {
      ApplyExposure(unique_name_.c_str(), /*hold=*/false);
      s_manual_active.store(false);
      s_auto_ensured.store(true);
    }
  }
  // hfn==0 (Env nicht gesetzt): Belichtung gar nicht anfassen.
#endif
  return true;
}

bool VcmCapturer::CaptureStarted() {
  return vcm_ != nullptr &&
         worker_thread_->BlockingCall([&] { return vcm_->CaptureStarted(); });
}

void VcmCapturer::StopCapture() {
  worker_thread_->BlockingCall([&] {
    // Verzoegerte Raten-Pruefung entwerten (laeuft auf demselben Faden).
    if (pruef_lebt_) pruef_lebt_->store(false);
    vcm_->StopCapture();
    // Release reference to VCM.
    vcm_ = nullptr;
  });
}

void VcmCapturer::Destroy() {
  if (!vcm_) return;

  vcm_->DeRegisterCaptureDataCallback();

  StopCapture();
}

VcmCapturer::~VcmCapturer() { Destroy(); }

void VcmCapturer::OnFrame(const VideoFrame& frame) {
#ifdef _WIN32
  // Takt-Messpunkt „ein" (2026-10-23): Abstand zum vorigen Kamerabild, roh,
  // VOR dem VideoAdapter. Nur Aufnahme-Faden (siehe Header), keine Allokation.
  {
    const int64_t q = HcQpc();
    if (qpc_freq_ == 0) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      qpc_freq_ = f.QuadPart > 0 ? static_cast<int64_t>(f.QuadPart) : 1;
      takt_fenster_qpc_ = q;
    }
    if (ein_letzt_qpc_ != 0) {
      const int64_t us = (q - ein_letzt_qpc_) * 1000000 / qpc_freq_;
      ein_us_[ein_pos_] = static_cast<int32_t>((std::min<int64_t>)(us, INT32_MAX));
      ein_pos_ = (ein_pos_ + 1) % kTaktRing;
      ++ein_n_;
    }
    ein_letzt_qpc_ = q;
    // Fuer die Raten-Pruefung (worker_thread_ liest).
    int64_t erwartet = 0;
    erstes_bild_qpc_.compare_exchange_strong(erwartet, q,
                                             std::memory_order_relaxed);
    bilder_gesamt_.fetch_add(1, std::memory_order_relaxed);
    if (q - takt_fenster_qpc_ >= 5 * qpc_freq_) TaktZusammenfassen(q);
  }
  // Geliefert-fps 1x/min: DAS ist die Zahl, die im Abendtest heimlich auf
  // 24/15 fiel. Ab jetzt objektiv im Log statt am Badge abgelesen.
  const int64_t now = static_cast<int64_t>(GetTickCount64());
  if (cam_dbg_start_ms_ == 0) cam_dbg_start_ms_ = now;
  ++cam_dbg_frames_;
  if (now - cam_dbg_start_ms_ >= 60000) {
    CamLog("liefert %.1f fps (%dx%d)",
           cam_dbg_frames_ * 1000.0 / (now - cam_dbg_start_ms_), frame.width(),
           frame.height());
    cam_dbg_frames_ = 0;
    cam_dbg_start_ms_ = now;
  }
#endif
  VideoCapturer::OnFrame(frame);
}

void VcmCapturer::NachAdapterBild() {
#ifdef _WIN32
  // Takt-Messpunkt „aus": Abstand zwischen Bildern, die der VideoAdapter
  // (FramerateController) durchgelassen hat. Gleicher Faden wie OnFrame.
  const int64_t q = HcQpc();
  if (qpc_freq_ == 0) return;  // OnFrame setzt die Frequenz zuerst
  if (aus_letzt_qpc_ != 0) {
    const int64_t us = (q - aus_letzt_qpc_) * 1000000 / qpc_freq_;
    aus_us_[aus_pos_] = static_cast<int32_t>((std::min<int64_t>)(us, INT32_MAX));
    aus_pos_ = (aus_pos_ + 1) % kTaktRing;
    ++aus_n_;
  }
  aus_letzt_qpc_ = q;
#endif
}

void VcmCapturer::TaktZusammenfassen(int64_t jetzt_qpc) {
#ifdef _WIN32
  // Aufnahme-Faden, alle 5 s. Rechnen ohne Allokation (feste Felder); nur die
  // fertige Zeile wird als std::string an den worker_thread_ gegeben, damit
  // das Datei-Schreiben (fopen/fclose) nicht den Bildpfad aufhaelt.
  struct Werte {
    int n;
    double min_ms, mittel_ms, max_ms, p95_ms;
    int ueber;  // > 1,5x Soll
  };
  const double soll_ms = wunsch_fps_ > 0 ? 1000.0 / wunsch_fps_ : 0.0;
  auto auswerten = [&](const int32_t* ring, int n_gesamt) {
    Werte w{n_gesamt, 0, 0, 0, 0, 0};
    const int n = (std::min)(n_gesamt, kTaktRing);
    if (n <= 0) return w;
    int64_t summe = 0;
    int32_t mn = INT32_MAX, mx = 0;
    for (int i = 0; i < n; ++i) {
      const int32_t v = ring[i];
      takt_sortier_[i] = v;
      summe += v;
      if (v < mn) mn = v;
      if (v > mx) mx = v;
      if (soll_ms > 0 && v > soll_ms * 1500.0) ++w.ueber;
    }
    int k = (n * 95 + 99) / 100 - 1;  // ceil(0,95 n) - 1
    if (k < 0) k = 0;
    std::nth_element(takt_sortier_, takt_sortier_ + k, takt_sortier_ + n);
    w.min_ms = mn / 1000.0;
    w.max_ms = mx / 1000.0;
    w.mittel_ms = static_cast<double>(summe) / n / 1000.0;
    w.p95_ms = takt_sortier_[k] / 1000.0;
    return w;
  };
  const Werte ein = auswerten(ein_us_, ein_n_);
  const Werte aus = auswerten(aus_us_, aus_n_);
  char zeile[320];
  std::snprintf(zeile, sizeof(zeile),
                "takt ein: n=%d %.1f/%.1f/%.1f/%.1f ms | aus: n=%d "
                "%.1f/%.1f/%.1f/%.1f ms, >1,5x soll: %d (soll %.1f ms, "
                "Kamera @%d, min/mittel/max/p95)",
                ein.n, ein.min_ms, ein.mittel_ms, ein.max_ms, ein.p95_ms,
                aus.n, aus.min_ms, aus.mittel_ms, aus.max_ms, aus.p95_ms,
                aus.ueber, soll_ms, capability_.maxFPS);
  ein_n_ = aus_n_ = 0;
  ein_pos_ = aus_pos_ = 0;
  takt_fenster_qpc_ = jetzt_qpc;
  if (worker_thread_) {
    worker_thread_->PostTask(
        [z = std::string(zeile)] { CamLog("%s", z.c_str()); });
  }
#endif
}

void VcmCapturer::PlaneRatenPruefung(
    std::shared_ptr<std::atomic<bool>> lebt, int runde, uint32_t n_vorher,
    int64_t qpc_vorher) {
#ifdef _WIN32
  // Ablauf (alles auf worker_thread_):
  //  - alle 2 s nachsehen; erst wenn Bilder fliessen, einen Messpunkt setzen,
  //    2 s spaeter die Rate des Fensters dazwischen beurteilen (die ersten
  //    Bilder nach dem Anlauf zaehlen also nicht mit — Belichtungs-Anlauf).
  //  - < 80 % der Wunschrate -> RateRueckfall (genau einmal).
  //  - nach 10 s noch kein Bild -> RateRueckfall („schwarz").
  worker_thread_->PostDelayedTask(
      [this, lebt, runde, n_vorher, qpc_vorher] {
        if (!lebt || !lebt->load() || !vcm_ || !rate_gesenkt_) return;
        const int64_t jetzt = HcQpc();
        const uint32_t n = bilder_gesamt_.load(std::memory_order_relaxed);
        const bool fliesst =
            erstes_bild_qpc_.load(std::memory_order_relaxed) != 0;
        if (qpc_vorher == 0) {
          if (fliesst) {
            PlaneRatenPruefung(lebt, runde + 1, n, jetzt);
          } else if (runde >= 4) {
            RateRueckfall("nach 10 s kein Bild");
          } else {
            PlaneRatenPruefung(lebt, runde + 1, 0, 0);
          }
          return;
        }
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const double sek =
            static_cast<double>(jetzt - qpc_vorher) /
            static_cast<double>(f.QuadPart > 0 ? f.QuadPart : 1);
        const double fps = sek > 0 ? (n - n_vorher) / sek : 0.0;
        if (fps < 0.8 * wunsch_fps_) {
          char grund[96];
          std::snprintf(grund, sizeof(grund),
                        "geliefert %.1f fps < 80%% von %d (Fenster %.1f s)",
                        fps, wunsch_fps_, sek);
          RateRueckfall(grund);
        } else {
          rate_gesenkt_ = false;  // bestaetigt, keine weitere Pruefung
          CamLog("Rate %d bestaetigt: geliefert %.1f fps (Fenster %.1f s)",
                 wunsch_fps_, fps, sek);
        }
      },
      webrtc::TimeDelta::Seconds(2));
#endif
}

void VcmCapturer::RateRueckfall(const char* grund) {
#ifdef _WIN32
  // worker_thread_ (wie StartCapture/StopCapture via BlockingCall). Hoechstens
  // einmal je Kamera-Start: rate_gesenkt_ wird vorher geloescht, und
  // PlaneRatenPruefung laeuft nur, solange es gesetzt ist.
  rate_gesenkt_ = false;
  if (!vcm_) return;
  CamLog("Rueckfall auf %d: %s", rueckfall_cap_.maxFPS, grund);
  const int64_t t0 = static_cast<int64_t>(GetTickCount64());
  vcm_->StopCapture();
  capability_ = rueckfall_cap_;
  // capability != _requestedCapability -> VideoCaptureDS baut den Graphen mit
  // der alten Faehigkeit neu (DisconnectGraph + SetCameraOutput).
  const int32_t r = vcm_->StartCapture(capability_);
  CamLog("Rueckfall-Start %s: %dx%d@%d Format=%s (%lldms)",
         r == 0 ? "OK" : "FEHLGESCHLAGEN", capability_.width,
         capability_.height, capability_.maxFPS,
         CamVideoTypeName(capability_.videoType),
         (long long)(static_cast<int64_t>(GetTickCount64()) - t0));
#endif
}

webrtc::scoped_refptr<CapturerTrackSource> CapturerTrackSource::Create(
    webrtc::Thread* worker_thread) {
  const size_t kWidth = 640;
  const size_t kHeight = 480;
  const size_t kFps = 30;
  std::shared_ptr<VcmCapturer> capturer;
  std::unique_ptr<webrtc::VideoCaptureModule::DeviceInfo> info(
      webrtc::VideoCaptureFactory::CreateDeviceInfo());
  if (!info) {
    return nullptr;
  }
  int num_devices = info->NumberOfDevices();
  for (int i = 0; i < num_devices; ++i) {
    capturer = VcmCapturer::Create(worker_thread, kWidth, kHeight, kFps, i);
    if (capturer) {
      return webrtc::scoped_refptr<CapturerTrackSource>(
          new webrtc::RefCountedObject<CapturerTrackSource>(capturer));
    }
  }

  return nullptr;
}

}  // namespace internal
}  // namespace webrtc
