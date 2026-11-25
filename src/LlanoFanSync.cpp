// LlanoFanSync - drives a llano V12 Ultra cooling pad from the ROG laptop's own fan speeds,
// with a tray icon and a settings / monitor window.
//
// Laptop fans + CPU temp: ASUS ATKACPI driver, DSTS method (what Armoury Crate / G-Helper read). No admin.
//   CPU fan 0x00110013, GPU fan 0x00110014, system fan 0x00110031 (value - 0x10000 = rpm/100), CPU temp 0x00120094.
// Pad: HOLTEK HID 374A:B101, 8-byte feature report (report id 0):
//   [mode_ctrl, fan_speed%, on_off, light_on_off<<7 | light_mode, light_color_mode, light_speed, light_power, ~sum]
//   Read back: mode 0x80 = software control, 0x88 = the pad's own speed wheel is in control.
// Monitor: see "hardware monitor" below for what is read always and what only while shown.
//
// Build (x64): run build.bat (finds Visual Studio 2019+ with the C++ tools), output build\LlanoFanSync.exe

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#include <evntrace.h>
#include <evntcons.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <initguid.h>
#include <devpkey.h>
#include <devguid.h>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <initializer_list>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;

static wchar_t g_ini[MAX_PATH], g_log[MAX_PATH], g_exe[MAX_PATH];

static void Log(const wchar_t* fmt, ...) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_log, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 1000000) DeleteFileW(g_log);
    FILE* f = _wfopen(g_log, L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(f, L"%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(f, fmt, a); va_end(a);
    fputwc(L'\n', f); fclose(f);
}

// ======================================================================= ASUS ACPI
struct Atk {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool Get(DWORD dev, DWORD& v) {
        if (h == INVALID_HANDLE_VALUE)
            h = CreateFileW(L"\\\\.\\ATKACPI", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD in[3] = { 0x53545344 /*DSTS*/, 4, dev }, out[4] = {}, got = 0;
        if (!DeviceIoControl(h, 0x0022240C, in, sizeof in, out, sizeof out, &got, nullptr)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; return false; }
        v = out[0]; return true;
    }
    bool Rpm(DWORD dev, int& rpm) { DWORD v; if (!Get(dev, v)) return false; rpm = std::max(0, (int)v - 65536) * 100; return true; }
};

// ======================================================================= llano pad
struct Pad {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool Open() {
        GUID g; HidD_GetHidGuid(&g);
        HDEVINFO set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return false;
        SP_DEVICE_INTERFACE_DATA di = { sizeof di };
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &di); i++) {
            DWORD need = 0; SetupDiGetDeviceInterfaceDetailW(set, &di, nullptr, 0, &need, nullptr);
            std::vector<BYTE> buf(need);
            auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf.data(); det->cbSize = sizeof *det;
            if (!SetupDiGetDeviceInterfaceDetailW(set, &di, det, need, nullptr, nullptr)) continue;
            std::wstring p = det->DevicePath; for (auto& c : p) c = towlower(c);
            if (p.find(L"vid_374a&pid_b101") == std::wstring::npos) continue;
            h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) break;
        }
        SetupDiDestroyDeviceInfoList(set);
        return h != INVALID_HANDLE_VALUE;
    }
    void Drop() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    bool Get(BYTE st[8]) {
        if (h == INVALID_HANDLE_VALUE && !Open()) return false;
        BYTE b[9] = {};
        if (!HidD_GetFeature(h, b, 9)) { Drop(); return false; }
        memcpy(st, b + 1, 8); return true;
    }
    bool SetSpeed(const BYTE st[8], int speed) {
        BYTE b[9] = {};
        b[1] = 1; b[2] = (BYTE)speed;                        // 1 = software control
        for (int i = 2; i < 7; i++) b[i + 1] = st[i];        // keep on/off + lighting untouched
        BYTE sum = 0; for (int i = 1; i < 8; i++) sum += b[i];
        b[8] = (BYTE)~sum;
        if (!HidD_SetFeature(h, b, 9)) { Drop(); return false; }
        return true;
    }
};

// ======================================================================= settings
static const wchar_t* kGpuDefault = L"20:1,30:8,75:72,79:72,100:100";
static const wchar_t* kCpuDefault = L"25:1,35:8,75:72,79:72,100:100";
static const wchar_t* kSysDefault = L"50:0,51:1,100:1";     // the old rule: above 50% -> pad 1%, otherwise off

struct Curve {
    std::vector<std::pair<double, double>> pts;
    void Parse(const wchar_t* s) {
        pts.clear();
        while (*s) {
            wchar_t* e; double x = wcstod(s, &e); if (e == s || *e != L':') break;
            double y = wcstod(e + 1, &e); pts.push_back({ std::clamp(x, 0.0, 100.0), std::clamp(y, 0.0, 100.0) });
            s = e; while (*s == L',' || *s == L' ') s++;
        }
        std::sort(pts.begin(), pts.end());
        if (pts.empty()) pts = { { 50, 50 }, { 100, 100 } };
    }
    std::wstring Str() const {
        std::wstring r; wchar_t b[32];
        for (auto& p : pts) { swprintf(b, 32, L"%s%g:%g", r.empty() ? L"" : L",", p.first, p.second); r += b; }
        return r;
    }
    // fan stopped -> 0; spinning at/below the first point -> first point's value; linear between points
    double Eval(double pct) const {
        if (pct <= 0 || pts.empty()) return 0;
        if (pct <= pts[0].first) return pts[0].second;
        for (size_t i = 1; i < pts.size(); i++)
            if (pct <= pts[i].first) {
                auto [x0, y0] = pts[i - 1]; auto [x1, y1] = pts[i];
                return x1 == x0 ? y1 : y0 + (pct - x0) / (x1 - x0) * (y1 - y0);
            }
        return pts.back().second;
    }
};

// Max rpm of this laptop's fans (G634JZ, measured at full speed) = 100%.
static const int kCpuMaxRpm = 6800, kGpuMaxRpm = 6600, kSysMaxRpm = 7300;

// Everything the history chart / overlay can show. Any combination can be ticked; each unit gets its own band.
static const int kMaxDisk = 4;
enum {
    H_GPUF, H_CPUF, H_SYSF, H_PAD, H_GPURPM, H_CPURPM, H_SYSRPM, H_PADRPM, H_PADDB,
    H_GPUT, H_CPUT, H_SYST, H_GPUW, H_CPUW, H_COREW, H_IGPUW,
    H_GPUCLK, H_GPUMEM, H_PCLK, H_ECLK, H_GPUUSE, H_PUSE, H_EUSE, H_ALLUSE,
    H_RAMPCT, H_RAMGB, H_COMMITPCT, H_COMMITGB, H_FPS,
    H_DISK, H_COUNT = H_DISK + 4 * kMaxDisk          // per disk: temperature, active time %, read MB/s, write MB/s
};
enum Unit { U_PCT, U_RPM, U_C, U_W, U_MHZ, U_GB, U_MBS, U_FPS, U_DB, U_COUNT };
static const wchar_t* kUnitSuffix[U_COUNT] = { L"%", L" rpm", L" °C", L" W", L" MHz", L" GB", L" MB/s", L" fps", L" dB" };
static const wchar_t* kUnitName[U_COUNT] = { L"%", L"rpm", L"°C", L"W", L"MHz", L"GB", L"MB/s", L"fps", L"dB" };
struct SeriesDef { const wchar_t* key; const wchar_t* name; const wchar_t* shortName; DWORD col; int unit, group; };
static wchar_t g_dName[4 * kMaxDisk][64], g_dShort[4 * kMaxDisk][32], g_fpsName[96] = L"FPS (game in front)";
static bool g_diskPresent[kMaxDisk];
static int g_diskNum[kMaxDisk];
#define DISK_SERIES(d, c1, c2, c3, c4) \
    { L"disk" #d "_temp",   g_dName[d * 4 + 0], g_dShort[d * 4 + 0], c1, U_C,   7 + d }, \
    { L"disk" #d "_active", g_dName[d * 4 + 1], g_dShort[d * 4 + 1], c2, U_PCT, 7 + d }, \
    { L"disk" #d "_read",   g_dName[d * 4 + 2], g_dShort[d * 4 + 2], c3, U_MBS, 7 + d }, \
    { L"disk" #d "_write",  g_dName[d * 4 + 3], g_dShort[d * 4 + 3], c4, U_MBS, 7 + d }
static SeriesDef kSeries[H_COUNT] = {
    { L"gpu_fan",   L"GPU fan",              L"GPU fan",  0x5FA300, U_PCT, 0 },
    { L"cpu_fan",   L"CPU fan",              L"CPU fan",  0x2F80ED, U_PCT, 0 },
    { L"sys_fan",   L"System fan",           L"Sys fan",  0x8B5CF6, U_PCT, 0 },
    { L"pad",       L"Llano pad",            L"Pad",      0xF2994A, U_PCT, 0 },
    { L"gpu_rpm",   L"GPU fan rpm",          L"GPU rpm",  0x5FA300, U_RPM, 0 },
    { L"cpu_rpm",   L"CPU fan rpm",          L"CPU rpm",  0x2F80ED, U_RPM, 0 },
    { L"sys_rpm",   L"System fan rpm",       L"Sys rpm",  0x8B5CF6, U_RPM, 0 },
    { L"pad_rpm",   L"Llano pad rpm", L"Pad rpm",  0xF2994A, U_RPM, 0 },
    { L"pad_db",    L"Llano pad noise (est.)", L"Pad dB", 0xF2994A, U_DB, 0 },
    { L"gpu_temp",  L"GPU temperature",      L"GPU temp",      0x5FA300, U_C, 1 },
    { L"cpu_temp",  L"CPU temperature",      L"CPU temp",      0x2F80ED, U_C, 1 },
    { L"sys_temp",  L"System temperature",   L"Sys temp",      0x8B5CF6, U_C, 1 },
    { L"gpu_w",     L"GPU power",            L"GPU power",      0x5FA300, U_W, 2 },
    { L"cpu_w",     L"CPU package power",    L"CPU power",      0x2F80ED, U_W, 2 },
    { L"cores_w",   L"CPU cores power",      L"Cores power",    0x0EA5E9, U_W, 2 },
    { L"igpu_w",    L"iGPU power",           L"iGPU power",     0x0D9488, U_W, 2 },
    { L"gpu_clk",   L"GPU core clock",       L"GPU clock",      0x5FA300, U_MHZ, 3 },
    { L"gpu_mem",   L"GPU memory clock",     L"GPU mem",  0xCA8A04, U_MHZ, 3 },
    { L"p_clk",     L"P-core clock",         L"P clock",  0x2F80ED, U_MHZ, 3 },
    { L"e_clk",     L"E-core clock",         L"E clock",  0xEC4899, U_MHZ, 3 },
    { L"gpu_use",   L"GPU usage",            L"GPU use",  0x0D9488, U_PCT, 4 },
    { L"p_use",     L"P-core usage",         L"P use",    0x0EA5E9, U_PCT, 4 },
    { L"e_use",     L"E-core usage",         L"E use",    0xEC4899, U_PCT, 4 },
    { L"all_use",   L"All-core usage",       L"All use",  0x6B7280, U_PCT, 4 },
    { L"ram_pct",   L"Memory used %",        L"RAM",      0xEAB308, U_PCT, 5 },
    { L"ram_gb",    L"Memory used",          L"RAM",      0xEAB308, U_GB, 5 },
    { L"commit_pct", L"Committed % of limit", L"Commit",  0xF97316, U_PCT, 5 },
    { L"commit_gb", L"Committed",            L"Commit",   0xF97316, U_GB, 5 },
    { L"fps",       g_fpsName,               L"FPS",      0xF43F5E, U_FPS, 6 },
    DISK_SERIES(0, 0xF59E0B, 0x06B6D4, 0x22C55E, 0xEF4444),
    DISK_SERIES(1, 0xD946EF, 0x3B82F6, 0x84CC16, 0xFB7185),
    DISK_SERIES(2, 0xA3A3A3, 0x14B8A6, 0x4ADE80, 0xF472B6),
    DISK_SERIES(3, 0x78716C, 0x6366F1, 0xBEF264, 0xFDA4AF),
};
static bool SeriesShown(int i) { return i < H_DISK || g_diskPresent[(i - H_DISK) / 4]; }
static unsigned long long ShownMask() { unsigned long long m = 0; for (int i = 0; i < H_COUNT; i++) if (SeriesShown(i)) m |= 1ull << i; return m; }
// Physical disks (up to 4) and their drive letters; names like "C: temperature". Opening a disk with no access rights never spins it up.
static void EnumDisks() {
    int n = 0;
    for (int i = 0; i < 16 && n < kMaxDisk; i++) {
        wchar_t p[64]; swprintf(p, 64, L"\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        CloseHandle(h);
        g_diskNum[n] = i; g_diskPresent[n] = true;
        std::wstring letters;
        for (wchar_t c = L'C'; c <= L'Z'; c++) {
            wchar_t v[16]; swprintf(v, 16, L"\\\\.\\%c:", c);
            HANDLE vh = CreateFileW(v, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (vh == INVALID_HANDLE_VALUE) continue;
            BYTE buf[256]; DWORD got;
            if (DeviceIoControl(vh, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, buf, sizeof buf, &got, nullptr) &&
                ((VOLUME_DISK_EXTENTS*)buf)->Extents[0].DiskNumber == (DWORD)i) { if (!letters.empty()) letters += L" "; letters += c; letters += L":"; }
            CloseHandle(vh);
        }
        if (letters.empty()) { wchar_t b[16]; swprintf(b, 16, L"Disk %d", i); letters = b; }
        static const wchar_t* what[4] = { L"temperature", L"active time", L"read", L"write" };
        static const wchar_t* sw[4] = { L"temp", L"active", L"read", L"write" };
        for (int k = 0; k < 4; k++) {
            swprintf(g_dName[n * 4 + k], 64, L"%s %s", letters.c_str(), what[k]);
            swprintf(g_dShort[n * 4 + k], 32, L"%s %s", letters.c_str(), sw[k]);
        }
        n++;
    }
}
static const wchar_t* kDefaultSeries = L"gpu_fan,cpu_fan,sys_fan,pad,gpu_temp,cpu_temp,gpu_w,cpu_w";

struct Settings {
    int intervalMs, logChanges;
    Curve cpu, gpu, sys;
    // history chart + overlay
    unsigned long long chartMask; int chartRange, recordHidden, theme;          // theme: 0 follow Windows, 1 light, 2 dark
    int ovOn, ovLocked, ovMode, ovRange, ovLayout, ovSize, ovOutline; // mode: 0 numbers, 1 chart, 2 numbers + chart
    unsigned long long ovMask; int ovTextColor, ovBg, ovBgColor, ovBgOpacity, ovOpacity;
    int ovAnchor, ovX, ovY, ovW, ovH;                                 // anchor: 0 free (dragged), 1..9 = TL TC TR ML MC MR BL BC BR
};
static const wchar_t* kDefaultOvSeries = L"gpu_temp,cpu_temp,gpu_w,cpu_w,gpu_fan,cpu_fan,pad";
static unsigned long long ParseMask(const wchar_t* s) {
    unsigned long long m = 0;
    std::wstring all = s;
    size_t a = 0;
    while (a <= all.size()) {
        size_t e = all.find(L',', a); if (e == std::wstring::npos) e = all.size();
        std::wstring tok = all.substr(a, e - a);
        for (int i = 0; i < H_COUNT; i++) if (tok == kSeries[i].key) m |= 1ull << i;
        a = e + 1;
    }
    return m;
}
static std::wstring MaskStr(unsigned long long m) {
    std::wstring r;
    for (int i = 0; i < H_COUNT; i++) if (m & (1ull << i)) { if (!r.empty()) r += L","; r += kSeries[i].key; }
    return r;
}
static const int kRangeMin[] = { 1, 5, 15, 30, 60 };
static const wchar_t* kRangeName[] = { L"1 minute", L"5 minutes", L"15 minutes", L"30 minutes", L"1 hour" };
static const int kNoPos = -99999;

// llano V12 Ultra noise, from a review's measurements (room 45.5 dB subtracted as sound power):
// 300 rpm 46 dB, 600 49.5, 900 54.9, 1800 64.7, 2800 67 (total) -> pad alone below.
// Pad rpm as the pad itself displays it: 1% 300, 4% 400, then 300 + 25 * speed% (8% 500, 12% 600 ... 100% 2800).
static int PadRpm(int pct) {
    if (pct <= 0) return 0;
    pct = std::min(pct, 100);
    return pct < 4 ? 300 + (pct - 1) * 100 / 3 : 300 + 25 * pct;
}
static double PadNoise(int pct) {
    static const double t[][2] = { { 300, 36.4 }, { 600, 47.3 }, { 900, 54.4 }, { 1800, 64.6 }, { 2800, 67.0 } };
    int rpm = PadRpm(pct);
    if (rpm <= 0) return -1000;
    if (rpm <= 300) return t[0][1] + 50.0 * log10(rpm / 300.0);
    for (int i = 1; i < 5; i++)
        if (rpm <= t[i][0]) {
            double f = (log10(rpm) - log10(t[i - 1][0])) / (log10(t[i][0]) - log10(t[i - 1][0]));
            return t[i - 1][1] + f * (t[i][1] - t[i - 1][1]);
        }
    return t[4][1];
}
static const char* kDefaultIni =
    "[sync]\r\n"
    "; Edited from the LlanoFanSync window (tray icon). Manual edits here are picked up within ~5 s.\r\n"
    "; Curves are laptop-fan% : pad-fan% points (fan% = rpm / max: CPU 6800, GPU 6600, system 7300).\r\n"
    "; Laptop fan at 0 rpm -> 0. Spinning but at/below the first point -> the first point's value.\r\n"
    "; The pad follows whichever of the three gives the highest speed.\r\n"
    "gpu_curve=20:1,30:8,75:72,79:72,100:100\r\n"
    "cpu_curve=25:1,35:8,75:72,79:72,100:100\r\n"
    "sys_curve=50:0,51:1,100:1\r\n"
    "interval_ms=1000\r\n"
    "log_changes=0\r\n";

static void LoadSettings(Settings& s) {
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(g_ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, kDefaultIni, (DWORD)strlen(kDefaultIni), &w, nullptr); CloseHandle(f); }
    }
    auto I = [](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"sync", k, d, g_ini); };
    auto U = [](const wchar_t* k, int d) { return (int)GetPrivateProfileIntW(L"ui", k, d, g_ini); };
    wchar_t buf[512];
    if (!GetPrivateProfileStringW(L"sync", L"sys_curve", L"", buf, 512, g_ini)) {   // older ini: turn the system rule into a curve
        int t = std::clamp(I(L"sys_threshold_pct", 50), 0, 98), v = std::clamp(I(L"sys_speed", 1), 0, 100);
        swprintf(buf, 512, L"%d:0,%d:%d,100:%d", std::max(1, t), std::max(1, t) + 1, v, v);
        WritePrivateProfileStringW(L"sync", L"sys_curve", buf, g_ini);
    }
    for (auto old : { L"cpu_max_rpm", L"gpu_max_rpm", L"sys_max_rpm", L"learn_max", L"noise_k", L"sys_threshold_pct", L"sys_speed" })   // settings that no longer exist
        if (GetPrivateProfileStringW(L"sync", old, L"", buf, 512, g_ini)) WritePrivateProfileStringW(L"sync", old, nullptr, g_ini);
    s.intervalMs = std::clamp(I(L"interval_ms", 1000), 250, 10000);
    s.logChanges = I(L"log_changes", 0);
    GetPrivateProfileStringW(L"sync", L"cpu_curve", kCpuDefault, buf, 512, g_ini); s.cpu.Parse(buf);
    GetPrivateProfileStringW(L"sync", L"gpu_curve", kGpuDefault, buf, 512, g_ini); s.gpu.Parse(buf);
    GetPrivateProfileStringW(L"sync", L"sys_curve", kSysDefault, buf, 512, g_ini); s.sys.Parse(buf);
    for (auto old : { L"chart_metric", L"overlay_transparent" })
        if (GetPrivateProfileStringW(L"ui", old, L"", buf, 512, g_ini)) WritePrivateProfileStringW(L"ui", old, nullptr, g_ini);
    GetPrivateProfileStringW(L"ui", L"chart_series", kDefaultSeries, buf, 512, g_ini); s.chartMask = ParseMask(buf);
    s.chartRange = std::clamp(U(L"chart_range", 1), 0, 4);
    s.recordHidden = U(L"record_hidden", 0) != 0;
    s.theme = std::clamp(U(L"theme", 0), 0, 2);
    s.ovOn = U(L"overlay", 0) != 0;
    s.ovLocked = U(L"overlay_locked", 0) != 0;
    s.ovMode = std::clamp(U(L"overlay_mode", 0), 0, 2);
    GetPrivateProfileStringW(L"ui", L"overlay_series", kDefaultOvSeries, buf, 512, g_ini); s.ovMask = ParseMask(buf);
    s.ovRange = std::clamp(U(L"overlay_range", 0), 0, 4);
    s.ovLayout = std::clamp(U(L"overlay_layout", 0), 0, 1);
    s.ovSize = std::clamp(U(L"overlay_text_size", 1), 0, 4);
    s.ovOutline = U(L"overlay_outline", 1) != 0;
    s.ovTextColor = U(L"overlay_text_color", 0xF9FAFB) & 0xFFFFFF;
    s.ovBg = U(L"overlay_background", 1) != 0;
    s.ovBgColor = U(L"overlay_bg_color", 0x111827) & 0xFFFFFF;
    s.ovBgOpacity = std::clamp(U(L"overlay_bg_opacity", 70), 0, 100);
    s.ovOpacity = std::clamp(U(L"overlay_opacity", 100), 10, 100);
    s.ovAnchor = std::clamp(U(L"overlay_anchor", 3), 0, 9);
    s.ovX = U(L"overlay_x", kNoPos); s.ovY = U(L"overlay_y", kNoPos);
    s.ovW = std::clamp(U(L"overlay_w", 360), 160, 2400); s.ovH = std::clamp(U(L"overlay_h", 150), 60, 1600);
}
static void WriteInt(const wchar_t* k, int v, const wchar_t* sec = L"sync") { wchar_t b[16]; swprintf(b, 16, L"%d", v); WritePrivateProfileStringW(sec, k, b, g_ini); }
static void SaveSettings(const Settings& s) {
    WritePrivateProfileStringW(L"sync", L"gpu_curve", s.gpu.Str().c_str(), g_ini);
    WritePrivateProfileStringW(L"sync", L"cpu_curve", s.cpu.Str().c_str(), g_ini);
    WritePrivateProfileStringW(L"sync", L"sys_curve", s.sys.Str().c_str(), g_ini);
    WriteInt(L"interval_ms", s.intervalMs);
    const wchar_t* u = L"ui";
    WritePrivateProfileStringW(u, L"chart_series", MaskStr(s.chartMask).c_str(), g_ini);
    WriteInt(L"chart_range", s.chartRange, u); WriteInt(L"record_hidden", s.recordHidden, u); WriteInt(L"theme", s.theme, u);
    WriteInt(L"overlay", s.ovOn, u); WriteInt(L"overlay_locked", s.ovLocked, u); WriteInt(L"overlay_mode", s.ovMode, u);
    WritePrivateProfileStringW(u, L"overlay_series", MaskStr(s.ovMask).c_str(), g_ini);
    WriteInt(L"overlay_range", s.ovRange, u); WriteInt(L"overlay_layout", s.ovLayout, u); WriteInt(L"overlay_text_size", s.ovSize, u);
    WriteInt(L"overlay_outline", s.ovOutline, u); WriteInt(L"overlay_text_color", s.ovTextColor, u);
    WriteInt(L"overlay_background", s.ovBg, u); WriteInt(L"overlay_bg_color", s.ovBgColor, u);
    WriteInt(L"overlay_bg_opacity", s.ovBgOpacity, u); WriteInt(L"overlay_opacity", s.ovOpacity, u);
    WriteInt(L"overlay_anchor", s.ovAnchor, u);
    WriteInt(L"overlay_x", s.ovX, u); WriteInt(L"overlay_y", s.ovY, u); WriteInt(L"overlay_w", s.ovW, u); WriteInt(L"overlay_h", s.ovH, u);
}
static FILETIME IniTime() { WIN32_FILE_ATTRIBUTE_DATA fa = {}; GetFileAttributesExW(g_ini, GetFileExInfoStandard, &fa); return fa.ftLastWriteTime; }

// ======================================================================= start with Windows
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool GetStartup() {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k)) return false;
    DWORD sz = 0; bool ok = RegQueryValueExW(k, L"LlanoFanSync", nullptr, nullptr, nullptr, &sz) == ERROR_SUCCESS;
    RegCloseKey(k); return ok;
}
static void SetStartup(bool on) {
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)) return;
    if (on) { std::wstring v = L"\"" + std::wstring(g_exe) + L"\" /background";
              RegSetValueExW(k, L"LlanoFanSync", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2)); }
    else RegDeleteValueW(k, L"LlanoFanSync");
    RegCloseKey(k);
}
static void MigrateStartupShortcut() {   // first version used a Startup-folder shortcut; switch it to the Run key
    PWSTR dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &dir))) {
        std::wstring lnk = std::wstring(dir) + L"\\LlanoFanSync.lnk";
        if (GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES && DeleteFileW(lnk.c_str())) SetStartup(true);
        CoTaskMemFree(dir);
    }
}

// ======================================================================= hardware monitor
// Each sensor group is opened only while something shows it: the main window's cards, or a sensor ticked in the
// chart / overlay. Untouched sensors are not read at all. (Fans + pad are always read: they drive the pad.)
//   CPU clocks + usage: PDH "Processor Information" split by CPU-set efficiency class (P / E cores)
//   CPU power: PDH "Energy Meter" (Intel RAPL)      System temp: PDH "Thermal Zone Information"
//   GPU: NVML, skipped while the dGPU sleeps (D3) so it is never woken up
//   Memory: GlobalMemoryStatusEx                     Disks: PDH "PhysicalDisk" + storage temperature query
//   FPS: ETW DXGI / D3D9 present events (needs admin or "Performance Log Users", which this account has)
enum { M_CPU = 1, M_POW = 2, M_TZ = 4, M_GPU = 8, M_CPUT = 16, M_RAM = 32, M_DISK = 64, M_DISKT = 128, M_FPS = 256 };
typedef int (*nvmlFn0)();
typedef int (*nvmlHandleFn)(unsigned, void**);
typedef int (*nvmlU1Fn)(void*, unsigned, unsigned*);
typedef int (*nvmlUFn)(void*, unsigned*);
typedef int (*nvmlNameFn)(void*, char*, unsigned);
struct NvUtil { unsigned gpu, mem; };
typedef int (*nvmlUtilFn)(void*, NvUtil*);

struct Monitor {
    bool inited = false;
    PDH_HQUERY qCpu = nullptr, qPow = nullptr, qTz = nullptr, qDisk = nullptr;
    PDH_HCOUNTER cUtil = nullptr, cPerf = nullptr, cFreq = nullptr, cPkg = nullptr, cCores = nullptr, cIgpu = nullptr, cTz = nullptr;
    PDH_HCOUNTER cIdle = nullptr, cRd = nullptr, cWr = nullptr;
    std::vector<int> cls;      // efficiency class by logical processor (group 0)
    int maxCls = 0; bool hybrid = false;
    HMODULE nv = nullptr; void* dev = nullptr; bool nvInit = false;
    nvmlFn0 nInit = nullptr, nShutdown = nullptr; nvmlHandleFn nHandle = nullptr;
    nvmlU1Fn nTemp = nullptr, nClock = nullptr; nvmlUFn nPower = nullptr; nvmlUtilFn nUtil = nullptr; nvmlNameFn nName = nullptr;
    DEVINST gpuInst = 0;
    HANDLE diskH[kMaxDisk] = {};
    ULONGLONG diskTAt[kMaxDisk] = {};
    // results (-1 / -1000 = not read)
    bool have = false;                // CPU clocks + usage ready
    double pGHz = 0, eGHz = 0, pUse = 0, eUse = 0, allUse = 0, cpuW = -1, coresW = -1, igpuW = -1, sysT = -1000;
    int gpuT = -1, gpuClk = -1, gpuMem = -1, gpuUse = -1; double gpuW = -1;
    int gpuState = 0;                 // 1 = D0 active, 4 = D3 asleep, 0 = unknown / not read
    double ramPct = -1, ramGB = -1, ramTotal = -1, commitPct = -1, commitGB = -1;
    double dT[kMaxDisk] = { -1000, -1000, -1000, -1000 }, dAct[kMaxDisk] = { -1, -1, -1, -1 }, dRd[kMaxDisk] = { -1, -1, -1, -1 }, dWr[kMaxDisk] = { -1, -1, -1, -1 };
    std::wstring cpuName, gpuName;

    void Init() {
        inited = true;
        ULONG len = 0; GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
        std::vector<BYTE> b(len);
        if (len && GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)b.data(), len, &len, GetCurrentProcess(), 0)) {
            int minC = 255;
            for (ULONG off = 0; off < len;) {
                auto* p = (PSYSTEM_CPU_SET_INFORMATION)(b.data() + off);
                if (p->Type == CpuSetInformation && p->CpuSet.Group == 0) {
                    int i = p->CpuSet.LogicalProcessorIndex;
                    if ((int)cls.size() <= i) cls.resize(i + 1, 0);
                    cls[i] = p->CpuSet.EfficiencyClass;
                    maxCls = std::max(maxCls, (int)p->CpuSet.EfficiencyClass); minC = std::min(minC, (int)p->CpuSet.EfficiencyClass);
                }
                off += p->Size;
            }
            hybrid = maxCls != minC;
        }
        wchar_t nm[128] = {}; DWORD sz = sizeof nm;
        RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, nm, &sz);
        cpuName = nm;
        for (auto cut : { L"13th Gen ", L"14th Gen ", L"12th Gen ", L"Intel(R) ", L"Core(TM) ", L"AMD " })
            for (size_t at; (at = cpuName.find(cut)) != std::wstring::npos;) cpuName.erase(at, wcslen(cut));
        while (!cpuName.empty() && cpuName.back() == L' ') cpuName.pop_back();
        FindGpu();
    }
    static void CloseQ(PDH_HQUERY& q) { if (q) { PdhCloseQuery(q); q = nullptr; } }
    static bool OpenQ(PDH_HQUERY& q, std::initializer_list<std::pair<const wchar_t*, PDH_HCOUNTER*>> cs) {
        if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) { q = nullptr; return false; }
        for (auto& c : cs) if (PdhAddEnglishCounterW(q, c.first, 0, c.second) != ERROR_SUCCESS) *c.second = nullptr;
        PdhCollectQueryData(q);
        return true;
    }
    void Stop() { Sample(0, 0); }
    void FindGpu() {
        HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return;
        SP_DEVINFO_DATA d = { sizeof d };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); i++) {
            wchar_t id[256]; if (!SetupDiGetDeviceInstanceIdW(set, &d, id, 256, nullptr)) continue;
            if (wcsstr(id, L"VEN_10DE")) {
                gpuInst = d.DevInst;
                wchar_t nm[128] = {};
                if (SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, nullptr, (BYTE*)nm, sizeof nm, nullptr)) gpuName = nm;
                for (auto cut : { L"NVIDIA ", L"GeForce ", L" Laptop GPU", L" GPU" })
                    for (size_t at; (at = gpuName.find(cut)) != std::wstring::npos;) gpuName.erase(at, wcslen(cut));
                break;
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    int GpuPowerState() {          // reads Windows' cached D-state; does not touch (or wake) the GPU
        if (!gpuInst) return 0;
        CM_POWER_DATA pd = {}; ULONG sz = sizeof pd; DEVPROPTYPE t;
        if (CM_Get_DevNode_PropertyW(gpuInst, &DEVPKEY_Device_PowerData, &t, (PBYTE)&pd, &sz, 0) != CR_SUCCESS) return 0;
        return (int)pd.PD_MostRecentPowerState;
    }
    bool NvUp() {
        if (nvInit) return true;
        if (!nv) {
            nv = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!nv) return false;
            nInit = (nvmlFn0)GetProcAddress(nv, "nvmlInit_v2"); nShutdown = (nvmlFn0)GetProcAddress(nv, "nvmlShutdown");
            nHandle = (nvmlHandleFn)GetProcAddress(nv, "nvmlDeviceGetHandleByIndex_v2");
            nTemp = (nvmlU1Fn)GetProcAddress(nv, "nvmlDeviceGetTemperature"); nClock = (nvmlU1Fn)GetProcAddress(nv, "nvmlDeviceGetClockInfo");
            nPower = (nvmlUFn)GetProcAddress(nv, "nvmlDeviceGetPowerUsage"); nUtil = (nvmlUtilFn)GetProcAddress(nv, "nvmlDeviceGetUtilizationRates");
            nName = (nvmlNameFn)GetProcAddress(nv, "nvmlDeviceGetName");
        }
        if (!nInit || !nHandle || nInit() != 0) return false;
        if (nHandle(0, &dev) != 0) { nShutdown(); return false; }
        nvInit = true;
        return true;
    }
    void NvDown() { if (nvInit && nShutdown) nShutdown(); nvInit = false; dev = nullptr; }

    static bool Arr(PDH_HCOUNTER c, std::vector<BYTE>& buf, PDH_FMT_COUNTERVALUE_ITEM_W*& items, DWORD& n) {
        if (!c) return false;
        DWORD sz = 0; n = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &sz, &n, nullptr) != PDH_MORE_DATA) return false;
        buf.resize(sz);
        items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
        return PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &sz, &n, items) == ERROR_SUCCESS;
    }
    static double One(PDH_HCOUNTER c, double scale) {
        PDH_FMT_COUNTERVALUE v;
        if (!c || PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS || v.CStatus != ERROR_SUCCESS) return -1;
        return v.doubleValue * scale;
    }
    static int Lp(const wchar_t* inst) {     // "0,12" -> 12 ; totals -> -1
        if (inst[0] != L'0' || inst[1] != L',' || !iswdigit(inst[2])) return -1;
        return _wtoi(inst + 2);
    }
    static int DiskSlot(const wchar_t* inst) {   // "1 D:" -> slot of physical disk 1 ; "_Total" -> -1
        if (!iswdigit(inst[0])) return -1;
        int n = _wtoi(inst);
        for (int d = 0; d < kMaxDisk; d++) if (g_diskPresent[d] && g_diskNum[d] == n) return d;
        return -1;
    }
    // f = which groups are needed (M_*), need = which history sensors are shown (for per-disk temperatures)
    void Sample(int f, unsigned long long need) {
        if (f && !inited) Init();
        // CPU clocks + usage
        if (f & M_CPU) {
            if (!qCpu) {
                OpenQ(qCpu, { { L"\\Processor Information(*)\\% Processor Utility", &cUtil }, { L"\\Processor Information(*)\\% Processor Performance", &cPerf },
                              { L"\\Processor Information(*)\\Processor Frequency", &cFreq } });
                have = false;
            } else if (PdhCollectQueryData(qCpu) == ERROR_SUCCESS) {
                std::vector<BYTE> bu, bp, bf; PDH_FMT_COUNTERVALUE_ITEM_W *iu = nullptr, *ip = nullptr, *iff = nullptr; DWORD nu = 0, np = 0, nf = 0;
                bool okU = Arr(cUtil, bu, iu, nu), okP = Arr(cPerf, bp, ip, np), okF = Arr(cFreq, bf, iff, nf);
                int n = (int)cls.size();
                std::vector<double> use(n, -1), perf(n, -1), freq(n, -1);
                if (okU) for (DWORD i = 0; i < nu; i++) { int l = Lp(iu[i].szName); if (l >= 0 && l < n) use[l] = std::min(100.0, iu[i].FmtValue.doubleValue); }
                if (okP) for (DWORD i = 0; i < np; i++) { int l = Lp(ip[i].szName); if (l >= 0 && l < n) perf[l] = ip[i].FmtValue.doubleValue; }
                if (okF) for (DWORD i = 0; i < nf; i++) { int l = Lp(iff[i].szName); if (l >= 0 && l < n) freq[l] = iff[i].FmtValue.doubleValue; }
                double sPc = 0, sEc = 0, sPu = 0, sEu = 0, sAu = 0; int cP = 0, cE = 0, cPu = 0, cEu = 0, cAu = 0;
                for (int l = 0; l < n; l++) {
                    bool P = !hybrid || cls[l] == maxCls;
                    if (perf[l] >= 0 && freq[l] > 0) { double mhz = freq[l] * perf[l] / 100.0; if (P) { sPc += mhz; cP++; } else { sEc += mhz; cE++; } }
                    if (use[l] >= 0) { if (P) { sPu += use[l]; cPu++; } else { sEu += use[l]; cEu++; } sAu += use[l]; cAu++; }
                }
                pGHz = cP ? sPc / cP / 1000 : 0; eGHz = cE ? sEc / cE / 1000 : 0;
                pUse = cPu ? sPu / cPu : 0;      eUse = cEu ? sEu / cEu : 0; allUse = cAu ? sAu / cAu : 0;
                have = true;
            }
        } else { CloseQ(qCpu); have = false; }
        // CPU package / cores / iGPU power
        if (f & M_POW) {
            if (!qPow) {
                OpenQ(qPow, { { L"\\Energy Meter(RAPL_Package0_PKG)\\Power", &cPkg }, { L"\\Energy Meter(RAPL_Package0_PP0)\\Power", &cCores },
                              { L"\\Energy Meter(RAPL_Package0_PP1)\\Power", &cIgpu } });
                cpuW = coresW = igpuW = -1;
            } else if (PdhCollectQueryData(qPow) == ERROR_SUCCESS) { cpuW = One(cPkg, 0.001); coresW = One(cCores, 0.001); igpuW = One(cIgpu, 0.001); }
        } else { CloseQ(qPow); cpuW = coresW = igpuW = -1; }
        // ACPI thermal zone
        if (f & M_TZ) {
            if (!qTz) { OpenQ(qTz, { { L"\\Thermal Zone Information(*)\\High Precision Temperature", &cTz } }); }
            if (qTz && PdhCollectQueryData(qTz) == ERROR_SUCCESS) {
                std::vector<BYTE> bt; PDH_FMT_COUNTERVALUE_ITEM_W* it = nullptr; DWORD nt = 0;
                sysT = (Arr(cTz, bt, it, nt) && nt) ? it[0].FmtValue.doubleValue / 10.0 - 273.15 : -1000;
            }
        } else { CloseQ(qTz); sysT = -1000; }
        // NVIDIA GPU (never woken up)
        if (f & M_GPU) {
            gpuState = GpuPowerState();
            if (gpuState == 4) { NvDown(); gpuT = gpuClk = gpuMem = gpuUse = -1; gpuW = -1; }
            else if (NvUp()) {
                unsigned v = 0; NvUtil u = {};
                gpuT = (nTemp && nTemp(dev, 0, &v) == 0) ? (int)v : -1;
                gpuClk = (nClock && nClock(dev, 0, &v) == 0) ? (int)v : -1;
                gpuMem = (nClock && nClock(dev, 2, &v) == 0) ? (int)v : -1;
                gpuW = (nPower && nPower(dev, &v) == 0) ? v / 1000.0 : -1;
                gpuUse = (nUtil && nUtil(dev, &u) == 0) ? (int)u.gpu : -1;
            }
        } else { NvDown(); gpuT = gpuClk = gpuMem = gpuUse = -1; gpuW = -1; gpuState = 0; }
        // memory (one system call)
        if (f & M_RAM) {
            MEMORYSTATUSEX ms = { sizeof ms };
            if (GlobalMemoryStatusEx(&ms)) {
                const double GB = 1073741824.0;
                ramTotal = ms.ullTotalPhys / GB; ramGB = (ms.ullTotalPhys - ms.ullAvailPhys) / GB; ramPct = 100.0 * ramGB / ramTotal;
                commitGB = (ms.ullTotalPageFile - ms.ullAvailPageFile) / GB; commitPct = 100.0 * commitGB / (ms.ullTotalPageFile / GB);
            }
        } else ramPct = ramGB = ramTotal = commitPct = commitGB = -1;
        // disk activity (all disks in one query)
        if (f & M_DISK) {
            if (!qDisk) {
                OpenQ(qDisk, { { L"\\PhysicalDisk(*)\\% Idle Time", &cIdle }, { L"\\PhysicalDisk(*)\\Disk Read Bytes/sec", &cRd },
                               { L"\\PhysicalDisk(*)\\Disk Write Bytes/sec", &cWr } });
                for (int d = 0; d < kMaxDisk; d++) dAct[d] = dRd[d] = dWr[d] = -1;
            } else if (PdhCollectQueryData(qDisk) == ERROR_SUCCESS) {
                std::vector<BYTE> b1, b2, b3; PDH_FMT_COUNTERVALUE_ITEM_W *i1 = nullptr, *i2 = nullptr, *i3 = nullptr; DWORD n1 = 0, n2 = 0, n3 = 0;
                if (Arr(cIdle, b1, i1, n1)) for (DWORD i = 0; i < n1; i++) { int d = DiskSlot(i1[i].szName); if (d >= 0) dAct[d] = std::clamp(100.0 - i1[i].FmtValue.doubleValue, 0.0, 100.0); }
                if (Arr(cRd, b2, i2, n2)) for (DWORD i = 0; i < n2; i++) { int d = DiskSlot(i2[i].szName); if (d >= 0) dRd[d] = i2[i].FmtValue.doubleValue / 1048576.0; }
                if (Arr(cWr, b3, i3, n3)) for (DWORD i = 0; i < n3; i++) { int d = DiskSlot(i3[i].szName); if (d >= 0) dWr[d] = i3[i].FmtValue.doubleValue / 1048576.0; }
            }
        } else { CloseQ(qDisk); for (int d = 0; d < kMaxDisk; d++) dAct[d] = dRd[d] = dWr[d] = -1; }
        // disk temperatures: only the disks whose temperature is shown, at most every 5 s (it is a command to the drive)
        ULONGLONG now = GetTickCount64();
        for (int d = 0; d < kMaxDisk; d++) {
            bool want = (f & M_DISKT) && g_diskPresent[d] && ((need >> (H_DISK + 4 * d)) & 1);
            if (!want) { if (diskH[d]) { CloseHandle(diskH[d]); diskH[d] = nullptr; } dT[d] = -1000; continue; }
            if (dT[d] > -100 && now - diskTAt[d] < 5000) continue;
            if (!diskH[d]) {
                wchar_t p[64]; swprintf(p, 64, L"\\\\.\\PhysicalDrive%d", g_diskNum[d]);
                HANDLE h = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                diskH[d] = h == INVALID_HANDLE_VALUE ? nullptr : h;
            }
            diskTAt[d] = now;
            BYTE buf[512] = {}; DWORD got = 0;
            STORAGE_PROPERTY_QUERY q = { StorageDeviceTemperatureProperty, PropertyStandardQuery };
            if (diskH[d] && DeviceIoControl(diskH[d], IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, nullptr) &&
                ((STORAGE_TEMPERATURE_DATA_DESCRIPTOR*)buf)->InfoCount > 0)
                dT[d] = ((STORAGE_TEMPERATURE_DATA_DESCRIPTOR*)buf)->TemperatureInfo[0].Temperature;
            else dT[d] = -1000;
        }
    }
};

// ---- FPS of the app in front: counts DXGI / D3D9 Present calls per process through a real-time ETW session.
static const wchar_t* kFpsSession = L"LlanoFanSync FPS";
static const GUID kDxgiProv = { 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };   // Microsoft-Windows-DXGI
static const GUID kD3d9Prov = { 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };   // Microsoft-Windows-D3D9
struct FpsMeter {
    TRACEHANDLE session = 0, trace = INVALID_PROCESSTRACE_HANDLE;
    HANDLE thread = nullptr;
    bool on = false; ULONG err = 0; ULONGLONG lastTry = 0, lastRead = 0;
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<std::pair<DWORD, int>> counts;
    DWORD target = 0, shown = 0;
    double fps = -1;
    std::vector<BYTE> pb;
    EVENT_TRACE_PROPERTIES* Props() {
        pb.assign(sizeof(EVENT_TRACE_PROPERTIES) + 1024, 0);
        auto* p = (EVENT_TRACE_PROPERTIES*)pb.data();
        p->Wnode.BufferSize = (ULONG)pb.size(); p->Wnode.Flags = WNODE_FLAG_TRACED_GUID; p->Wnode.ClientContext = 1;
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE; p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        p->BufferSize = 16; p->MinimumBuffers = 2; p->MaximumBuffers = 8; p->FlushTimer = 1;
        return p;
    }
    void Start();
    void Stop() {
        if (!on) return;
        ControlTraceW(session, nullptr, Props(), EVENT_TRACE_CONTROL_STOP);
        CloseTrace(trace);
        if (thread) { WaitForSingleObject(thread, 3000); CloseHandle(thread); thread = nullptr; }
        on = false; session = 0; trace = INVALID_PROCESSTRACE_HANDLE; fps = -1; shown = 0;
        wcscpy(g_fpsName, L"FPS (game in front)");
    }
    static bool ExeName(DWORD pid, wchar_t* out, DWORD n) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return false;
        wchar_t path[MAX_PATH]; DWORD len = MAX_PATH; bool ok = QueryFullProcessImageNameW(h, 0, path, &len) != 0;
        CloseHandle(h);
        if (!ok) return false;
        const wchar_t* s = wcsrchr(path, L'\\'); wcsncpy(out, s ? s + 1 : path, n - 1); out[n - 1] = 0;
        return true;
    }
    void Read() {                     // about once a second: presents per second of the app in front
        if (!on) return;
        ULONGLONG now = GetTickCount64(); double dt = (now - lastRead) / 1000.0;
        if (dt < 0.5) return;
        lastRead = now;
        std::vector<std::pair<DWORD, int>> c;
        AcquireSRWLockExclusive(&lock); c.swap(counts); ReleaseSRWLockExclusive(&lock);
        DWORD fg = 0; HWND fw = GetForegroundWindow(); if (fw) GetWindowThreadProcessId(fw, &fg);
        if (fg && fg != GetCurrentProcessId()) target = fg;           // our own window in front: keep the last app
        int n = 0; DWORD pick = 0;
        for (auto& p : c) if (p.first == target) { n = p.second; pick = target; }
        if (!pick)                                                     // the app in front is not rendering: busiest renderer
            for (auto& p : c) {
                wchar_t nm[MAX_PATH];
                if (p.first == GetCurrentProcessId() || p.second <= n) continue;
                if (ExeName(p.first, nm, MAX_PATH) && !_wcsicmp(nm, L"dwm.exe")) continue;
                n = p.second; pick = p.first;
            }
        fps = pick ? n / dt : -1;
        if (pick != shown) {
            shown = pick;
            wchar_t nm[MAX_PATH] = L"";
            if (pick && ExeName(pick, nm, MAX_PATH)) { wchar_t* dot = wcsrchr(nm, L'.'); if (dot) *dot = 0; swprintf(g_fpsName, 96, L"FPS · %s", nm); }
            else wcscpy(g_fpsName, L"FPS (game in front)");
        }
    }
};
static FpsMeter g_fps;
static void WINAPI FpsEvent(PEVENT_RECORD e) {
    const EVENT_HEADER& h = e->EventHeader; USHORT id = h.EventDescriptor.Id;
    bool present = (IsEqualGUID(h.ProviderId, kDxgiProv) && (id == 42 || id == 55)) || (IsEqualGUID(h.ProviderId, kD3d9Prov) && id == 1);
    if (!present) return;
    AcquireSRWLockExclusive(&g_fps.lock);
    bool found = false;
    for (auto& c : g_fps.counts) if (c.first == h.ProcessId) { c.second++; found = true; break; }
    if (!found && g_fps.counts.size() < 512) g_fps.counts.push_back({ h.ProcessId, 1 });
    ReleaseSRWLockExclusive(&g_fps.lock);
}
static DWORD WINAPI FpsThread(LPVOID) { ProcessTrace(&g_fps.trace, 1, nullptr, nullptr); return 0; }
void FpsMeter::Start() {
    if (on) return;
    ULONGLONG now = GetTickCount64();
    if (err && now - lastTry < 30000) return;                          // failed recently: retry every 30 s
    lastTry = now;
    ULONG r = StartTraceW(&session, kFpsSession, Props());
    if (r == ERROR_ALREADY_EXISTS) { ControlTraceW(0, kFpsSession, Props(), EVENT_TRACE_CONTROL_STOP); r = StartTraceW(&session, kFpsSession, Props()); }
    if (r != ERROR_SUCCESS) { if (err != r) Log(L"FPS: cannot start the ETW session (error %lu)", r); err = r; session = 0; return; }
    // only the Present events are delivered (filtered inside Windows before they are buffered)
    auto enable = [&](const GUID& g, std::initializer_list<USHORT> ids) {
        BYTE buf[sizeof(EVENT_FILTER_EVENT_ID) + 8 * sizeof(USHORT)] = {};
        auto* fi = (EVENT_FILTER_EVENT_ID*)buf; fi->FilterIn = TRUE; fi->Count = (USHORT)ids.size();
        int k = 0; for (USHORT i : ids) fi->Events[k++] = i;
        EVENT_FILTER_DESCRIPTOR d = { (ULONGLONG)fi, (ULONG)(FIELD_OFFSET(EVENT_FILTER_EVENT_ID, Events) + ids.size() * sizeof(USHORT)), EVENT_FILTER_TYPE_EVENT_ID };
        ENABLE_TRACE_PARAMETERS p = {}; p.Version = ENABLE_TRACE_PARAMETERS_VERSION_2; p.EnableFilterDesc = &d; p.FilterDescCount = 1;
        if (EnableTraceEx2(session, &g, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, ~0ULL, 0, 0, &p) != ERROR_SUCCESS)
            EnableTraceEx2(session, &g, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, ~0ULL, 0, 0, nullptr);
    };
    enable(kDxgiProv, { 42, 55 });
    enable(kD3d9Prov, { 1 });
    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName = (LPWSTR)kFpsSession;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = FpsEvent;
    trace = OpenTraceW(&lf);
    if (trace == INVALID_PROCESSTRACE_HANDLE) {
        err = GetLastError(); Log(L"FPS: cannot open the ETW session (error %lu)", err);
        ControlTraceW(session, nullptr, Props(), EVENT_TRACE_CONTROL_STOP); session = 0; return;
    }
    AcquireSRWLockExclusive(&lock); counts.clear(); ReleaseSRWLockExclusive(&lock);
    thread = CreateThread(nullptr, 0, FpsThread, nullptr, 0, nullptr);
    on = true; err = 0; lastRead = now;
}

// ======================================================================= app state
static Settings g_s;
static Atk g_atk;
static Pad g_pad;
static Monitor g_mon;

struct Live {
    bool fansOk = false, padOk = false, padOff = false, padWheel = false;   // padWheel: the pad's own wheel sets the speed
    int cpu = 0, gpu = 0, sys = 0, cpuT = -1;
    double cpuP = 0, gpuP = 0, sysP = 0, cpuV = 0, gpuV = 0, sysV = 0;
    int target = -1, pad = -1, driver = -1;     // pad = the speed the pad itself reports; driver: 0 gpu, 1 cpu, 2 system
} g_live;

static bool g_paused = false, g_visible = false;
static bool g_wheelHold = false;   // the wheel was turned while we were in control: leave the pad alone
static bool g_takeBack = true;     // take control (again): at start, after a reconnect, on un-pause or "Take back control"
static int g_lastSent = -1, g_tick = 0, g_lastErr = 0;
static ULONGLONG g_lastSend = 0, g_lastPadTry = 0;
static FILETIME g_iniT;
static void SetPaused(bool on) {
    g_paused = on;
    if (!on) { g_takeBack = true; g_wheelHold = false; }
    WriteInt(L"paused", on ? 1 : 0); g_iniT = IniTime();
}

static HWND g_wnd;
static HINSTANCE g_inst;
static UINT WM_TASKBARCREATED;
static const UINT WM_TRAY = WM_APP + 1, WM_APP_SHOW = WM_APP + 2;
static const wchar_t* kClass = L"LlanoFanSyncWnd";
static NOTIFYICONDATAW g_nid;
static HICON g_icon = nullptr;
static int g_iconVal = -2; static int g_iconState = -1;
static ULONG_PTR g_gdip = 0;
static int g_dpi = 96;
static HFONT g_font = nullptr, g_fontBold = nullptr, g_fontBig = nullptr, g_fontSmall = nullptr;
static int g_dragG = -1, g_dragI = -1, g_hovG = -1, g_hovI = -1;

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static float Sf(float v) { return v * g_dpi / 96.f; }

static void SetEco(bool on) {
    PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
                                          on ? (ULONG)PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0u };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
    SetPriorityClass(GetCurrentProcess(), on ? IDLE_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
}

static DWORD LevelRgb(int pct) {
    if (pct <= 0) return 0x9CA3AF;
    if (pct < 40) return 0x2F80ED;
    if (pct < 75) return 0xF2994A;
    return 0xEB5757;
}

// ======================================================================= tray
static void UpdateTray(bool force = false) {
    int state = g_paused ? 1 : (!g_live.fansOk || !g_live.padOk) ? 2 : g_live.padOff ? 3 : 0;
    int val = g_live.pad;
    bool iconChanged = force || val != g_iconVal || state != g_iconState;
    if (iconChanged) {
        int sz = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
        HDC sdc = GetDC(nullptr), dc = CreateCompatibleDC(sdc);
        HBITMAP col = CreateCompatibleBitmap(sdc, sz, sz), mask = CreateBitmap(sz, sz, 1, 1, nullptr);
        HGDIOBJ old = SelectObject(dc, mask);
        PatBlt(dc, 0, 0, sz, sz, WHITENESS);
        SelectObject(dc, GetStockObject(BLACK_BRUSH)); SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, 0, 0, sz + 1, sz + 1, sz / 2, sz / 2);
        SelectObject(dc, col);
        PatBlt(dc, 0, 0, sz, sz, BLACKNESS);
        DWORD rgb = state ? 0x6B7280 : LevelRgb(val);
        HBRUSH br = CreateSolidBrush(RGB(rgb >> 16, (rgb >> 8) & 255, rgb & 255));
        SelectObject(dc, br); RoundRect(dc, 0, 0, sz + 1, sz + 1, sz / 2, sz / 2);
        wchar_t t[8];
        if (state == 1) wcscpy(t, L"II"); else if (state == 2) wcscpy(t, L"–"); else if (state == 3) wcscpy(t, L"off"); else swprintf(t, 8, L"%d", std::max(0, val));
        int len = (int)wcslen(t);
        HFONT f = CreateFontW(-(len >= 3 ? sz * 50 / 100 : sz * 68 / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT r = { 0, 0, sz, sz }; DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(dc, old);
        ICONINFO ii = { TRUE, 0, 0, mask, col };
        HICON ic = CreateIconIndirect(&ii);
        DeleteObject(f); DeleteObject(br); DeleteObject(col); DeleteObject(mask); DeleteDC(dc); ReleaseDC(nullptr, sdc);
        if (g_icon) DestroyIcon(g_icon);
        g_icon = ic; g_iconVal = val; g_iconState = state;
        g_nid.hIcon = g_icon;
    }
    wchar_t tip[128];
    if (state == 1) swprintf(tip, 128, L"Llano Fan Sync — paused");
    else if (!g_live.fansOk) swprintf(tip, 128, L"Llano Fan Sync — cannot read laptop fans");
    else if (!g_live.padOk) swprintf(tip, 128, L"Llano Fan Sync — pad not connected");
    else swprintf(tip, 128, L"Llano pad %d%%%s\nGPU %d · CPU %d · System %d rpm", g_live.pad, g_wheelHold ? L" (set with its wheel)" : L"",
                  g_live.gpu, g_live.cpu, g_live.sys);
    static ULONGLONG lastTip = 0;
    ULONGLONG now = GetTickCount64();
    if (!iconChanged && (wcscmp(tip, g_nid.szTip) == 0 || now - lastTip < 3000)) return;
    lastTip = now;
    wcsncpy(g_nid.szTip, tip, 127);
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void AddTray() {
    g_nid = {}; g_nid.cbSize = sizeof g_nid; g_nid.hWnd = g_wnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP; g_nid.uCallbackMessage = WM_TRAY;
    wcscpy(g_nid.szTip, L"Llano Fan Sync");
    g_iconVal = -2; UpdateTray(true);
    g_nid.hIcon = g_icon;
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

// ======================================================================= sync tick
// The pad is read every tick (paused or not) so the real speed is always shown.
// State byte 0: bit 0x08 set = the pad's own wheel sets the speed (0x88); software control reads back as 0x80.
static void SyncPad(int target) {
    ULONGLONG now = GetTickCount64();
    if (g_lastErr == 2 && now - g_lastPadTry < 5000) return;                  // pad missing: retry every 5 s
    g_lastPadTry = now;
    BYTE st[8];
    if (!g_pad.Get(st)) {
        if (g_lastErr != 2) Log(L"waiting: llano pad not connected");
        g_lastErr = 2; g_live.padOk = false; g_lastSent = -1; g_takeBack = true; return;
    }
    if (g_lastErr == 2) Log(L"pad connected");
    g_lastErr = 0; g_live.padOk = true; g_live.padOff = st[2] == 1; g_live.pad = st[1];
    g_live.padWheel = (st[0] & 0x08) != 0;
    if (g_live.padWheel && !g_takeBack && !g_wheelHold) { g_wheelHold = true; Log(L"pad wheel turned (%d%%): manual until control is taken back", st[1]); }
    if (g_paused || g_live.padOff) return;
    if (g_wheelHold && !g_takeBack) return;                                   // the wheel wins
    if (!g_live.padWheel && st[1] == target) { g_takeBack = false; g_wheelHold = false; return; }
    if (!g_pad.SetSpeed(st, target)) { g_live.padOk = false; return; }
    g_live.pad = target; g_live.padWheel = false;
    if (g_s.logChanges || g_takeBack)
        Log(L"GPU %d rpm (%.0f%%)  CPU %d rpm (%.0f%%)  SYS %d rpm (%.0f%%)  ->  pad %d%%%s", g_live.gpu, g_live.gpuP,
            g_live.cpu, g_live.cpuP, g_live.sys, g_live.sysP, target, g_takeBack ? L"  (taking control)" : L"");
    g_takeBack = false; g_wheelHold = false;
    g_lastSent = target; g_lastSend = now;
}

// ======================================================================= history (one sample per second at most, last hour)
struct HSample { ULONGLONG t; float v[H_COUNT]; };
static std::vector<HSample> g_hist;
static size_t g_histHead = 0;
static const size_t kHistCap = 3600;
static const HSample& HistAt(size_t i) { return g_hist[(g_histHead + i) % g_hist.size()]; }   // 0 = oldest

static void Record() {
    ULONGLONG now = GetTickCount64();
    if (!g_hist.empty() && now - HistAt(g_hist.size() - 1).t < 950) return;
    HSample s; s.t = now;
    for (auto& v : s.v) v = NAN;
    auto& L = g_live; auto& M = g_mon;
    if (L.fansOk) {
        s.v[H_GPUF] = (float)L.gpuP; s.v[H_CPUF] = (float)L.cpuP; s.v[H_SYSF] = (float)L.sysP;
        s.v[H_GPURPM] = (float)L.gpu; s.v[H_CPURPM] = (float)L.cpu; s.v[H_SYSRPM] = (float)L.sys;
    }
    if (L.padOk) {
        int p = L.padOff ? 0 : std::max(0, L.pad);
        s.v[H_PAD] = (float)p; s.v[H_PADRPM] = (float)PadRpm(p);
        if (p > 0) s.v[H_PADDB] = (float)PadNoise(p);
    }
    // only what was actually read this tick (everything else stays empty)
    if (L.cpuT >= 0) s.v[H_CPUT] = (float)L.cpuT;
    if (M.sysT > -100) s.v[H_SYST] = (float)M.sysT;
    if (M.cpuW >= 0) s.v[H_CPUW] = (float)M.cpuW;
    if (M.coresW >= 0) s.v[H_COREW] = (float)M.coresW;
    if (M.igpuW >= 0) s.v[H_IGPUW] = (float)M.igpuW;
    if (M.have) {
        s.v[H_PCLK] = (float)(M.pGHz * 1000); s.v[H_PUSE] = (float)M.pUse; s.v[H_ALLUSE] = (float)M.allUse;
        if (M.hybrid) { s.v[H_ECLK] = (float)(M.eGHz * 1000); s.v[H_EUSE] = (float)M.eUse; }
    }
    if (M.gpuState == 4) { s.v[H_GPUW] = 0; s.v[H_GPUUSE] = 0; }        // asleep
    else {
        if (M.gpuT >= 0) s.v[H_GPUT] = (float)M.gpuT;
        if (M.gpuW >= 0) s.v[H_GPUW] = (float)M.gpuW;
        if (M.gpuClk >= 0) s.v[H_GPUCLK] = (float)M.gpuClk;
        if (M.gpuMem >= 0) s.v[H_GPUMEM] = (float)M.gpuMem;
        if (M.gpuUse >= 0) s.v[H_GPUUSE] = (float)M.gpuUse;
    }
    if (M.ramPct >= 0) {
        s.v[H_RAMPCT] = (float)M.ramPct; s.v[H_RAMGB] = (float)M.ramGB;
        s.v[H_COMMITPCT] = (float)M.commitPct; s.v[H_COMMITGB] = (float)M.commitGB;
    }
    if (g_fps.on && g_fps.fps >= 0) s.v[H_FPS] = (float)g_fps.fps;
    for (int d = 0; d < kMaxDisk; d++) {
        if (M.dT[d] > -100) s.v[H_DISK + 4 * d] = (float)M.dT[d];
        if (M.dAct[d] >= 0) s.v[H_DISK + 4 * d + 1] = (float)M.dAct[d];
        if (M.dRd[d] >= 0) s.v[H_DISK + 4 * d + 2] = (float)M.dRd[d];
        if (M.dWr[d] >= 0) s.v[H_DISK + 4 * d + 3] = (float)M.dWr[d];
    }
    if (g_hist.size() < kHistCap) g_hist.push_back(s);
    else { g_hist[g_histHead] = s; g_histHead = (g_histHead + 1) % kHistCap; }
}

static void InvalidateAll();
static void OvRender(bool force = false);
static void OvApply();
static int MonFlags(unsigned long long& need);
static void Tick() {
    g_tick++;
    if (g_tick % 5 == 0) {
        FILETIME t = IniTime();
        if (CompareFileTime(&t, &g_iniT)) { LoadSettings(g_s); g_iniT = t; g_lastSent = -1; InvalidateAll(); OvApply(); }
    }
    int cpu, gpu, sys;
    if (!g_atk.Rpm(0x00110013, cpu) || !g_atk.Rpm(0x00110014, gpu) || !g_atk.Rpm(0x00110031, sys)) {
        if (g_lastErr != 1) Log(L"waiting: cannot read laptop fans (ATKACPI)");
        g_lastErr = 1; g_live.fansOk = false;
    } else {
        if (g_lastErr == 1) g_lastErr = 0;
        g_live.fansOk = true;
        auto& L = g_live;
        L.cpu = cpu; L.gpu = gpu; L.sys = sys;
        L.cpuP = 100.0 * cpu / kCpuMaxRpm; L.gpuP = 100.0 * gpu / kGpuMaxRpm; L.sysP = 100.0 * sys / kSysMaxRpm;
        L.gpuV = g_s.gpu.Eval(L.gpuP); L.cpuV = g_s.cpu.Eval(L.cpuP); L.sysV = g_s.sys.Eval(L.sysP);
        double v = std::max({ L.gpuV, L.cpuV, L.sysV });
        L.driver = v <= 0 ? -1 : (v == L.gpuV ? 0 : v == L.cpuV ? 1 : 2);
        L.target = std::clamp((int)std::lround(v), 0, 100);
        SyncPad(L.target);
    }
    unsigned long long need = 0;
    int f = MonFlags(need);                       // which sensor groups anything shows right now
    DWORD t; g_live.cpuT = (f & M_CPUT) && g_atk.Get(0x00120094, t) ? (int)(t & 0xFFFF) : -1;
    g_mon.Sample(f, need);
    if (f & M_FPS) { g_fps.Start(); g_fps.Read(); } else g_fps.Stop();
    Record();
    InvalidateAll();
    OvRender();
    UpdateTray();
}

// ======================================================================= UI: custom-drawn, themed (light / dark), scrollable windows
// No child controls: every widget is painted here and registers a clickable region ("hot") while painting.
enum WinKind { WK_MAIN, WK_ZGPU, WK_ZCPU, WK_ZSYS, WK_ZHIST, WK_OVSET, WK_COUNT };
enum Act { A_NONE, A_TOGGLE, A_BUTTON, A_SEG, A_CHIP, A_OVCHIP, A_SLIDER, A_EXPAND, A_SWATCH, A_ANCHOR };
enum { T_OVERLAY, T_OVLOCK, T_STARTUP, T_PAUSE, T_RECORD, T_OVBG, T_OVOUTLINE };
enum { B_OVSET, B_RESET, B_HIDE, B_TXTCUSTOM, B_BGCUSTOM, B_CLOSE, B_CHARTNONE, B_OVNONE, B_TAKEBACK, B_PICK };
enum { G_RANGE, G_INTERVAL, G_THEME, G_OVMODE, G_OVRANGE, G_OVLAYOUT, G_OVSIZE };
enum { SL_OVOPAC, SL_OVBGOPAC, SL_OVW, SL_OVH };
enum { MENU_OPEN = 1, MENU_PAUSE, MENU_TAKEBACK, MENU_OVERLAY, MENU_OVLOCK, MENU_OVSET, MENU_EXIT };
static const UINT WM_APP_OVMENU = WM_APP + 3, WM_APP_RELAYOUT = WM_APP + 4;

struct Hot { RECT r; int act, arg; };
struct Win {
    HWND h = nullptr; int kind = 0, dpi = 96;
    int minW = 400, minH = 300, cw = 400, ch = 300, sx = 0, sy = 0;   // content size in 96-dpi units, scroll in px
    std::vector<Hot> hots;
    int dragG = -1, dragI = -1, hovG = -1, hovI = -1;                 // curve editing
    int hotAct = 0, hotArg = -1;                                       // widget under the mouse
    int capAct = 0, capArg = -1; RECT capR = {};                       // slider being dragged
    int crossX = -1; RECT histPlot = {};                               // chart crosshair
    bool pick = false;                                                 // chart sensor list expanded (all sensors) or just the ticked ones
};
static Win g_w[WK_COUNT];
static HWND g_ov = nullptr;                                            // overlay window
static bool g_startup = false;

static const int kIntervals[] = { 250, 500, 1000, 2000, 3000, 5000 };
static const wchar_t* kIntervalName[] = { L"0.25 s", L"0.5 s", L"1 s", L"2 s", L"3 s", L"5 s" };
static const wchar_t* kRangeShort[] = { L"1 min", L"5 min", L"15 min", L"30 min", L"1 hour" };
static const int kRangeDiv[] = { 4, 5, 3, 3, 4 };
static const wchar_t* kThemeName[] = { L"Follow Windows", L"Light", L"Dark" };
static const wchar_t* kOvModeName[] = { L"Numbers", L"Chart", L"Numbers + chart" };
static const wchar_t* kOvLayoutName[] = { L"Vertical list", L"Horizontal line" };
static const wchar_t* kOvSizeName[] = { L"Small", L"Medium", L"Large", L"X-Large", L"Huge" };
static const float kOvScale[] = { 0.85f, 1.0f, 1.25f, 1.6f, 2.0f };
static const wchar_t* kAnchorName[] = { L"Free — where you dragged it", L"Top left", L"Top centre", L"Top right", L"Middle left", L"Centre",
                                        L"Middle right", L"Bottom left", L"Bottom centre", L"Bottom right" };
static const DWORD kTxtSw[8] = { 0xF9FAFB, 0x111827, 0xFACC15, 0x4ADE80, 0x38BDF8, 0xFB923C, 0xF87171, 0xC4B5FD };
static const DWORD kBgSw[8] = { 0x111827, 0x000000, 0x1F2937, 0x0F172A, 0x1E1B4B, 0x052E16, 0x450A0A, 0xFFFFFF };

// ---- palette
struct Pal { DWORD bg, card, ink, sub, line, grid, axis, btn, btnHot, accent, tipBg, tipInk; bool dark; };
static const Pal kLight = { 0xFFFFFF, 0xF7F8FA, 0x1F2328, 0x6B7280, 0xE5E7EB, 0xEEF0F3, 0xC9CDD3, 0xF1F3F5, 0xE3E6EA, 0x2F80ED, 0x1F2328, 0xFFFFFF, false };
static const Pal kDark  = { 0x15171C, 0x1D2026, 0xE6E8EB, 0x9AA1AC, 0x2D323A, 0x23272E, 0x3A404A, 0x252A31, 0x323843, 0x4C8DF6, 0x30353E, 0xF3F4F6, true };
static const Pal* P = &kLight;
static const DWORD kNowCol = 0xF2994A;
static bool SystemDark() {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}
static void ThemeWindow(HWND h) {
    BOOL d = P->dark; DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof d);
    SetWindowTheme(h, P->dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);     // scroll bars
}
static void ApplyTheme() {
    P = (g_s.theme == 2 || (g_s.theme == 0 && SystemDark())) ? &kDark : &kLight;
    for (auto& w : g_w) if (w.h) {
        ThemeWindow(w.h);
        SetWindowPos(w.h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(w.h, nullptr, FALSE);
    }
}

// ---- drawing helpers
static Color Rgb(DWORD rgb, BYTE a = 255) { return Color(a, (BYTE)(rgb >> 16), (BYTE)(rgb >> 8), (BYTE)rgb); }
static void RoundPath(GraphicsPath& p, RectF r, REAL rad) {
    REAL d = std::min(rad * 2, std::min(r.Width, r.Height));
    if (d <= 0) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90); p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90); p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
struct Fonts {
    FontFamily ff; Font n, b, s, sb, h, big;
    explicit Fonts(float k) : ff(L"Segoe UI"), n(&ff, 12 * k, FontStyleRegular, UnitPixel), b(&ff, 12 * k, FontStyleBold, UnitPixel),
        s(&ff, 11 * k, FontStyleRegular, UnitPixel), sb(&ff, 11 * k, FontStyleBold, UnitPixel),
        h(&ff, 16 * k, FontStyleBold, UnitPixel), big(&ff, 24 * k, FontStyleBold, UnitPixel) {}
};
static void Txt(Graphics& g, const wchar_t* s, Font& f, DWORD rgb, REAL x, REAL y, REAL w = 0, StringAlignment al = StringAlignmentNear) {
    SolidBrush b(Rgb(rgb));
    if (w <= 0) { g.DrawString(s, -1, &f, PointF(x, y), &b); return; }
    StringFormat sf; sf.SetAlignment(al); sf.SetFormatFlags(StringFormatFlagsNoWrap); sf.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(s, -1, &f, RectF(x, y, w, f.GetHeight(&g) * 1.3f), &sf, &b);
}
static REAL TextW(Graphics& g, const wchar_t* s, Font& f) { RectF b; g.MeasureString(s, -1, &f, PointF(0, 0), &b); return b.Width; }
static RECT RcF(REAL x, REAL y, REAL w, REAL h) { return { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) }; }
static void AddHot(Win& w, RECT r, int act, int arg) { w.hots.push_back({ r, act, arg }); }
static bool IsHot(Win& w, int act, int arg) { return w.hotAct == act && w.hotArg == arg; }

static void FmtVal(wchar_t* b, float v, int unit) {
    if (std::isnan(v)) { wcscpy(b, L"—"); return; }
    bool dec = unit == U_W || unit == U_GB || (unit == U_MBS && v < 10);
    swprintf(b, 32, dec ? L"%.1f%s" : L"%.0f%s", v, kUnitSuffix[unit]);
}
static const HSample* LastSample() {
    if (g_hist.empty()) return nullptr;
    const HSample* s = &HistAt(g_hist.size() - 1);
    return GetTickCount64() - s->t > 10000 ? nullptr : s;
}
static void Nice(double a, double b, int n, double& lo, double& hi, double& step) {
    if (b <= a) b = a + 1;
    double raw = (b - a) / std::max(1, n), mag = pow(10, floor(log10(raw))), f = raw / mag;
    step = (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * mag;
    lo = floor(a / step) * step; hi = ceil(b / step) * step;
    if (hi <= lo) hi = lo + step;
}

// ---- widgets (all return / register their clickable area)
static REAL Toggle(Graphics& g, Win& w, Fonts& F, REAL x, REAL y, const wchar_t* label, bool on, int id, bool enabled = true) {
    REAL bs = Sf(16), wd = bs + Sf(8) + TextW(g, label, F.n);
    bool hot = enabled && IsHot(w, A_TOGGLE, id);
    GraphicsPath p; RoundPath(p, RectF(x, y + Sf(3), bs, bs), Sf(4));
    if (on) {
        SolidBrush b(Rgb(P->accent, enabled ? 255 : 90)); g.FillPath(&b, &p);
        Pen ck(Rgb(0xFFFFFF), Sf(2)); ck.SetLineJoin(LineJoinRound); ck.SetStartCap(LineCapRound); ck.SetEndCap(LineCapRound);
        PointF pts[3] = { PointF(x + Sf(4), y + Sf(11)), PointF(x + Sf(7), y + Sf(14)), PointF(x + Sf(12.5f), y + Sf(7.5f)) };
        g.DrawLines(&ck, pts, 3);
    } else {
        SolidBrush b(Rgb(P->btn)); g.FillPath(&b, &p);
        Pen bd(Rgb(hot ? P->accent : P->axis), Sf(1.3f)); g.DrawPath(&bd, &p);
    }
    Txt(g, label, F.n, enabled ? P->ink : P->sub, x + bs + Sf(8), y + Sf(2));
    if (enabled) AddHot(w, RcF(x, y, wd, Sf(22)), A_TOGGLE, id);
    return wd;
}
static void Button(Graphics& g, Win& w, Fonts& F, RectF r, const wchar_t* label, int id, bool primary = false) {
    bool hot = IsHot(w, A_BUTTON, id);
    GraphicsPath p; RoundPath(p, r, Sf(6));
    SolidBrush b(Rgb(primary ? P->accent : hot ? P->btnHot : P->btn)); g.FillPath(&b, &p);
    if (!primary) { Pen bd(Rgb(hot ? P->accent : P->line), Sf(1)); g.DrawPath(&bd, &p); }
    Txt(g, label, F.n, primary ? 0xFFFFFF : P->ink, r.X, r.Y + (r.Height - Sf(17)) / 2, r.Width, StringAlignmentCenter);
    AddHot(w, RcF(r.X, r.Y, r.Width, r.Height), A_BUTTON, id);
}
static REAL SegWidth(Graphics& g, Fonts& F, const wchar_t* const* labels, int n) {
    REAL t = 0; for (int i = 0; i < n; i++) t += TextW(g, labels[i], F.n) + Sf(14); return t;
}
static REAL Seg(Graphics& g, Win& w, Fonts& F, REAL x, REAL y, const wchar_t* const* labels, int n, int sel, int group) {
    REAL h = Sf(26), tot = SegWidth(g, F, labels, n);
    GraphicsPath p; RoundPath(p, RectF(x, y, tot, h), Sf(6));
    SolidBrush bg(Rgb(P->btn)); g.FillPath(&bg, &p);
    Pen bd(Rgb(P->line), Sf(1)); g.DrawPath(&bd, &p);
    REAL cx = x;
    for (int i = 0; i < n; i++) {
        REAL wi = TextW(g, labels[i], F.n) + Sf(14);
        bool hot = IsHot(w, A_SEG, group * 100 + i);
        if (i == sel || hot) {
            GraphicsPath sp; RoundPath(sp, RectF(cx + Sf(2), y + Sf(2), wi - Sf(4), h - Sf(4)), Sf(5));
            SolidBrush sb(Rgb(i == sel ? P->accent : P->btnHot)); g.FillPath(&sb, &sp);
        }
        Txt(g, labels[i], F.n, i == sel ? 0xFFFFFF : P->ink, cx, y + (h - Sf(17)) / 2, wi, StringAlignmentCenter);
        AddHot(w, RcF(cx, y, wi, h), A_SEG, group * 100 + i);
        cx += wi;
    }
    return tot;
}
static void Slider(Graphics& g, Win& w, Fonts& F, RectF r, int val, int lo, int hi, int id, const wchar_t* valText) {
    REAL cy = r.Y + r.Height / 2, x0 = r.X + Sf(8), x1 = r.GetRight() - Sf(8);
    REAL kx = x0 + (x1 - x0) * (REAL)(val - lo) / (REAL)(hi - lo);
    Pen tr(Rgb(P->axis), Sf(4)); tr.SetStartCap(LineCapRound); tr.SetEndCap(LineCapRound); g.DrawLine(&tr, x0, cy, x1, cy);
    Pen ac(Rgb(P->accent), Sf(4)); ac.SetStartCap(LineCapRound); ac.SetEndCap(LineCapRound); g.DrawLine(&ac, x0, cy, kx, cy);
    bool hot = IsHot(w, A_SLIDER, id) || (w.capAct == A_SLIDER && w.capArg == id);
    REAL kr = Sf(hot ? 8.f : 7.f);
    SolidBrush kb(Rgb(0xFFFFFF)); Pen kp(Rgb(P->accent), Sf(2));
    g.FillEllipse(&kb, kx - kr, cy - kr, 2 * kr, 2 * kr); g.DrawEllipse(&kp, kx - kr, cy - kr, 2 * kr, 2 * kr);
    AddHot(w, RcF(r.X, r.Y, r.Width, r.Height), A_SLIDER, id);
    if (valText) Txt(g, valText, F.n, P->ink, r.GetRight() + Sf(10), cy - Sf(9));
}
static int SliderValue(RECT r, int x, int lo, int hi) {
    int x0 = r.left + S(8), x1 = r.right - S(8);
    return lo + (int)std::lround((double)std::clamp(x - x0, 0, x1 - x0) / std::max(1, x1 - x0) * (hi - lo));
}
static int* SliderRef(int id, int& lo, int& hi) {
    switch (id) {
    case SL_OVOPAC: lo = 10; hi = 100; return &g_s.ovOpacity;
    case SL_OVBGOPAC: lo = 0; hi = 100; return &g_s.ovBgOpacity;
    case SL_OVW: lo = 160; hi = 1600; return &g_s.ovW;
    default: lo = 60; hi = 1000; return &g_s.ovH;
    }
}
static void ExpandBtn(Graphics& g, Win& w, RectF r, int what) {
    bool hot = IsHot(w, A_EXPAND, what);
    if (hot) { GraphicsPath p; RoundPath(p, r, Sf(5)); SolidBrush b(Rgb(P->btnHot)); g.FillPath(&b, &p); }
    Pen p(Rgb(hot ? P->accent : P->sub), Sf(1.7f)); p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
    REAL m = Sf(6), x0 = r.X + m, y0 = r.Y + m, x1 = r.GetRight() - m, y1 = r.GetBottom() - m, c = Sf(4.5f);
    g.DrawLine(&p, x0, y0 + c, x0, y0); g.DrawLine(&p, x0, y0, x0 + c, y0);
    g.DrawLine(&p, x1 - c, y0, x1, y0); g.DrawLine(&p, x1, y0, x1, y0 + c);
    g.DrawLine(&p, x0, y1 - c, x0, y1); g.DrawLine(&p, x0, y1, x0 + c, y1);
    g.DrawLine(&p, x1 - c, y1, x1, y1); g.DrawLine(&p, x1, y1, x1, y1 - c);
    AddHot(w, RcF(r.X, r.Y, r.Width, r.Height), A_EXPAND, what);
}
static REAL Swatches(Graphics& g, Win& w, REAL x, REAL y, const DWORD* cols, int n, DWORD cur, int target) {
    REAL s = Sf(22), gap = Sf(7);
    for (int i = 0; i < n; i++) {
        RectF r(x + i * (s + gap), y, s, s);
        if (cols[i] == cur) { GraphicsPath rp; RoundPath(rp, RectF(r.X - Sf(3), r.Y - Sf(3), s + Sf(6), s + Sf(6)), Sf(7)); Pen ring(Rgb(P->accent), Sf(2)); g.DrawPath(&ring, &rp); }
        GraphicsPath p; RoundPath(p, r, Sf(5));
        SolidBrush b(Rgb(cols[i])); g.FillPath(&b, &p);
        Pen bd(Rgb(P->axis), Sf(1)); g.DrawPath(&bd, &p);
        AddHot(w, RcF(r.X, r.Y, s, s), A_SWATCH, target * 100 + i);
    }
    return n * (s + gap);
}
// Sensor chips: click to show / hide. Returns the bottom edge.
static REAL Chips(Graphics& g, Win& w, Fonts& F, RectF area, unsigned long long mask, int act, bool all = true) {
    const HSample* last = LastSample();
    REAL x = area.X, y = area.Y, h = Sf(24), gap = Sf(6);
    int prevGroup = -1;
    for (int i = 0; i < H_COUNT; i++) {
        if (!SeriesShown(i) || (!all && !((mask >> i) & 1))) continue;
        const SeriesDef& s = kSeries[i];
        bool on = (mask >> i) & 1, hot = IsHot(w, act, i);
        bool hasV = last && !std::isnan(last->v[i]);          // sensors nobody shows are not read: no value
        wchar_t val[32] = L""; if (hasV) FmtVal(val, last->v[i], s.unit);
        REAL tn = TextW(g, s.name, F.s), tv = hasV ? TextW(g, val, F.sb) : 0, cw = Sf(hasV ? 24 : 26) + tn + tv + Sf(2);
        if (prevGroup >= 0 && s.group != prevGroup) x += Sf(12);       // small gap between groups
        prevGroup = s.group;
        if (x > area.X && x + cw > area.GetRight()) { x = area.X; y += h + gap; }
        RectF r(x, y, cw, h);
        GraphicsPath p; RoundPath(p, r, h / 2);
        if (on) { SolidBrush b(Rgb(s.col, P->dark ? 70 : 40)); g.FillPath(&b, &p); Pen bd(Rgb(s.col), Sf(1.3f)); g.DrawPath(&bd, &p); }
        else { SolidBrush b(Rgb(hot ? P->btnHot : P->btn)); g.FillPath(&b, &p); Pen bd(Rgb(hot ? s.col : P->line), Sf(1)); g.DrawPath(&bd, &p); }
        REAL dr = Sf(4.5f), dx = x + Sf(11), dy = y + h / 2;
        if (on) { SolidBrush d(Rgb(s.col)); g.FillEllipse(&d, dx - dr, dy - dr, 2 * dr, 2 * dr); }
        else { Pen d(Rgb(s.col), Sf(1.5f)); g.DrawEllipse(&d, dx - dr, dy - dr, 2 * dr, 2 * dr); }
        Txt(g, s.name, F.s, on ? P->ink : P->sub, x + Sf(18), y + (h - Sf(15)) / 2);
        if (hasV) Txt(g, val, F.sb, on ? P->ink : P->sub, x + Sf(20) + tn, y + (h - Sf(15)) / 2);
        AddHot(w, RcF(x, y, cw, h), act, i);
        x += cw + gap;
    }
    if (act == A_CHIP) {                 // chart list: shows the ticked sensors; this chip opens / closes the full list
        const wchar_t* lab = all ? L"Done choosing" : L"＋  Choose sensors…";
        REAL cw = TextW(g, lab, F.sb) + Sf(20);
        if (x > area.X && x + cw > area.GetRight()) { x = area.X; y += h + gap; }
        RectF r(x, y, cw, h);
        GraphicsPath p; RoundPath(p, r, h / 2);
        bool hot = IsHot(w, A_BUTTON, B_PICK);
        SolidBrush b(Rgb(all ? P->accent : hot ? P->btnHot : P->btn)); g.FillPath(&b, &p);
        if (!all) { Pen bd(Rgb(P->accent), Sf(1.3f)); bd.SetDashStyle(DashStyleDash); g.DrawPath(&bd, &p); }
        Txt(g, lab, F.sb, all ? 0xFFFFFF : P->accent, x, y + (h - Sf(15)) / 2, cw, StringAlignmentCenter);
        AddHot(w, RcF(x, y, cw, h), A_BUTTON, B_PICK);
    }
    return y + h;
}

// ---- history chart: one band per unit, shared time axis, crosshair with every value at that moment
struct ChartTheme { Color ink, sub, grid, axis, tipBg, tipInk; bool shadow; float k; };
static void HTxt(Graphics& g, const wchar_t* s, Font& f, const Color& c, REAL x, REAL y, const ChartTheme& th, REAL w = 0, StringAlignment al = StringAlignmentNear) {
    StringFormat sf; sf.SetAlignment(al); sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    RectF r(x, y, w > 0 ? w : 10000.f, f.GetHeight(&g) * 1.4f);
    if (th.shadow) {               // dark outline so text stays readable on any background
        SolidBrush sb(Color((BYTE)std::min(230, c.GetA() + 30), 0, 0, 0)); REAL o = std::max(1.f, th.k);
        const REAL d[4][2] = { { -o, 0 }, { o, 0 }, { 0, -o }, { 0, o } };
        for (auto& v : d) { RectF rr = r; rr.X += v[0]; rr.Y += v[1]; g.DrawString(s, -1, &f, rr, &sf, &sb); }
    }
    SolidBrush b(c); g.DrawString(s, -1, &f, r, &sf, &b);
}
static void DrawChart(Graphics& g, Win* w, RectF a, unsigned long long mask, int range, const ChartTheme& th, Fonts& F) {
    const float k = th.k;
    const ULONGLONG now = GetTickCount64(), span = kRangeMin[range] * 60000ULL;
    mask &= ShownMask();
    int units[U_COUNT], nb = 0;
    for (int u = 0; u < U_COUNT; u++)
        for (int i = 0; i < H_COUNT; i++) if (((mask >> i) & 1) && kSeries[i].unit == u) { units[nb++] = u; break; }
    if (w) w->histPlot = {};
    if (!nb) { HTxt(g, L"Click sensors to chart them", F.s, th.sub, a.X, a.Y + a.Height / 2 - 8 * k, th, a.Width, StringAlignmentCenter); return; }
    RectF pl(a.X + 40 * k, a.Y + 2 * k, a.Width - 46 * k, a.Height - 20 * k);
    if (pl.Width < 40 * k || pl.Height < 16 * k) return;
    REAL gap = 8 * k, bh = (pl.Height - gap * (nb - 1)) / nb;
    Pen grid(th.grid, 1 * k), axis(th.axis, 1 * k);
    wchar_t t[64];
    int div = kRangeDiv[range];
    ULONGLONG gapMs = std::max<ULONGLONG>(5000, (ULONGLONG)g_s.intervalMs * 3);
    double bLo[U_COUNT] = {}, bHi[U_COUNT] = {}; RectF bR[U_COUNT];
    for (int b = 0; b < nb; b++) {
        int u = units[b];
        RectF br(pl.X, pl.Y + b * (bh + gap), pl.Width, bh);
        double mn = 1e30, mx = -1e30;
        for (size_t i = 0; i < g_hist.size(); i++) {
            const HSample& hs = HistAt(i); if (now - hs.t > span) continue;
            for (int j = 0; j < H_COUNT; j++)
                if (((mask >> j) & 1) && kSeries[j].unit == u && !std::isnan(hs.v[j])) { mn = std::min(mn, (double)hs.v[j]); mx = std::max(mx, (double)hs.v[j]); }
        }
        bool any = mx >= mn;
        int nt = std::clamp((int)(bh / (24 * k)), 1, 4);
        double lo, hi, step;
        switch (u) {
        case U_PCT: Nice(0, std::max(100.0, any ? mx : 0.0), nt, lo, hi, step); break;
        case U_RPM: Nice(0, std::max(8000.0, any ? mx : 0.0), nt, lo, hi, step); break;
        case U_W:   Nice(0, std::max(10.0, any ? mx * 1.1 : 0.0), nt, lo, hi, step); break;
        case U_MHZ: Nice(0, std::max(1000.0, any ? mx * 1.05 : 0.0), nt, lo, hi, step); break;
        case U_GB:  Nice(0, std::max(8.0, any ? mx * 1.1 : 0.0), nt, lo, hi, step); break;
        case U_MBS: Nice(0, std::max(10.0, any ? mx * 1.1 : 0.0), nt, lo, hi, step); break;
        case U_FPS: Nice(0, std::max(60.0, any ? mx * 1.1 : 0.0), nt, lo, hi, step); break;
        default: {
            double a0 = any ? mn - 3 : 30, b0 = any ? mx + 3 : 80;
            if (b0 - a0 < 12) { double c = (a0 + b0) / 2; a0 = c - 6; b0 = c + 6; }
            Nice(std::max(0.0, a0), b0, nt, lo, hi, step);
        }
        }
        bLo[u] = lo; bHi[u] = hi; bR[u] = br;
        for (double v = lo; v <= hi + step * 0.01; v += step) {
            REAL py = br.GetBottom() - (REAL)((v - lo) / (hi - lo)) * br.Height;
            g.DrawLine(&grid, br.X, py, br.GetRight(), py);
            swprintf(t, 64, L"%g", v);
            REAL ly = std::clamp(py - 8 * k, br.Y - 2 * k, br.GetBottom() - 14 * k);   // keep labels inside their own band
            HTxt(g, t, F.s, th.sub, a.X, ly, th, 36 * k, StringAlignmentFar);
        }
        for (int i = 1; i < div; i++) { REAL px = br.X + br.Width * i / div; g.DrawLine(&grid, px, br.Y, px, br.GetBottom()); }
        g.DrawRectangle(&axis, br);
        HTxt(g, kUnitName[u], F.sb, th.sub, br.X + 5 * k, br.Y + 2 * k, th);
        GraphicsState st = g.Save();
        g.SetClip(RectF(br.X, br.Y - 3 * k, br.Width, br.Height + 6 * k));
        for (int j = H_COUNT - 1; j >= 0; j--) {
            if (!((mask >> j) & 1) || kSeries[j].unit != u) continue;
            const SeriesDef& s = kSeries[j];
            Pen pen(Rgb(s.col), 1.8f * k); pen.SetLineJoin(LineJoinRound);
            Pen halo(Color(130, 0, 0, 0), 4 * k); halo.SetLineJoin(LineJoinRound);
            std::vector<PointF> seg; ULONGLONG prevT = 0;
            auto flush = [&] {
                if (seg.size() >= 2) { if (th.shadow) g.DrawLines(&halo, seg.data(), (INT)seg.size()); g.DrawLines(&pen, seg.data(), (INT)seg.size()); }
                else if (seg.size() == 1) { SolidBrush bb(Rgb(s.col)); g.FillEllipse(&bb, seg[0].X - 1.5f * k, seg[0].Y - 1.5f * k, 3 * k, 3 * k); }
                seg.clear();
            };
            for (size_t i = 0; i < g_hist.size(); i++) {
                const HSample& hs = HistAt(i); if (now - hs.t > span) continue;
                float v = hs.v[j];
                if (std::isnan(v) || (prevT && hs.t - prevT > gapMs)) flush();
                prevT = hs.t;
                if (std::isnan(v)) continue;
                REAL px = br.GetRight() - (REAL)((double)(now - hs.t) / span) * br.Width;
                REAL py = br.GetBottom() - (REAL)((std::clamp((double)v, lo, hi) - lo) / (hi - lo)) * br.Height;
                seg.push_back(PointF(px, py));
            }
            flush();
        }
        g.Restore(st);
    }
    for (int i = 0; i <= div; i++) {
        REAL px = pl.X + pl.Width * i / div;
        ULONGLONG ago = span * (div - i) / div;
        if (i == div) wcscpy(t, L"now");
        else if (span <= 60000) swprintf(t, 64, L"−%llu s", ago / 1000);
        else swprintf(t, 64, L"−%llu min", ago / 60000);
        StringAlignment al = i == 0 ? StringAlignmentNear : i == div ? StringAlignmentFar : StringAlignmentCenter;
        REAL bx = i == 0 ? px : i == div ? px - 70 * k : px - 35 * k;
        HTxt(g, t, F.s, th.sub, bx, pl.GetBottom() + 2 * k, th, 70 * k, al);
    }
    if (!w) return;
    w->histPlot = { (LONG)pl.X, (LONG)pl.Y, (LONG)pl.GetRight(), (LONG)pl.GetBottom() };
    if (w->crossX < pl.X || w->crossX > pl.GetRight() || g_hist.empty()) return;
    ULONGLONG target = now - (ULONGLONG)((pl.GetRight() - w->crossX) / pl.Width * span);
    const HSample* best = nullptr; ULONGLONG bd = ~0ULL;
    for (size_t i = 0; i < g_hist.size(); i++) {
        const HSample& hs = HistAt(i); ULONGLONG d = hs.t > target ? hs.t - target : target - hs.t;
        if (d < bd) { bd = d; best = &hs; }
    }
    if (!best || bd > gapMs || now - best->t > span) return;
    REAL cx = pl.GetRight() - (REAL)((double)(now - best->t) / span) * pl.Width;
    Pen cp(th.ink, 1 * k); cp.SetDashStyle(DashStyleDash);
    g.DrawLine(&cp, cx, pl.Y, cx, pl.GetBottom());
    std::vector<int> ids;                   // same order and grouping as the bands (one section per unit)
    for (int b = 0; b < nb; b++)
        for (int j = 0; j < H_COUNT; j++) if (((mask >> j) & 1) && kSeries[j].unit == units[b]) ids.push_back(j);
    for (int j : ids) {                     // a dot where the line crosses
        float v = best->v[j]; if (std::isnan(v)) continue;
        int u = kSeries[j].unit; RectF& br = bR[u];
        REAL py = br.GetBottom() - (REAL)((std::clamp((double)v, bLo[u], bHi[u]) - bLo[u]) / (bHi[u] - bLo[u])) * br.Height;
        SolidBrush d(Rgb(kSeries[j].col)); Pen o(th.tipInk, 1.5f * k);
        g.FillEllipse(&d, cx - 4 * k, py - 4 * k, 8 * k, 8 * k); g.DrawEllipse(&o, cx - 4 * k, py - 4 * k, 8 * k, 8 * k);
    }
    ULONGLONG ago = (now - best->t) / 1000;
    if (ago < 60) swprintf(t, 64, L"%llu s ago", ago); else swprintf(t, 64, L"%llu:%02llu ago", ago / 60, ago % 60);
    REAL lh = 17 * k, nw = TextW(g, t, F.sb), vw = 0;
    std::vector<std::wstring> vals;
    for (int j : ids) {
        wchar_t v[32]; FmtVal(v, best->v[j], kSeries[j].unit); vals.push_back(v);
        nw = std::max(nw, TextW(g, kSeries[j].name, F.s)); vw = std::max(vw, TextW(g, v, F.sb));
    }
    REAL sepH = 11 * k;                     // a separator bar between unit sections, like the gaps between the bands
    int groups = 0; for (size_t i = 0; i < ids.size(); i++) if (i == 0 || kSeries[ids[i]].unit != kSeries[ids[i - 1]].unit) groups++;
    REAL bw = 14 * k + nw + 10 * k + vw + 10 * k, bh2 = lh * (ids.size() + 1) + sepH * std::max(0, groups - 1) + 12 * k;
    REAL bx = cx + 14 * k; if (bx + bw > a.GetRight()) bx = cx - 14 * k - bw;
    REAL by = pl.Y;
    GraphicsPath tp; RoundPath(tp, RectF(bx, by, bw, bh2), 6 * k);
    SolidBrush tb(th.tipBg); g.FillPath(&tb, &tp);
    HTxt(g, t, F.sb, th.tipInk, bx + 8 * k, by + 5 * k, th);
    Pen sep(Color(90, th.tipInk.GetR(), th.tipInk.GetG(), th.tipInk.GetB()), 1 * k);
    REAL ly = by + 5 * k + lh + 2 * k;
    for (size_t i = 0; i < ids.size(); i++) {
        if (i > 0 && kSeries[ids[i]].unit != kSeries[ids[i - 1]].unit) {
            g.DrawLine(&sep, bx + 8 * k, ly + sepH / 2 - 1 * k, bx + bw - 8 * k, ly + sepH / 2 - 1 * k);
            ly += sepH;
        }
        SolidBrush d(Rgb(kSeries[ids[i]].col)); g.FillEllipse(&d, bx + 8 * k, ly + 5 * k, 7 * k, 7 * k);
        HTxt(g, kSeries[ids[i]].name, F.s, th.tipInk, bx + 20 * k, ly, th);
        HTxt(g, vals[i].c_str(), F.sb, th.tipInk, bx + bw - 8 * k - vw, ly, th, vw, StringAlignmentFar);
        ly += lh;
    }
}
static ChartTheme UiChartTheme() {
    return { Rgb(P->ink), Rgb(P->sub), Rgb(P->grid), Rgb(P->axis), Rgb(P->tipBg, 240), Rgb(P->tipInk), false, g_dpi / 96.f };
}

// ======================================================================= window contents
static const int MainMinW = 1000, MainMinH = 860, kBottom = 132;
static int MidAvail(const Win& w) { return w.ch - kBottom - 10 - 236 - 30; }
static int CurveH(const Win& w) { return MidAvail(w) * 40 / 100; }
static RECT CardL(const Win& w, int i) { int cw = (w.cw - 32 - 2 * 24) / 3, x = 16 + i * (cw + 24); return { x, 78, x + cw, 224 }; }
static RECT CurveL(const Win& w, int g) { int pw = (w.cw - 32 - 24) / 3, x = 16 + g * (pw + 12); return { x, 236, x + pw, 236 + CurveH(w) }; }
static RECT HistL(const Win& w) { return { 16, 236 + CurveH(w) + 30, w.cw - 16, w.ch - kBottom - 10 }; }
static RECT Scale(RECT r) { return { S(r.left), S(r.top), S(r.right), S(r.bottom) }; }
static RECT PlotOf(RECT p) { return { p.left + S(40), p.top + S(40), p.right - S(14), p.bottom - S(26) }; }
static Curve& CurveOf(int g) { return g == 0 ? g_s.gpu : g == 1 ? g_s.cpu : g_s.sys; }
static bool CurvePanelPx(const Win& w, int g, RECT& r) {      // where curve g is drawn in this window (content px)
    if (w.kind == WK_MAIN) { r = Scale(CurveL(w, g)); return true; }
    if (w.kind == WK_ZGPU + g) { r = { S(12), S(12), S(w.cw) - S(12), S(w.ch) - S(36) }; return true; }
    return false;
}

static void DrawCard(Graphics& g, Fonts& F, int idx, RectF rc) {
    static const wchar_t* names[3] = { L"GPU", L"CPU", L"System" };
    static const DWORD cols[3] = { 0x5FA300, 0x2F80ED, 0x8B5CF6 };
    REAL x = rc.X, y = rc.Y, w = rc.Width, h = rc.Height;
    bool drv = g_live.driver == idx && g_live.fansOk;
    GraphicsPath p; RoundPath(p, rc, Sf(8));
    SolidBrush bg(Rgb(P->card)); g.FillPath(&bg, &p);
    Pen bd(Rgb(drv ? cols[idx] : P->line), drv ? Sf(2) : Sf(1)); g.DrawPath(&bd, &p);
    SolidBrush dot(Rgb(cols[idx])); g.FillEllipse(&dot, x + Sf(12), y + Sf(15), Sf(9), Sf(9));
    std::wstring title = names[idx];
    if (idx == 0 && !g_mon.gpuName.empty()) title += L"  ·  " + g_mon.gpuName;
    if (idx == 1 && !g_mon.cpuName.empty()) title += L"  ·  " + g_mon.cpuName;
    Txt(g, title.c_str(), F.b, P->ink, x + Sf(28), y + Sf(10), w - Sf(110));
    if (drv) Txt(g, L"drives pad", F.s, cols[idx], x + w - Sf(100), y + Sf(12), Sf(88), StringAlignmentFar);

    wchar_t v[5][96] = {}; const wchar_t* k[5] = {};
    auto& L = g_live; auto& M = g_mon;
    auto temp = [](wchar_t* b, double t) { if (t > -100) swprintf(b, 96, L"%.0f °C", t); else wcscpy(b, L"—"); };
    int rpm = idx == 0 ? L.gpu : idx == 1 ? L.cpu : L.sys; double pct = idx == 0 ? L.gpuP : idx == 1 ? L.cpuP : L.sysP;
    double contrib = idx == 0 ? L.gpuV : idx == 1 ? L.cpuV : L.sysV;
    k[1] = L"Fan → pad";
    if (L.fansOk) swprintf(v[1], 96, L"%d rpm · %.0f%%  →  %.0f%%", rpm, pct, contrib); else wcscpy(v[1], L"—");
    if (idx == 0) {
        bool asleep = M.gpuState == 4;
        k[0] = L"Temperature"; if (asleep) wcscpy(v[0], L"asleep (not woken)"); else temp(v[0], M.gpuT);
        k[2] = L"Clock";       if (M.gpuClk >= 0) swprintf(v[2], 96, L"%d MHz · mem %d", M.gpuClk, M.gpuMem); else wcscpy(v[2], asleep ? L"asleep" : L"—");
        k[3] = L"Usage";       if (M.gpuUse >= 0) swprintf(v[3], 96, L"%d%%", M.gpuUse); else wcscpy(v[3], asleep ? L"0% (asleep)" : L"—");
        k[4] = L"Power";       if (M.gpuW >= 0) swprintf(v[4], 96, L"%.1f W", M.gpuW); else wcscpy(v[4], asleep ? L"~0 W (asleep)" : L"—");
    } else if (idx == 1) {
        k[0] = L"Temperature"; temp(v[0], L.cpuT >= 0 ? L.cpuT : -1000);
        if (M.hybrid) {
            k[2] = L"P-cores"; if (M.have) swprintf(v[2], 96, L"%.2f GHz  ·  %.0f%% used", M.pGHz, M.pUse); else wcscpy(v[2], L"…");
            k[3] = L"E-cores"; if (M.have) swprintf(v[3], 96, L"%.2f GHz  ·  %.0f%% used", M.eGHz, M.eUse); else wcscpy(v[3], L"…");
        } else {
            k[2] = L"Clock"; if (M.have) swprintf(v[2], 96, L"%.2f GHz", M.pGHz); else wcscpy(v[2], L"…");
            k[3] = L"Usage"; if (M.have) swprintf(v[3], 96, L"%.0f%%", M.allUse); else wcscpy(v[3], L"…");
        }
        k[4] = L"Power";
        if (M.cpuW >= 0) swprintf(v[4], 96, L"%.1f W   (cores %.1f W)", M.cpuW, std::max(0.0, M.coresW)); else wcscpy(v[4], M.have ? L"not available" : L"…");
    } else {
        k[0] = L"Temperature"; temp(v[0], M.sysT); if (M.sysT > -100) wcscat(v[0], L"   (ACPI zone)");
        k[2] = L"iGPU power"; if (M.igpuW >= 0) swprintf(v[2], 96, L"%.1f W", M.igpuW); else wcscpy(v[2], L"—");
        k[3] = L"Pad fan";            // the speed the pad itself reports; rpm / dB are estimated from it
        if (!L.padOk) wcscpy(v[3], L"not connected");
        else if (L.padOff || L.pad <= 0) wcscpy(v[3], L"stopped · silent");
        else swprintf(v[3], 96, L"%d%%%s · ≈ %d rpm · ≈ %.0f dB", L.pad, L.padWheel ? L" (wheel)" : L"", PadRpm(L.pad), PadNoise(L.pad));
        k[4] = L"Memory";
        if (M.ramGB >= 0) swprintf(v[4], 96, L"%.1f / %.0f GB · commit %.0f GB", M.ramGB, M.ramTotal, M.commitGB); else wcscpy(v[4], L"…");
    }
    for (int i = 0; i < 5; i++) {
        if (!k[i]) continue;
        REAL ly = y + Sf(36) + i * Sf(21);
        Txt(g, k[i], F.n, P->sub, x + Sf(14), ly);
        Txt(g, v[i], F.n, P->ink, x + Sf(98), ly, w - Sf(108));
    }
}
static void Button(Graphics& g, Win& w, Fonts& F, RectF r, const wchar_t* label, int id, bool primary);
static void DrawStatus(Graphics& g, Fonts& F, Win& w) {
    auto& L = g_live;
    int state = g_paused ? 1 : !L.fansOk ? 2 : !L.padOk ? 3 : L.padOff ? 4 : g_wheelHold ? 5 : 0;
    DWORD col = (state && state != 5) ? 0x6B7280 : LevelRgb(L.pad);
    GraphicsPath p; RoundPath(p, RectF(Sf(16), Sf(12), Sf(150), Sf(54)), Sf(10));
    SolidBrush b(Rgb(col)); g.FillPath(&b, &p);
    Txt(g, L"LLANO PAD", F.s, 0xFFFFFF, Sf(16), Sf(15), Sf(150), StringAlignmentCenter);
    wchar_t t[64];
    if (L.padOk) swprintf(t, 64, L"%d%%", L.pad); else wcscpy(t, L"—");
    Txt(g, t, F.big, 0xFFFFFF, Sf(16), Sf(29), Sf(150), StringAlignmentCenter);
    static const wchar_t* who[3] = { L"GPU fan", L"CPU fan", L"system fan" };
    const wchar_t* line1; wchar_t buf[160];
    switch (state) {
    case 1: line1 = L"Paused — the pad keeps its current speed"; break;
    case 2: line1 = L"Cannot read the laptop fans"; break;
    case 3: line1 = L"Llano pad not connected — waiting for it"; break;
    case 4: line1 = L"Pad is switched off (power button) — left alone"; break;
    case 5: line1 = L"Manual — you set the pad with its wheel, so the curves leave it alone"; break;
    default:
        if (L.driver < 0) line1 = L"All laptop fans are idle — pad off";
        else { swprintf(buf, 160, L"Following the %s", who[L.driver]); line1 = buf; }
    }
    Txt(g, line1, F.b, P->ink, Sf(184), Sf(16), Sf((REAL)w.cw - (state == 5 ? 400 : 200)));
    if (state == 5) Button(g, w, F, RectF(Sf((REAL)w.cw - 200), Sf(14), Sf(184), Sf(30)), L"Take back control", B_TAKEBACK, true);
    wchar_t l2[200];
    swprintf(l2, 200, L"Curve results:  GPU → %.0f%%    CPU → %.0f%%    System → %.0f%%      Highest wins.", L.gpuV, L.cpuV, L.sysV);
    Txt(g, L.fansOk ? l2 : L"", F.n, P->sub, Sf(184), Sf(40), Sf((REAL)w.cw - 200));
}

static float PX(const RECT& pl, double x) { return (float)(pl.left + (pl.right - pl.left) * x / 100.0); }
static float PY(const RECT& pl, double y) { return (float)(pl.bottom - (pl.bottom - pl.top) * y / 100.0); }

static void DrawCurvePanel(Graphics& g, Win& w, Fonts& F, int gi, RECT pr, bool expand, bool big) {
    static const wchar_t* title[3] = { L"GPU fan  →  pad", L"CPU fan  →  pad", L"System fan  →  pad" };
    static const wchar_t* maxs[3] = { L"fan 100% = 6600 rpm", L"fan 100% = 6800 rpm", L"fan 100% = 7300 rpm" };
    static const DWORD cols[3] = { 0x5FA300, 0x2F80ED, 0x8B5CF6 };
    RECT pl = PlotOf(pr);
    Curve& c = CurveOf(gi);
    DWORD col = cols[gi];
    GraphicsPath pp; RoundPath(pp, RectF((REAL)pr.left, (REAL)pr.top, (REAL)(pr.right - pr.left), (REAL)(pr.bottom - pr.top)), Sf(8));
    Pen bd(Rgb(P->line), Sf(1)); g.DrawPath(&bd, &pp);
    Txt(g, title[gi], big ? F.h : F.b, P->ink, (REAL)pr.left + Sf(12), (REAL)pr.top + Sf(big ? 8 : 11));
    REAL rEdge = (REAL)pr.right - Sf(expand ? 42 : 12);
    Txt(g, maxs[gi], F.s, P->sub, rEdge - Sf(150), (REAL)pr.top + Sf(13), Sf(150), StringAlignmentFar);
    if (expand) ExpandBtn(g, w, RectF((REAL)pr.right - Sf(36), (REAL)pr.top + Sf(6), Sf(28), Sf(28)), gi);

    Pen grid(Rgb(P->grid), Sf(1));
    for (int i = 0; i <= (big ? 20 : 10); i++) {
        double v = i * (big ? 5 : 10);
        g.DrawLine(&grid, PX(pl, v), (REAL)pl.top, PX(pl, v), (REAL)pl.bottom);
        g.DrawLine(&grid, (REAL)pl.left, PY(pl, v), (REAL)pl.right, PY(pl, v));
    }
    Pen axis(Rgb(P->axis), Sf(1)); g.DrawRectangle(&axis, (REAL)pl.left, (REAL)pl.top, (REAL)(pl.right - pl.left), (REAL)(pl.bottom - pl.top));
    wchar_t t[48];
    for (int i = 0; i <= 100; i += big ? 10 : 25) {
        swprintf(t, 48, L"%d", i);
        Txt(g, t, F.s, P->sub, PX(pl, i) - Sf(20), (REAL)pl.bottom + Sf(4), Sf(40), StringAlignmentCenter);
        Txt(g, t, F.s, P->sub, (REAL)pl.left - Sf(36), PY(pl, i) - Sf(8), Sf(30), StringAlignmentFar);
    }
    Txt(g, L"pad %", F.s, P->sub, (REAL)pl.left + Sf(4), (REAL)pl.top + Sf(2));

    std::vector<PointF> v; v.push_back(PointF(PX(pl, 0), PY(pl, c.pts[0].second)));
    for (auto& p : c.pts) v.push_back(PointF(PX(pl, p.first), PY(pl, p.second)));
    if (c.pts.back().first < 100) v.push_back(PointF(PX(pl, 100), PY(pl, c.pts.back().second)));
    std::vector<PointF> fill = v; fill.push_back(PointF(v.back().X, PY(pl, 0))); fill.push_back(PointF(PX(pl, 0), PY(pl, 0)));
    SolidBrush area(Rgb(col, P->dark ? 40 : 28)); g.FillPolygon(&area, fill.data(), (INT)fill.size());
    Pen cp(Rgb(col), Sf(2.5f)); cp.SetLineJoin(LineJoinRound); g.DrawLines(&cp, v.data(), (INT)v.size());

    if (g_live.fansOk) {
        double x = std::min(100.0, gi == 0 ? g_live.gpuP : gi == 1 ? g_live.cpuP : g_live.sysP);
        double y = gi == 0 ? g_live.gpuV : gi == 1 ? g_live.cpuV : g_live.sysV;
        Pen mk(Rgb(kNowCol), Sf(1.5f)); mk.SetDashStyle(DashStyleDash);
        g.DrawLine(&mk, PX(pl, x), (REAL)pl.top, PX(pl, x), (REAL)pl.bottom);
        SolidBrush mb(Rgb(kNowCol)); REAL r = Sf(5);
        g.FillEllipse(&mb, PX(pl, x) - r, PY(pl, y) - r, 2 * r, 2 * r);
        swprintf(t, 48, L"now %.0f%% → %.0f%%", x, y);
        bool left = x > 60;
        Txt(g, t, F.s, kNowCol, left ? PX(pl, x) - Sf(126) : PX(pl, x) + Sf(6), (REAL)pl.top + Sf(4), Sf(120), left ? StringAlignmentFar : StringAlignmentNear);
    }
    for (int i = 0; i < (int)c.pts.size(); i++) {
        bool hot = (w.dragG == gi && w.dragI == i) || (w.dragG < 0 && w.hovG == gi && w.hovI == i);
        REAL r = Sf(hot ? 7.f : 5.f), x = PX(pl, c.pts[i].first), y = PY(pl, c.pts[i].second);
        SolidBrush wb(Rgb(P->bg)); Pen o(Rgb(col), Sf(2));
        g.FillEllipse(&wb, x - r, y - r, 2 * r, 2 * r); g.DrawEllipse(&o, x - r, y - r, 2 * r, 2 * r);
        if (hot) {
            swprintf(t, 48, L"%g%%  →  %g%%", c.pts[i].first, c.pts[i].second);
            REAL bw = Sf(96), bh = Sf(22), bx = std::clamp(x - bw / 2, (REAL)pl.left, (REAL)pl.right - bw), by = y - Sf(34);
            if (by < pl.top) by = y + Sf(12);
            GraphicsPath tp; RoundPath(tp, RectF(bx, by, bw, bh), Sf(5));
            SolidBrush tb(Rgb(P->tipBg)); g.FillPath(&tb, &tp);
            Txt(g, t, F.s, P->tipInk, bx, by + Sf(4), bw, StringAlignmentCenter);
        }
    }
}
// Returns how many px the panel is short of (or over) what keeps every unit band readable (~44 px each).
static int DrawHistPanel(Graphics& g, Win& w, Fonts& F, RECT r, bool expand) {
    GraphicsPath pp; RoundPath(pp, RectF((REAL)r.left, (REAL)r.top, (REAL)(r.right - r.left), (REAL)(r.bottom - r.top)), Sf(8));
    Pen bd(Rgb(P->line), Sf(1)); g.DrawPath(&bd, &pp);
    REAL l = (REAL)r.left + Sf(12), t = (REAL)r.top, right = (REAL)r.right - Sf(12);
    Txt(g, L"History", expand ? F.b : F.h, P->ink, l, t + Sf(expand ? 12 : 9));
    if (expand) { ExpandBtn(g, w, RectF(right - Sf(28), t + Sf(7), Sf(28), Sf(28)), 3); right -= Sf(38); }
    REAL sw = SegWidth(g, F, kRangeShort, 5);
    Seg(g, w, F, right - sw, t + Sf(8), kRangeShort, 5, g_s.chartRange, G_RANGE);
    Button(g, w, F, RectF(right - sw - Sf(104), t + Sf(8), Sf(92), Sf(26)), L"Clear all", B_CHARTNONE);
    REAL hx = l + TextW(g, L"History", expand ? F.b : F.h) + Sf(10);
    Txt(g, L"click a sensor to hide it  ·  ＋ Choose sensors to add more  ·  hover the chart to read every value at that moment", F.s, P->sub,
        hx, t + Sf(14), std::max(0.f, right - sw - Sf(112) - hx));
    REAL yb = Chips(g, w, F, RectF(l, t + Sf(44), right - l + (expand ? Sf(38) : 0), 0), g_s.chartMask, A_CHIP, w.pick);
    RectF ca(l, yb + Sf(10), (REAL)r.right - Sf(12) - l, (REAL)r.bottom - Sf(10) - (yb + Sf(10)));
    ChartTheme th = UiChartTheme();
    DrawChart(g, &w, ca, g_s.chartMask, g_s.chartRange, th, F);
    int nb = 0; unsigned long long m = g_s.chartMask & ShownMask();
    for (int u = 0; u < U_COUNT; u++) for (int i = 0; i < H_COUNT; i++) if (((m >> i) & 1) && kSeries[i].unit == u) { nb++; break; }
    REAL needChart = nb ? nb * Sf(44) + (nb - 1) * Sf(8) + Sf(26) : Sf(80);
    return (int)(needChart - ca.Height);
}
// content height (96-dpi) that fits the chart: grows when many bands are ticked (the window scrolls), shrinks back after
static int FitHeight(const Win& w, int base, int shortPx) { return std::max(base, w.ch + (int)std::ceil(shortPx * 96.0 / w.dpi)); }

static int PaintMain(Graphics& g, Win& w, Fonts& F) {
    DrawStatus(g, F, w);
    for (int i = 0; i < 3; i++) { RECT c = Scale(CardL(w, i)); DrawCard(g, F, i, RectF((REAL)c.left, (REAL)c.top, (REAL)(c.right - c.left), (REAL)(c.bottom - c.top))); }
    for (int i = 0; i < 3; i++) DrawCurvePanel(g, w, F, i, Scale(CurveL(w, i)), true, false);
    REAL hy = (REAL)S(CurveL(w, 0).bottom);
    Txt(g, L"Across: laptop fan %   Up: pad %   ·   click to add a point  ·  drag a point to move it  ·  right-click a point to remove it  ·  the corner icon opens a bigger window",
        F.s, P->sub, Sf(16), hy + Sf(7), Sf((REAL)w.cw - 32));
    int shortPx = DrawHistPanel(g, w, F, Scale(HistL(w)), true);

    REAL y1 = Sf((REAL)w.ch - 122), y2 = Sf((REAL)w.ch - 84), y3 = Sf((REAL)w.ch - 46);
    REAL x = Sf(16);
    x += Toggle(g, w, F, x, y1, L"Show overlay", g_s.ovOn != 0, T_OVERLAY) + Sf(18);
    Button(g, w, F, RectF(x, y1 - Sf(3), Sf(150), Sf(28)), L"Overlay settings…", B_OVSET); x += Sf(170);
    Toggle(g, w, F, x, y1, L"Lock overlay in place (click-through)", g_s.ovLocked != 0, T_OVLOCK, g_s.ovOn != 0);
    x = Sf(16);
    x += Toggle(g, w, F, x, y2, L"Start with Windows", g_startup, T_STARTUP) + Sf(24);
    Txt(g, L"Update every", F.n, P->ink, x, y2 + Sf(2)); x += TextW(g, L"Update every", F.n) + Sf(6);
    int isel = -1; for (int i = 0; i < 6; i++) if (kIntervals[i] == g_s.intervalMs) isel = i;
    x += Seg(g, w, F, x, y2 - Sf(3), kIntervalName, 6, isel, G_INTERVAL) + Sf(24);
    Toggle(g, w, F, x, y2, L"Pause syncing", g_paused, T_PAUSE);
    x = Sf(16);
    Txt(g, L"Theme", F.n, P->ink, x, y3 + Sf(5)); x += TextW(g, L"Theme", F.n) + Sf(6);
    x += Seg(g, w, F, x, y3, kThemeName, 3, g_s.theme, G_THEME) + Sf(24);
    Toggle(g, w, F, x, y3 + Sf(3), L"Also record GPU, SSD temps and FPS in the background", g_s.recordHidden != 0, T_RECORD);
    REAL rx = Sf((REAL)w.cw - 16);
    Button(g, w, F, RectF(rx - Sf(124), y3 - Sf(2), Sf(124), Sf(30)), L"Hide to tray", B_HIDE);
    Button(g, w, F, RectF(rx - Sf(258), y3 - Sf(2), Sf(124), Sf(30)), L"Reset curves", B_RESET);
    return FitHeight(w, MainMinH, shortPx);
}
static void PaintZoomCurve(Graphics& g, Win& w, Fonts& F) {
    int gi = w.kind - WK_ZGPU; RECT pr; CurvePanelPx(w, gi, pr);
    DrawCurvePanel(g, w, F, gi, pr, false, true);
    Txt(g, L"Across: laptop fan %   Up: pad %   ·   click to add a point  ·  drag a point to move it  ·  right-click a point to remove it  ·  applies instantly",
        F.s, P->sub, Sf(14), (REAL)pr.bottom + Sf(8), Sf((REAL)w.cw - 28));
}
static int PaintZoomHist(Graphics& g, Win& w, Fonts& F) {
    int shortPx = DrawHistPanel(g, w, F, { S(12), S(12), S(w.cw) - S(12), S(w.ch) - S(12) }, false);
    return FitHeight(w, 420, shortPx);
}
static int PaintOvSet(Graphics& g, Win& w, Fonts& F) {      // returns content height (96-dpi units)
    REAL x0 = Sf(22), xc = Sf(200), right = Sf((REAL)w.cw - 22), y = Sf(16);
    wchar_t t[64];
    Txt(g, L"Overlay", F.h, P->ink, x0, y);
    Txt(g, L"An always-on-top readout. Everything here applies instantly.", F.s, P->sub, x0, y + Sf(24));
    y += Sf(56);
    auto label = [&](const wchar_t* s) { Txt(g, s, F.n, P->sub, x0, y + Sf(4)); };
    REAL tx = xc + Toggle(g, w, F, xc, y, L"Show overlay", g_s.ovOn != 0, T_OVERLAY) + Sf(28);
    Toggle(g, w, F, tx, y, L"Lock in place (click-through)", g_s.ovLocked != 0, T_OVLOCK);
    y += Sf(38);
    label(L"Show"); Seg(g, w, F, xc, y, kOvModeName, 3, g_s.ovMode, G_OVMODE); y += Sf(40);
    label(L"Sensors");
    Button(g, w, F, RectF(x0, y + Sf(26), Sf(90), Sf(24)), L"Clear all", B_OVNONE);
    y = Chips(g, w, F, RectF(xc, y, right - xc, 0), g_s.ovMask, A_OVCHIP) + Sf(16);
    label(L"Chart time range"); Seg(g, w, F, xc, y, kRangeShort, 5, g_s.ovRange, G_OVRANGE); y += Sf(40);
    label(L"Number layout"); Seg(g, w, F, xc, y, kOvLayoutName, 2, g_s.ovLayout, G_OVLAYOUT); y += Sf(40);
    label(L"Text size"); Seg(g, w, F, xc, y, kOvSizeName, 5, g_s.ovSize, G_OVSIZE); y += Sf(40);
    label(L"Text colour");
    REAL sx = xc + Swatches(g, w, xc, y + Sf(2), kTxtSw, 8, (DWORD)g_s.ovTextColor, 0);
    Button(g, w, F, RectF(sx + Sf(6), y, Sf(92), Sf(26)), L"Custom…", B_TXTCUSTOM); y += Sf(38);
    Toggle(g, w, F, xc, y, L"Outline text (readable without a background)", g_s.ovOutline != 0, T_OVOUTLINE); y += Sf(36);
    label(L"Background"); Toggle(g, w, F, xc, y, L"Show a background", g_s.ovBg != 0, T_OVBG); y += Sf(36);
    label(L"Background colour");
    sx = xc + Swatches(g, w, xc, y + Sf(2), kBgSw, 8, (DWORD)g_s.ovBgColor, 1);
    Button(g, w, F, RectF(sx + Sf(6), y, Sf(92), Sf(26)), L"Custom…", B_BGCUSTOM); y += Sf(38);
    label(L"Background opacity"); swprintf(t, 64, L"%d%%", g_s.ovBgOpacity);
    Slider(g, w, F, RectF(xc, y, Sf(280), Sf(26)), g_s.ovBgOpacity, 0, 100, SL_OVBGOPAC, t); y += Sf(38);
    label(L"Overall opacity"); swprintf(t, 64, L"%d%%", g_s.ovOpacity);
    Slider(g, w, F, RectF(xc, y, Sf(280), Sf(26)), g_s.ovOpacity, 10, 100, SL_OVOPAC, t); y += Sf(38);
    label(L"Chart width"); swprintf(t, 64, L"%d px", g_s.ovW);
    Slider(g, w, F, RectF(xc, y, Sf(280), Sf(26)), g_s.ovW, 160, 1600, SL_OVW, t); y += Sf(38);
    label(L"Chart height"); swprintf(t, 64, L"%d px", g_s.ovH);
    Slider(g, w, F, RectF(xc, y, Sf(280), Sf(26)), g_s.ovH, 60, 1000, SL_OVH, t); y += Sf(42);
    label(L"Position");
    REAL cwd = Sf(38), chh = Sf(26), gp = Sf(4);
    for (int a = 1; a <= 9; a++) {
        int c = (a - 1) % 3, r = (a - 1) / 3;
        RectF rc(xc + c * (cwd + gp), y + r * (chh + gp), cwd, chh);
        bool sel = g_s.ovAnchor == a, hot = IsHot(w, A_ANCHOR, a);
        GraphicsPath p; RoundPath(p, rc, Sf(4));
        SolidBrush b(Rgb(sel ? P->accent : hot ? P->btnHot : P->btn)); g.FillPath(&b, &p);
        Pen bd(Rgb(P->line), Sf(1)); g.DrawPath(&bd, &p);
        SolidBrush d(Rgb(sel ? 0xFFFFFF : P->sub));
        REAL dx = rc.X + (c == 0 ? Sf(7) : c == 1 ? cwd / 2 - Sf(3) : cwd - Sf(13)), dy = rc.Y + (r == 0 ? Sf(5) : r == 1 ? chh / 2 - Sf(3) : chh - Sf(11));
        g.FillRectangle(&d, dx, dy, Sf(6), Sf(6));
        AddHot(w, RcF(rc.X, rc.Y, cwd, chh), A_ANCHOR, a);
    }
    REAL nx = xc + 3 * (cwd + gp) + Sf(16);
    Txt(g, kAnchorName[g_s.ovAnchor], F.b, P->ink, nx, y + Sf(4));
    Txt(g, L"Pick a spot, or drag the overlay anywhere (while unlocked).", F.s, P->sub, nx, y + Sf(26), right - nx);
    Txt(g, L"Drag its bottom-right corner to resize the chart. Right-click it for options.", F.s, P->sub, nx, y + Sf(44), right - nx);
    y += 3 * (chh + gp) + Sf(20);
    Button(g, w, F, RectF(right - Sf(110), y, Sf(110), Sf(30)), L"Close", B_CLOSE, true);
    y += Sf(46);
    return (int)(y * 96 / w.dpi) + 1;
}

static void PaintWin(Win& w, HDC hdc) {
    RECT rc; GetClientRect(w.h, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bm);
    int needH = 0;
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(Rgb(P->bg));
        g.TranslateTransform((REAL)-w.sx, (REAL)-w.sy);
        w.hots.clear();
        Fonts F(w.dpi / 96.f);
        switch (w.kind) {
        case WK_MAIN: needH = PaintMain(g, w, F); break;
        case WK_ZHIST: needH = PaintZoomHist(g, w, F); break;
        case WK_OVSET: needH = PaintOvSet(g, w, F); break;
        default: PaintZoomCurve(g, w, F); break;
        }
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
    if (needH && std::abs(needH - w.minH) > 2) { w.minH = needH; PostMessageW(w.h, WM_APP_RELAYOUT, 0, 0); }
}

// ======================================================================= scrolling + layout
static void Layout(Win& w) {
    static bool busy = false;
    if (busy || !w.h) return;
    busy = true;
    for (int pass = 0; pass < 3; pass++) {           // showing one scroll bar can make the other one necessary
        RECT rc; GetClientRect(w.h, &rc);
        SCROLLINFO si = { sizeof si, SIF_RANGE | SIF_PAGE };
        si.nMax = std::max(S(w.minW), (int)rc.right) - 1; si.nPage = rc.right; SetScrollInfo(w.h, SB_HORZ, &si, TRUE);
        si.nMax = std::max(S(w.minH), (int)rc.bottom) - 1; si.nPage = rc.bottom; SetScrollInfo(w.h, SB_VERT, &si, TRUE);
        RECT rc2; GetClientRect(w.h, &rc2);
        if (rc2.right == rc.right && rc2.bottom == rc.bottom) break;
    }
    RECT rc; GetClientRect(w.h, &rc);
    w.cw = std::max(w.minW, MulDiv(rc.right, 96, w.dpi)); w.ch = std::max(w.minH, MulDiv(rc.bottom, 96, w.dpi));
    SCROLLINFO si = { sizeof si, SIF_POS };
    GetScrollInfo(w.h, SB_HORZ, &si); w.sx = si.nPos;
    GetScrollInfo(w.h, SB_VERT, &si); w.sy = si.nPos;
    InvalidateRect(w.h, nullptr, FALSE);
    busy = false;
}
static void ScrollTo(Win& w, int bar, int pos) {
    SCROLLINFO si = { sizeof si, SIF_POS }; si.nPos = pos;
    SetScrollInfo(w.h, bar, &si, TRUE);
    GetScrollInfo(w.h, bar, &si);
    int& cur = bar == SB_HORZ ? w.sx : w.sy;
    if (si.nPos != cur) { cur = si.nPos; InvalidateRect(w.h, nullptr, FALSE); }
}
static void OnScroll(Win& w, int bar, int code) {
    SCROLLINFO si = { sizeof si, SIF_ALL }; GetScrollInfo(w.h, bar, &si);
    int pos = si.nPos;
    switch (code) {
    case SB_LINEUP: pos -= S(40); break;
    case SB_LINEDOWN: pos += S(40); break;
    case SB_PAGEUP: pos -= (int)si.nPage; break;
    case SB_PAGEDOWN: pos += (int)si.nPage; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP: pos = 0; break;
    case SB_BOTTOM: pos = si.nMax; break;
    default: return;
    }
    ScrollTo(w, bar, pos);
}

// ======================================================================= activity (sensors only while someone looks)
static void EnsureGdip() { if (!g_gdip) { GdiplusStartupInput in; GdiplusStartup(&g_gdip, &in, nullptr); } }
static bool OvShown() { return g_ov && IsWindowVisible(g_ov); }
static bool AnyUi() { for (auto& w : g_w) if (w.h && IsWindowVisible(w.h)) return true; return false; }
static void UpdateActivity() {
    bool ui = AnyUi();
    static int eco = -1;
    int want = ui ? 0 : 1;
    if (want != eco) {
        SetEco(want != 0); eco = want;
        if (want) SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);   // give the UI memory back
    }
}
static bool Shown(int kind) { return g_w[kind].h && IsWindowVisible(g_w[kind].h) && !IsIconic(g_w[kind].h); }
// Which sensor groups to read this tick.
// Always (measured ~0.6-1.7 ms each per 2 s, no side effects): CPU clocks/usage, CPU power, CPU temp, ACPI temp,
// memory, disk activity - so their history is always complete.
// Only while shown (or "record in the background" is on), because they have side effects:
//   GPU (polling NVML can keep the dGPU awake), SSD temperature (a command to the drive), FPS (runs inside the game).
// need = the ticked sensors that are displayed somewhere (or recorded).
static int MonFlags(unsigned long long& need) {
    need = 0;
    bool chart = Shown(WK_MAIN) || Shown(WK_ZHIST);
    if (chart || g_s.recordHidden) need |= g_s.chartMask;
    if (OvShown() || Shown(WK_OVSET) || g_s.recordHidden) need |= g_s.ovMask;
    need &= ShownMask();
    int f = M_CPU | M_POW | M_TZ | M_CPUT | M_RAM | M_DISK;
    if (Shown(WK_MAIN)) f |= M_GPU;                                            // the GPU card
    auto any = [&](std::initializer_list<int> ids) { for (int i : ids) if ((need >> i) & 1) return true; return false; };
    if (any({ H_PCLK, H_ECLK, H_PUSE, H_EUSE, H_ALLUSE })) f |= M_CPU;
    if (any({ H_CPUW, H_COREW, H_IGPUW })) f |= M_POW;
    if (any({ H_SYST })) f |= M_TZ;
    if (any({ H_GPUT, H_GPUW, H_GPUCLK, H_GPUMEM, H_GPUUSE })) f |= M_GPU;
    if (any({ H_CPUT })) f |= M_CPUT;
    if (any({ H_RAMPCT, H_RAMGB, H_COMMITPCT, H_COMMITGB })) f |= M_RAM;
    if (any({ H_FPS })) f |= M_FPS;
    for (int d = 0; d < kMaxDisk; d++) {
        if ((need >> (H_DISK + 4 * d)) & 1) f |= M_DISKT;
        if ((need >> (H_DISK + 4 * d + 1)) & 7) f |= M_DISK;
    }
    return f;
}
static void InvalidateAll() { for (auto& w : g_w) if (w.h && IsWindowVisible(w.h)) InvalidateRect(w.h, nullptr, FALSE); }
static void Save() { SaveSettings(g_s); g_iniT = IniTime(); g_lastSent = -1; }

static void CenterOnWork(HWND h, int w, int ht) {
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoW(MonitorFromWindow(g_w[WK_MAIN].h ? g_w[WK_MAIN].h : h, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT wa = mi.rcWork;
    w = std::min<int>(w, wa.right - wa.left); ht = std::min<int>(ht, wa.bottom - wa.top);
    SetWindowPos(h, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - ht) / 2, w, ht, SWP_NOZORDER | SWP_NOACTIVATE);
}
static const wchar_t* kWinClass = L"LlanoFanSyncWin";
static void OpenWin(int kind) {
    Win& w = g_w[kind];
    EnsureGdip();
    if (!w.h) {
        static const wchar_t* titles[WK_COUNT] = { L"Llano Fan Sync", L"GPU fan → pad curve — Llano Fan Sync", L"CPU fan → pad curve — Llano Fan Sync",
                                                   L"System fan → pad curve — Llano Fan Sync", L"History — Llano Fan Sync", L"Overlay settings — Llano Fan Sync" };
        w.kind = kind;
        w.minW = kind == WK_OVSET ? 660 : kind == WK_ZHIST ? 620 : 440;
        w.minH = kind == WK_OVSET ? 900 : kind == WK_ZHIST ? 420 : 340;
        CreateWindowExW(0, kWinClass, titles[kind], WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 200, 200, nullptr, nullptr, g_inst, &w);
        if (!w.h) return;
        ThemeWindow(w.h);
        MONITORINFO mi = { sizeof mi }; GetMonitorInfoW(MonitorFromWindow(w.h, MONITOR_DEFAULTTOPRIMARY), &mi);
        int ww = mi.rcWork.right - mi.rcWork.left, wh = mi.rcWork.bottom - mi.rcWork.top;
        if (kind == WK_OVSET) CenterOnWork(w.h, MulDiv(700, w.dpi, 96), MulDiv(960, w.dpi, 96));
        else CenterOnWork(w.h, ww * 85 / 100, wh * 85 / 100);
        Layout(w);
    }
    if (IsIconic(w.h)) ShowWindow(w.h, SW_RESTORE); else ShowWindow(w.h, SW_SHOW);
    SetForegroundWindow(w.h);
    UpdateActivity();
    Tick();
}
static void ShowUi() {
    Win& m = g_w[WK_MAIN];
    EnsureGdip();
    g_startup = GetStartup();
    if (!IsWindowVisible(m.h)) {
        RECT wr; GetWindowRect(m.h, &wr);
        CenterOnWork(m.h, wr.right - wr.left, wr.bottom - wr.top);
    }
    ShowWindow(m.h, IsIconic(m.h) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(m.h);
    g_visible = true;
    UpdateActivity();
    Tick();
}
static void HideUi() {
    ShowWindow(g_w[WK_MAIN].h, SW_HIDE);
    g_visible = false;
    UpdateActivity();
}
static bool PickColor(HWND owner, int& rgb) {
    static COLORREF cust[16];
    CHOOSECOLORW cc = { sizeof cc };
    cc.hwndOwner = owner; cc.rgbResult = RGB(rgb >> 16, (rgb >> 8) & 255, rgb & 255); cc.lpCustColors = cust; cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    rgb = (GetRValue(cc.rgbResult) << 16) | (GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
    return true;
}

// ======================================================================= overlay (always on top, per-pixel alpha)
static const wchar_t* kOvClass = L"LlanoFanSyncOverlay";
static bool g_ovSizing = false, g_ovMoving = false;
static POINT g_ovPt; static RECT g_ovMoveStart; static int g_ovStartW, g_ovStartH;

static ChartTheme OvTheme(float k) {
    DWORD c = (DWORD)g_s.ovTextColor;
    return { Rgb(c), Rgb(c, 185), Rgb(c, 38), Rgb(c, 90), Color(230, 30, 34, 40), Rgb(0xF3F4F6), g_s.ovOutline != 0, k };
}
static std::vector<int> OvIds() { std::vector<int> v; for (int i = 0; i < H_COUNT; i++) if (((g_s.ovMask >> i) & 1) && SeriesShown(i)) v.push_back(i); return v; }
// numbers block (measure only when draw = false)
static SizeF OvNumbers(Graphics& g, Fonts& F, float k, REAL x0, REAL y0, bool draw, const ChartTheme& th) {
    std::vector<int> ids = OvIds();
    if (ids.empty()) return SizeF(0, 0);
    const HSample* last = LastSample();
    REAL rowH = 19 * k, dot = 8 * k;
    std::vector<std::wstring> vals;
    for (int i : ids) { wchar_t v[32]; FmtVal(v, last ? last->v[i] : NAN, kSeries[i].unit); vals.push_back(v); }
    if (g_s.ovLayout == 0) {
        REAL nw = 0, vw = 0;
        for (size_t i = 0; i < ids.size(); i++) { nw = std::max(nw, TextW(g, kSeries[ids[i]].shortName, F.n)); vw = std::max(vw, TextW(g, vals[i].c_str(), F.b)); }
        REAL W = dot + 6 * k + nw + 8 * k + vw;
        if (draw)
            for (size_t i = 0; i < ids.size(); i++) {
                REAL y = y0 + i * rowH;
                SolidBrush d(Rgb(kSeries[ids[i]].col)); g.FillEllipse(&d, x0, y + 6 * k, dot, dot);
                HTxt(g, kSeries[ids[i]].shortName, F.n, th.sub, x0 + dot + 5 * k, y + 1 * k, th);
                HTxt(g, vals[i].c_str(), F.b, th.ink, x0 + W - vw, y + 1 * k, th, vw, StringAlignmentFar);
            }
        return SizeF(W, rowH * ids.size());
    }
    REAL x = x0;
    for (size_t i = 0; i < ids.size(); i++) {
        REAL tn = TextW(g, kSeries[ids[i]].shortName, F.n), tv = TextW(g, vals[i].c_str(), F.b);
        if (draw) {
            SolidBrush d(Rgb(kSeries[ids[i]].col)); g.FillEllipse(&d, x, y0 + 6 * k, dot, dot);
            HTxt(g, kSeries[ids[i]].shortName, F.n, th.sub, x + dot + 4 * k, y0 + 1 * k, th);
            HTxt(g, vals[i].c_str(), F.b, th.ink, x + dot + 2 * k + tn, y0 + 1 * k, th);
        }
        x += dot + 4 * k + tn + tv + 12 * k;
    }
    return SizeF(x - x0 - 12 * k, rowH);
}
static SizeF OvLegend(Graphics& g, Fonts& F, float k, REAL x0, REAL y0, REAL maxW, bool draw, const ChartTheme& th) {
    std::vector<int> ids = OvIds();
    REAL x = x0, y = y0, rowH = 17 * k, dot = 7 * k, wMax = 0;
    for (int i : ids) {
        REAL iw = dot + 4 * k + TextW(g, kSeries[i].shortName, F.s) + 8 * k;
        if (x > x0 && x + iw > x0 + maxW) { x = x0; y += rowH; }
        if (draw) {
            SolidBrush d(Rgb(kSeries[i].col)); g.FillEllipse(&d, x, y + 5 * k, dot, dot);
            HTxt(g, kSeries[i].shortName, F.s, th.sub, x + dot + 3 * k, y, th);
        }
        x += iw; wMax = std::max(wMax, x - x0);
    }
    return ids.empty() ? SizeF(0, 0) : SizeF(wMax, y - y0 + rowH);
}
static void OvRender(bool force) {
    if (!g_ov || (!force && !IsWindowVisible(g_ov))) return;
    int dpi = GetDpiForWindow(g_ov);
    float dk = dpi / 96.f, k = dk * kOvScale[g_s.ovSize];
    Fonts F(k);
    ChartTheme th = OvTheme(k);
    bool chart = g_s.ovMode != 0, nums = g_s.ovMode != 1, none = g_s.ovMask == 0;
    REAL pad = 8 * k, chW = g_s.ovW * dk, chH = g_s.ovH * dk;
    SizeF top(0, 0);
    {
        Bitmap mb(1, 1, PixelFormat32bppPARGB); Graphics mg(&mb);
        if (nums) top = OvNumbers(mg, F, k, 0, 0, false, th);
        else top = OvLegend(mg, F, k, 0, 0, chW, false, th);
    }
    REAL cw = chart ? std::max(chW, top.Width) : top.Width;
    REAL W = cw + 2 * pad, H = top.Height + (chart ? (top.Height > 0 ? 6 * k : 0) + chH : 0) + 2 * pad;
    if (none) { W = 230 * dk; H = 34 * dk; }
    int w = std::max(8, (int)std::ceil(W)), h = std::max(8, (int)std::ceil(H));
    RECT cur; GetWindowRect(g_ov, &cur);
    int x = cur.left, y = cur.top;
    if (!g_ovMoving) {
        if (g_s.ovAnchor == 0 && g_s.ovX != kNoPos) { x = g_s.ovX; y = g_s.ovY; }
        else {
            int a = g_s.ovAnchor ? g_s.ovAnchor : 3;
            MONITORINFO mi = { sizeof mi }; GetMonitorInfoW(MonitorFromWindow(g_ov, MONITOR_DEFAULTTOPRIMARY), &mi);
            RECT wa = mi.rcWork; int m = (int)(12 * dk), c = (a - 1) % 3, r = (a - 1) / 3;
            x = c == 0 ? wa.left + m : c == 1 ? (wa.left + wa.right - w) / 2 : wa.right - w - m;
            y = r == 0 ? wa.top + m : r == 1 ? (wa.top + wa.bottom - h) / 2 : wa.bottom - h - m;
        }
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib) {
        HGDIOBJ old = SelectObject(mdc, dib);
        {
            Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
            Graphics g(&bmp);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
            g.Clear(Color(0, 0, 0, 0));
            bool locked = g_s.ovLocked != 0;
            GraphicsPath p; RoundPath(p, RectF(0.5f, 0.5f, w - 1.f, h - 1.f), 8 * dk);
            BYTE bgA = g_s.ovBg ? (BYTE)(g_s.ovBgOpacity * 255 / 100) : 0;
            if (!locked && bgA < 1) bgA = 1;          // invisible, but keeps the whole overlay grabbable
            if (bgA) { SolidBrush bg(Rgb((DWORD)g_s.ovBgColor, bgA)); g.FillPath(&bg, &p); }
            if (!locked) {                            // unlocked: show its edge (and a resize grip for the chart)
                Pen bd(Rgb((DWORD)g_s.ovTextColor, 120), 1 * dk); bd.SetDashStyle(DashStyleDash); g.DrawPath(&bd, &p);
                if (chart) {
                    SolidBrush gb(Rgb((DWORD)g_s.ovTextColor, 200));
                    for (int i = 0; i < 3; i++)
                        for (int j = 0; j < 3 - i; j++) g.FillRectangle(&gb, w - (6 + 4 * i) * dk, h - (6 + 4 * j) * dk, 2 * dk, 2 * dk);
                }
            }
            if (none) HTxt(g, L"No sensors ticked — open Overlay settings", F.s, th.sub, 0, (h - 15 * dk) / 2, th, (REAL)w, StringAlignmentCenter);
            else {
                REAL yy = pad;
                if (nums) OvNumbers(g, F, k, pad, yy, true, th); else OvLegend(g, F, k, pad, yy, cw, true, th);
                yy += top.Height + (top.Height > 0 ? 6 * k : 0);
                if (chart) DrawChart(g, nullptr, RectF(pad, yy, cw, chH), g_s.ovMask, g_s.ovRange, th, F);
            }
        }
        POINT src = { 0, 0 }, dst = { x, y }; SIZE sz = { w, h };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(g_s.ovOpacity * 255 / 100), AC_SRC_ALPHA };
        UpdateLayeredWindow(g_ov, sdc, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
        SelectObject(mdc, old); DeleteObject(dib);
    }
    DeleteDC(mdc); ReleaseDC(nullptr, sdc);
}
static LRESULT CALLBACK OvProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: {           // body = drag to move, bottom-right corner = resize the chart; locked = clicks go through
        if (g_s.ovLocked) return HTTRANSPARENT;
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) }; RECT r; GetWindowRect(h, &r);
        int gs = MulDiv(18, GetDpiForWindow(h), 96);
        return (g_s.ovMode != 0 && p.x >= r.right - gs && p.y >= r.bottom - gs) ? HTCLIENT : HTCAPTION;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE)); return TRUE; }
        if (LOWORD(l) == HTCAPTION) { SetCursor(LoadCursorW(nullptr, IDC_SIZEALL)); return TRUE; }
        break;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: g_ovSizing = true; GetCursorPos(&g_ovPt); g_ovStartW = g_s.ovW; g_ovStartH = g_s.ovH; SetCapture(h); return 0;
    case WM_MOUSEMOVE:
        if (g_ovSizing) {
            POINT p; GetCursorPos(&p); float dk = GetDpiForWindow(h) / 96.f;
            g_s.ovW = std::clamp(g_ovStartW + (int)((p.x - g_ovPt.x) / dk), 160, 1600);
            g_s.ovH = std::clamp(g_ovStartH + (int)((p.y - g_ovPt.y) / dk), 60, 1000);
            OvRender();
        }
        return 0;
    case WM_LBUTTONUP: if (g_ovSizing) { g_ovSizing = false; ReleaseCapture(); Save(); InvalidateAll(); } return 0;
    case WM_CAPTURECHANGED: if (g_ovSizing) { g_ovSizing = false; Save(); } return 0;
    case WM_ENTERSIZEMOVE: g_ovMoving = true; GetWindowRect(h, &g_ovMoveStart); return 0;
    case WM_EXITSIZEMOVE: {
        g_ovMoving = false;
        RECT r; GetWindowRect(h, &r);
        if (r.left != g_ovMoveStart.left || r.top != g_ovMoveStart.top) { g_s.ovAnchor = 0; g_s.ovX = r.left; g_s.ovY = r.top; Save(); InvalidateAll(); }
        return 0;
    }
    case WM_NCRBUTTONUP: case WM_RBUTTONUP: PostMessageW(g_w[WK_MAIN].h, WM_APP_OVMENU, 0, 0); return 0;
    case WM_NCRBUTTONDOWN: case WM_RBUTTONDOWN: return 0;
    case WM_DPICHANGED: case WM_DISPLAYCHANGE: OvRender(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void OvApply() {            // create / show / hide / lock the overlay to match the settings
    if (g_s.ovOn && !g_ov) {
        EnsureGdip();
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = OvProc; wc.hInstance = g_inst; wc.lpszClassName = kOvClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        bool free = g_s.ovAnchor == 0 && g_s.ovX != kNoPos && MonitorFromPoint({ g_s.ovX + 20, g_s.ovY + 20 }, MONITOR_DEFAULTTONULL);
        if (g_s.ovAnchor == 0 && !free) g_s.ovAnchor = 3;
        g_ov = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kOvClass, L"Llano Fan Sync overlay", WS_POPUP,
                               free ? g_s.ovX : 0, free ? g_s.ovY : 0, 10, 10, nullptr, nullptr, g_inst, nullptr);
    }
    if (g_ov) {
        LONG_PTR ex = GetWindowLongPtrW(g_ov, GWL_EXSTYLE), nex = g_s.ovLocked ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
        if (nex != ex) SetWindowLongPtrW(g_ov, GWL_EXSTYLE, nex);
        if (g_s.ovOn) {
            OvRender(true);
            if (!IsWindowVisible(g_ov)) ShowWindow(g_ov, SW_SHOWNOACTIVATE);
            SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (IsWindowVisible(g_ov)) ShowWindow(g_ov, SW_HIDE);
    }
    UpdateActivity();
}

// ======================================================================= curve editing (content px = client + scroll)
static bool HitPoint(const Win& w, int mx, int my, int& gi, int& pi) {
    for (int g = 0; g < 3; g++) {
        RECT pr; if (!CurvePanelPx(w, g, pr)) continue;
        RECT pl = PlotOf(pr); Curve& c = CurveOf(g);
        for (int i = 0; i < (int)c.pts.size(); i++) {
            float dx = PX(pl, c.pts[i].first) - mx, dy = PY(pl, c.pts[i].second) - my;
            if (dx * dx + dy * dy <= Sf(9) * Sf(9)) { gi = g; pi = i; return true; }
        }
    }
    return false;
}
static int PlotAt(const Win& w, int mx, int my) {
    for (int g = 0; g < 3; g++) {
        RECT pr; if (!CurvePanelPx(w, g, pr)) continue;
        RECT pl = PlotOf(pr); InflateRect(&pl, S(6), S(6)); POINT p = { mx, my };
        if (PtInRect(&pl, p)) return g;
    }
    return -1;
}
static void ToVal(const Win& w, int g, int mx, int my, double& x, double& y) {
    RECT pr; CurvePanelPx(w, g, pr); RECT pl = PlotOf(pr);
    x = std::clamp(std::round((mx - pl.left) * 100.0 / (pl.right - pl.left)), 0.0, 100.0);
    y = std::clamp(std::round((pl.bottom - my) * 100.0 / (pl.bottom - pl.top)), 0.0, 100.0);
}
static const Hot* HotAt(const Win& w, int x, int y) {
    for (size_t i = w.hots.size(); i-- > 0;) { POINT p = { x, y }; if (PtInRect(&w.hots[i].r, p)) return &w.hots[i]; }
    return nullptr;
}

// ======================================================================= actions
static void ShowMenu(POINT pt) {
    HMENU mn = CreatePopupMenu();
    AppendMenuW(mn, MF_STRING, MENU_OPEN, L"Open Llano Fan Sync");
    AppendMenuW(mn, MF_STRING | (g_paused ? MF_CHECKED : 0), MENU_PAUSE, L"Pause syncing");
    if (g_wheelHold && !g_paused) AppendMenuW(mn, MF_STRING, MENU_TAKEBACK, L"Take back control from the pad's wheel");
    AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mn, MF_STRING | (g_s.ovOn ? MF_CHECKED : 0), MENU_OVERLAY, L"Show overlay");
    AppendMenuW(mn, MF_STRING | (g_s.ovLocked ? MF_CHECKED : 0) | (g_s.ovOn ? 0 : MF_GRAYED), MENU_OVLOCK, L"Lock overlay in place (click-through)");
    AppendMenuW(mn, MF_STRING, MENU_OVSET, L"Overlay settings…");
    AppendMenuW(mn, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(mn, MF_STRING, MENU_EXIT, L"Exit");
    SetMenuDefaultItem(mn, MENU_OPEN, FALSE);
    SetForegroundWindow(g_w[WK_MAIN].h);
    int cmd = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_w[WK_MAIN].h, nullptr);
    DestroyMenu(mn);
    switch (cmd) {
    case MENU_OPEN: ShowUi(); break;
    case MENU_PAUSE: SetPaused(!g_paused); Tick(); break;
    case MENU_TAKEBACK: SetPaused(false); Tick(); break;
    case MENU_OVERLAY: g_s.ovOn = !g_s.ovOn; Save(); OvApply(); break;
    case MENU_OVLOCK: g_s.ovLocked = !g_s.ovLocked; Save(); OvApply(); break;
    case MENU_OVSET: OpenWin(WK_OVSET); break;
    case MENU_EXIT: DestroyWindow(g_w[WK_MAIN].h); break;
    }
    InvalidateAll();
}
static void DoHot(Win& w, const Hot& ht, int mx) {
    int a = ht.arg;
    switch (ht.act) {
    case A_TOGGLE:
        switch (a) {
        case T_OVERLAY: g_s.ovOn = !g_s.ovOn; Save(); OvApply(); break;
        case T_OVLOCK: g_s.ovLocked = !g_s.ovLocked; Save(); OvApply(); break;
        case T_STARTUP: SetStartup(!GetStartup()); g_startup = GetStartup(); break;
        case T_PAUSE: SetPaused(!g_paused); Tick(); break;
        case T_RECORD: g_s.recordHidden = !g_s.recordHidden; Save(); UpdateActivity(); break;
        case T_OVBG: g_s.ovBg = !g_s.ovBg; Save(); OvRender(); break;
        case T_OVOUTLINE: g_s.ovOutline = !g_s.ovOutline; Save(); OvRender(); break;
        }
        break;
    case A_BUTTON:
        switch (a) {
        case B_OVSET: OpenWin(WK_OVSET); break;
        case B_TAKEBACK: SetPaused(false); Tick(); break;
        case B_PICK: w.pick = !w.pick; break;
        case B_HIDE: HideUi(); break;
        case B_CLOSE: DestroyWindow(w.h); return;
        case B_CHARTNONE: g_s.chartMask = 0; Save(); break;
        case B_OVNONE: g_s.ovMask = 0; Save(); OvRender(); break;
        case B_TXTCUSTOM: if (PickColor(w.h, g_s.ovTextColor)) { Save(); OvRender(); } break;
        case B_BGCUSTOM: if (PickColor(w.h, g_s.ovBgColor)) { Save(); OvRender(); } break;
        case B_RESET:
            if (MessageBoxW(w.h, L"Reset all three curves to the original values?", L"Llano Fan Sync", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                g_s.gpu.Parse(kGpuDefault); g_s.cpu.Parse(kCpuDefault); g_s.sys.Parse(kSysDefault); Save(); Tick();
            }
            break;
        }
        break;
    case A_SEG: {
        int grp = a / 100, i = a % 100;
        switch (grp) {
        case G_RANGE: g_s.chartRange = i; break;
        case G_INTERVAL: g_s.intervalMs = kIntervals[i]; SetTimer(g_w[WK_MAIN].h, 1, g_s.intervalMs, nullptr); break;
        case G_THEME: g_s.theme = i; ApplyTheme(); break;
        case G_OVMODE: g_s.ovMode = i; break;
        case G_OVRANGE: g_s.ovRange = i; break;
        case G_OVLAYOUT: g_s.ovLayout = i; break;
        case G_OVSIZE: g_s.ovSize = i; break;
        }
        Save(); OvRender();
        break;
    }
    case A_CHIP: g_s.chartMask ^= 1ull << a; Save(); break;
    case A_OVCHIP: g_s.ovMask ^= 1ull << a; Save(); OvRender(); break;
    case A_SLIDER: {
        int lo, hi; int* ref = SliderRef(a, lo, hi);
        *ref = SliderValue(ht.r, mx, lo, hi);
        w.capAct = A_SLIDER; w.capArg = a; w.capR = ht.r; SetCapture(w.h);
        OvRender();
        break;
    }
    case A_EXPAND: OpenWin(a == 3 ? WK_ZHIST : WK_ZGPU + a); break;
    case A_SWATCH: (a / 100 == 0 ? g_s.ovTextColor : g_s.ovBgColor) = (int)(a / 100 == 0 ? kTxtSw : kBgSw)[a % 100]; Save(); OvRender(); break;
    case A_ANCHOR: g_s.ovAnchor = a; Save(); OvRender(); break;
    }
    InvalidateAll();
}

// ======================================================================= window proc (main window and all other windows)
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM l) {
    if (m == WM_NCCREATE) {
        Win* nw = (Win*)((CREATESTRUCTW*)l)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)nw);
        nw->h = h; nw->dpi = GetDpiForWindow(h);
        if (nw->kind == WK_MAIN) g_wnd = h;
    }
    Win* pw = (Win*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!pw) return DefWindowProcW(h, m, wp, l);
    Win& w = *pw;
    g_dpi = w.dpi;
    bool isMain = w.kind == WK_MAIN;
    if (isMain && m == WM_TASKBARCREATED && m) { AddTray(); return 0; }
    switch (m) {
    case WM_TIMER: if (isMain) Tick(); return 0;
    case WM_TRAY:
        switch (LOWORD(l)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONDBLCLK: ShowUi(); break;
        case WM_CONTEXTMENU: ShowMenu({ GET_X_LPARAM(wp), GET_Y_LPARAM(wp) }); break;
        }
        return 0;
    case WM_APP_OVMENU: { POINT p; GetCursorPos(&p); ShowMenu(p); return 0; }
    case WM_APP_SHOW: ShowUi(); return 0;
    case WM_APP_RELAYOUT: Layout(w); return 0;
    case WM_SETTINGCHANGE:
        if (isMain && l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet") && g_s.theme == 0) ApplyTheme();
        break;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) { if (isMain) HideUi(); else DestroyWindow(h); } return 0;
    case WM_HSCROLL: OnScroll(w, SB_HORZ, LOWORD(wp)); return 0;
    case WM_VSCROLL: OnScroll(w, SB_VERT, LOWORD(wp)); return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) ScrollTo(w, SB_HORZ, w.sx - MulDiv(d, S(60), WHEEL_DELTA));
        else ScrollTo(w, SB_VERT, w.sy - MulDiv(d, S(60), WHEEL_DELTA));
        return 0;
    }
    case WM_MOUSEHWHEEL: ScrollTo(w, SB_HORZ, w.sx + MulDiv(GET_WHEEL_DELTA_WPARAM(wp), S(60), WHEEL_DELTA)); return 0;
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(l) + w.sx, my = GET_Y_LPARAM(l) + w.sy, gi, pi;
        SetFocus(h);
        if (const Hot* ht = HotAt(w, mx, my)) { Hot copy = *ht; DoHot(w, copy, mx); return 0; }
        if (HitPoint(w, mx, my, gi, pi)) { w.dragG = gi; w.dragI = pi; SetCapture(h); InvalidateAll(); return 0; }
        int g = PlotAt(w, mx, my); if (g < 0) return 0;
        Curve& c = CurveOf(g);
        double x, y; ToVal(w, g, mx, my, x, y);
        x = std::max(1.0, x);
        for (auto& p : c.pts) if (p.first == x) return 0;
        c.pts.push_back({ x, y }); std::sort(c.pts.begin(), c.pts.end());
        for (int i = 0; i < (int)c.pts.size(); i++) if (c.pts[i].first == x) { w.dragG = g; w.dragI = i; }
        SetCapture(h); InvalidateAll();
        return 0;
    }
    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(l) + w.sx, my = GET_Y_LPARAM(l) + w.sy;
        if (w.capAct == A_SLIDER) {
            int lo, hi; int* ref = SliderRef(w.capArg, lo, hi);
            int v = SliderValue(w.capR, mx, lo, hi);
            if (v != *ref) { *ref = v; OvRender(); InvalidateRect(h, nullptr, FALSE); }
            return 0;
        }
        if (w.dragG >= 0) {
            Curve& c = CurveOf(w.dragG);
            double x, y; ToVal(w, w.dragG, mx, my, x, y);
            int n = (int)c.pts.size();
            double lo = w.dragI > 0 ? c.pts[w.dragI - 1].first + 1 : 1, hi = w.dragI < n - 1 ? c.pts[w.dragI + 1].first - 1 : 100;
            c.pts[w.dragI] = { std::clamp(x, lo, std::max(lo, hi)), y };
            InvalidateAll();
            return 0;
        }
        bool changed = false;
        int gi = -1, pi = -1; HitPoint(w, mx, my, gi, pi);
        if (gi != w.hovG || pi != w.hovI) { w.hovG = gi; w.hovI = pi; changed = true; }
        const Hot* ht = HotAt(w, mx, my);
        int ha = ht ? ht->act : 0, hg = ht ? ht->arg : -1;
        if (ha != w.hotAct || hg != w.hotArg) { w.hotAct = ha; w.hotArg = hg; changed = true; }
        POINT p = { mx, my };
        int cx = (!ht && PtInRect(&w.histPlot, p)) ? mx : -1;
        if (cx != w.crossX) { w.crossX = cx; changed = true; }
        if (changed) InvalidateRect(h, nullptr, FALSE);
        TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 }; TrackMouseEvent(&te);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (w.hovG >= 0 || w.hotAct || w.crossX >= 0) { w.hovG = w.hovI = -1; w.hotAct = 0; w.hotArg = -1; w.crossX = -1; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (w.capAct == A_SLIDER) { w.capAct = 0; ReleaseCapture(); Save(); OvRender(); InvalidateAll(); }
        if (w.dragG >= 0) { w.dragG = w.dragI = -1; ReleaseCapture(); Save(); Tick(); }
        return 0;
    case WM_CAPTURECHANGED:
        if (w.capAct == A_SLIDER) { w.capAct = 0; Save(); }
        return 0;
    case WM_RBUTTONUP: {
        int gi, pi;
        if (HitPoint(w, GET_X_LPARAM(l) + w.sx, GET_Y_LPARAM(l) + w.sy, gi, pi)) {
            Curve& c = CurveOf(gi);
            if (c.pts.size() > 2) { c.pts.erase(c.pts.begin() + pi); w.hovG = w.hovI = -1; Save(); Tick(); }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p); p.x += w.sx; p.y += w.sy; int gi, pi;
            if (w.dragG >= 0 || HotAt(w, p.x, p.y) || HitPoint(w, p.x, p.y, gi, pi)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
            if (PlotAt(w, p.x, p.y) >= 0 || PtInRect(&w.histPlot, p)) { SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE; }
        }
        break;
    case WM_SIZE: if (wp != SIZE_MINIMIZED) Layout(w); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)l)->ptMinTrackSize = { MulDiv(360, w.dpi, 96), MulDiv(240, w.dpi, 96) }; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); if (g_gdip) PaintWin(w, dc); EndPaint(h, &ps); return 0; }
    case WM_DPICHANGED: {
        w.dpi = HIWORD(wp); g_dpi = w.dpi;
        RECT* r = (RECT*)l; SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(w);
        return 0;
    }
    case WM_CLOSE: if (isMain) HideUi(); else DestroyWindow(h); return 0;
    case WM_DESTROY:
        if (isMain) {
            for (int k = 1; k < WK_COUNT; k++) if (g_w[k].h) DestroyWindow(g_w[k].h);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_ov) DestroyWindow(g_ov);
            g_mon.Stop(); g_fps.Stop();
            Log(L"exit");
            PostQuitMessage(0);
        } else {
            w.h = nullptr; w.hots.clear(); w.dragG = w.hovG = -1; w.capAct = 0; w.crossX = -1;
            UpdateActivity();
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    g_inst = inst;
    CreateMutexW(nullptr, TRUE, L"Local\\LlanoFanSync");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {           // already running: just open its window
        HWND w = FindWindowW(kClass, nullptr);
        if (w) { AllowSetForegroundWindow(ASFW_ANY); PostMessageW(w, WM_APP_SHOW, 0, 0); }
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    GetModuleFileNameW(nullptr, g_exe, MAX_PATH);
    wchar_t dir[MAX_PATH]; wcscpy(dir, g_exe); *wcsrchr(dir, L'\\') = 0;
    swprintf(g_ini, MAX_PATH, L"%s\\LlanoFanSync.ini", dir);
    swprintf(g_log, MAX_PATH, L"%s\\LlanoFanSync.log", dir);
    MigrateStartupShortcut();
    EnumDisks();
    LoadSettings(g_s); g_iniT = IniTime();
    g_paused = GetPrivateProfileIntW(L"sync", L"paused", 0, g_ini) != 0;
    P = (g_s.theme == 2 || (g_s.theme == 0 && SystemDark())) ? &kDark : &kLight;

    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    wc.lpszClassName = kWinClass; RegisterClassExW(&wc);

    Win& mw = g_w[WK_MAIN];
    mw.kind = WK_MAIN; mw.minW = MainMinW; mw.minH = MainMinH;
    CreateWindowExW(0, kClass, L"Llano Fan Sync", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, inst, &mw);
    ThemeWindow(mw.h);
    {
        RECT r = { 0, 0, MulDiv(1120, mw.dpi, 96), MulDiv(960, mw.dpi, 96) };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, mw.dpi);
        CenterOnWork(mw.h, r.right - r.left, r.bottom - r.top);
    }
    Layout(mw);

    Log(L"started");
    Tick();
    AddTray();
    SetTimer(mw.h, 1, g_s.intervalMs, nullptr);
    OvApply();
    bool background = cmd && wcsstr(cmd, L"/background");
    if (!background) ShowUi(); else UpdateActivity();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_gdip) GdiplusShutdown(g_gdip);
    return 0;
}
