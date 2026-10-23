// Soru Auto X/Z/C - external, standalone (C++ port of soru_autox.ahk)
// Fires only on your own Soru: your position jumps (teleport) AND a "Soru"
// sound (ProxySound) spawns near you, within 0.5s of each other, gated by a
// recent R keypress. The sound is "Soru", or "DodgeQuick" in Human V4 (its
// flash-step effect doesn't play "Soru"). Then sends the current mode's key (X, Z or C) to
// replicate the follow-up combo input. Other teleport moves don't count.
// The mode key cycles the mode: OFF -> X -> Z -> C -> V -> OFF. It is Caps Lock
// by default; a small window asks for it the very first time only, and it can
// be changed later from the tray menu. The same window turns each mode on or
// off (an off mode is skipped by the cycle). Both are saved in settings.cfg,
// next to the offsets cache. The key itself is swallowed, so it never does its normal thing (Caps
// Lock never actually turns on). No console: an AHK-style tooltip near the
// cursor on each mode change, and a tray icon (hover = mode + key + status,
// right-click = keybind / Exit). Launching it again replaces the running
// copy, like AHK's #SingleInstance Force.
// Build (meow.rc mete meow.ico como icono del .exe y de la bandeja;
// /MANIFEST:EMBED mete el manifest de Common Controls v6 adentro del .exe,
// sin eso queda en un .manifest aparte y el tooltip sale sin estilo):
//   rc meow.rc
//   cl /EHsc /std:c++17 /O2 SoruAutoX.cpp meow.res /Fe:meow.exe /link /MANIFEST:EMBED

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <timeapi.h>
#include <winhttp.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")
// Common Controls v6: el tooltip con el estilo actual de Windows (igual que AHK)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
#include <iostream>
#include <cmath>
#include <chrono>
#include <thread>
#include <optional>
#include <future>
#include <climits>
#include <memory>
// GDI+ (formas con bordes suaves en la ventana de teclas). Sus headers usan
// min/max, que NOMINMAX saco.
using std::min;
using std::max;
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include "json.hpp"
using json = nlohmann::json;

// ------------------------------------------------------------
// Offsets: SIN valores por defecto. Se cargan en caliente desde
// offsets.imtheo.lol (o del cache en disco si es del mismo build que el
// Roblox que esta corriendo, ver LoadOffsets). Si la carga falla, OFF se
// queda vacio y g_offsetsLoaded en false: el programa NO intenta atacharse
// a Roblox hasta tener offsets reales, nunca usa numeros inventados.
// ------------------------------------------------------------
struct Offsets {
    uintptr_t FDM_Pointer        = 0;
    uintptr_t FDM_RealDM         = 0;
    uintptr_t InstanceName       = 0;
    uintptr_t ChildrenStart      = 0;
    uintptr_t ChildrenEnd        = 0;
    uintptr_t LocalPlayer        = 0;
    uintptr_t ModelInstance      = 0;
    uintptr_t BasePart_Primitive = 0;
    uintptr_t Primitive_Position = 0;
    uintptr_t DM_Workspace       = 0; // opcional: DataModel->Workspace directo, solo para diagnostico
};
static Offsets OFF;
static bool g_offsetsLoaded = false;
static std::wstring g_offsetsVersion; // build de Roblox para el que son los offsets cargados

constexpr double MATCH_RADIUS = 50.0;
constexpr double TELEPORT_JUMP_MIN = 8.0;      // studs entre polls consecutivos para contar como "se teleporto" (un Soru corto de V4 mide ~12; igual hace falta el sonido y la R)
// Estos tres son los valores por defecto; la pestana Timing los cambia (ver Opts / g_rWindowMs...)
constexpr DWORD SORU_MATCH_WINDOW_MS = 500;    // tu teleport y el sonido "Soru" tienen que pasar con menos de esto entre uno y otro
constexpr DWORD R_WINDOW_MS = 800;
constexpr DWORD TRIGGER_COOLDOWN_MS = 400;
constexpr DWORD HISTORY_MS = 2000;
constexpr int POLL_MS = 1;
constexpr DWORD STALE_REATTACH_MS = 3000;      // si GetMyPosition falla sostenido esto, re-attach (cambio de server)
constexpr DWORD PENDING_TIMEOUT_MS = 250;
constexpr DWORD OFFSETS_RECHECK_MS = 30000;    // si el cliente y los offsets no coinciden, cada cuanto re-consultar el site
constexpr DWORD V4_COOLDOWN_MAX_MS = 150;      // Human V4 da 3 cargas de Soru seguidas (~300ms entre una y otra): el cooldown no puede pasar de esto o se come las cargas 2 y 3
constexpr DWORD SOUND_DEDUPE_MS = 100;         // despues de disparar, se ignoran los sonidos que lleguen (V4 suena 2 veces por Soru) para que no queden colgados como "sonido Soru" del proximo teleport
constexpr DWORD SORU_VK = 'R';                 // tecla de Soru en el juego
constexpr DWORD DEFAULT_MODE_VK = VK_CAPITAL;  // tecla por defecto que cicla el modo (el hook se la come); se cambia desde la ventana de la primera vez o la bandeja
constexpr UINT TIP_MS = 700;                   // tooltip de cambio de modo
constexpr UINT STATUS_TIP_MS = 1500;           // tooltip de "arranco" / "conectado"
constexpr uint64_t MAX_CHILDREN = 65536;       // mas que esto = lectura basura

// Logs de diagnostico por poll / por hijo nuevo. Apagados: escribir a la
// consola de Windows es lento y metia latencia justo en el camino critico.
// En true, ademas se abre una consola para verlos (normalmente no hay).
constexpr bool VERBOSE = false;
#define DBG(expr) do { if constexpr (VERBOSE) { std::cout << expr; } } while (0)

// ------------------------------------------------------------
// Modos. 0 = OFF. Solo lo escribe el thread principal (CycleMode, desde el
// hook de la tecla de modo) y lo lee el thread de deteccion.
// ------------------------------------------------------------
static const char MODE_ORDER[] = { 0, 'X', 'Z', 'C', 'V' };
static const char MODE_LETTERS[] = "XZCV"; // bit i de g_modeMask = MODE_LETTERS[i]; en ese orden salen en la ventana
constexpr unsigned ALL_MODES = 0xF;
static std::atomic<char> g_mode{ 0 };
// Que modos entran en el ciclo; la ventana de teclas los prende y apaga. OFF siempre esta.
static std::atomic<unsigned> g_modeMask{ ALL_MODES };
// Ajustes de la ventana (pestanas Tooltips, Timing y General). Solo los toca el thread principal.
constexpr int ACCENT_COUNT = 5;
constexpr int TIP_STYLES = 4;   // Classic, Pill, HUD, Keycap
constexpr int IND_CHOICES = 5;  // indicador: apagado + las 4 esquinas
struct Range { int lo, hi, step, def; };
constexpr Range RANGE_TIP_MS   = { 300, 2000, 100, 700 };
constexpr Range RANGE_R_MS     = { 200, 2000, 50, (int)R_WINDOW_MS };
constexpr Range RANGE_MATCH_MS = { 100, 1500, 50, (int)SORU_MATCH_WINDOW_MS };
constexpr Range RANGE_COOL_MS  = { 100, 1500, 50, (int)TRIGGER_COOLDOWN_MS };
struct Opts {
    int tipStyle = 0;      // como se muestra el modo al cambiarlo: 0 Classic (el tooltip de siempre), 1 Pill, 2 HUD, 3 Keycap
    int tipMs = RANGE_TIP_MS.def;             // cuanto dura ese tooltip en pantalla
    int indicator = 0;     // indicador siempre visible mientras Roblox tiene el foco: 0 apagado, 1 arriba-izq, 2 arriba-der, 3 abajo-izq, 4 abajo-der
    int rWindowMs = RANGE_R_MS.def;           // cuanto despues de apretar R sigue contando un Soru
    int matchMs = RANGE_MATCH_MS.def;         // maximo entre tu teleport y el sonido "Soru"
    int cooldownMs = RANGE_COOL_MS.def;       // minimo entre dos teclas enviadas
    bool statusTip = true; // tooltips de "running" / "Connected"
    bool rememberPos = false; // la ventana se abre donde la dejaste
    int accent = 0;        // color de la ventana de teclas (indice en KB_ACCENTS)
};
static Opts g_opts;
static int g_winX = INT_MIN, g_winY = INT_MIN; // posicion recordada de la ventana, INT_MIN = ninguna
// Los tiempos de la deteccion: los escribe el thread principal (ApplySettings) y los lee el de deteccion
static std::atomic<int> g_rWindowMs{ RANGE_R_MS.def }, g_matchMs{ RANGE_MATCH_MS.def }, g_cooldownMs{ RANGE_COOL_MS.def };
// Contadores de la sesion para la pestana Stats; los escribe el thread de deteccion
struct Stats {
    std::atomic<unsigned> detected{ 0 };          // veces que se detecto tu Soru (teleport + sonido + R)
    std::atomic<unsigned> sent[4] = { {0}, {0}, {0}, {0} }; // teclas enviadas por modo, en el orden de MODE_LETTERS
    std::atomic<unsigned> ignoredOff{ 0 };        // detectado con el modo en OFF
    std::atomic<unsigned> ignoredFocus{ 0 };      // detectado con Roblox sin foco
    std::atomic<DWORD> lastTick{ 0 };             // GetTickCount del ultimo envio, 0 = ninguno
    std::atomic<char> lastMode{ 0 };
    std::atomic<int> lastGapMs{ 0 };              // sonido Soru - teleport, del ultimo envio
    std::atomic<DWORD> start{ 0 };                // cuando arranco la sesion (o se reseteo)
};
static Stats g_stats;
static void ResetStats() {
    g_stats.detected = 0;
    for (auto& s : g_stats.sent) s = 0;
    g_stats.ignoredOff = 0;
    g_stats.ignoredFocus = 0;
    g_stats.lastTick = 0;
    g_stats.lastMode = 0;
    g_stats.lastGapMs = 0;
    g_stats.start = GetTickCount();
}
// Tecla que cicla el modo. Se escribe en el thread principal (ventana de
// teclas) y la lee el hook, que corre en ese mismo thread.
static std::atomic<DWORD> g_modeVk{ DEFAULT_MODE_VK };
static bool g_keyConfigured = false; // false = primera vez, hay que preguntar la tecla

static std::string ModeName(char m) { return m ? std::string(1, m) : std::string("OFF"); }
static bool ModeEnabled(char m, unsigned mask) {
    for (int i = 0; i < 4; i++)
        if (MODE_LETTERS[i] == m) return (mask >> i) & 1;
    return false;
}
// "OFF -> X -> Z -> C -> V", solo con los modos prendidos
static std::wstring CycleText() {
    std::wstring s = L"OFF";
    for (int i = 0; i < 4; i++)
        if ((g_modeMask.load() >> i) & 1) { s += L" -> "; s += (wchar_t)MODE_LETTERS[i]; }
    return s;
}

// Compartidos entre el hook de teclado (thread principal) y la deteccion
static std::atomic<DWORD> g_robloxPid{ 0 };  // pid conectado, 0 = ninguno
static std::atomic<DWORD> g_lastRPress{ 0 }; // GetTickCount de la ultima R con Roblox en foco

// ------------------------------------------------------------
// String helpers
// ------------------------------------------------------------
static std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s += (c < 128) ? (char)c : '?';
    return s;
}
static std::wstring TrimW(std::wstring s) {
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t start = s.find_first_not_of(L" \t\r\n");
    return start == std::wstring::npos ? L"" : s.substr(start);
}
// "version-ABC..." y "abc..." cuentan como la misma version
static std::wstring NormVersion(std::wstring v) {
    v = TrimW(v);
    for (auto& c : v) c = (wchar_t)towlower(c);
    if (v.rfind(L"version-", 0) == 0) v = v.substr(8);
    return v;
}
static bool SameVersion(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && NormVersion(a) == NormVersion(b);
}

// ------------------------------------------------------------
// Estado visible (no hay consola): el icono de la bandeja lo muestra al
// pasar el mouse, y algunos cambios salen tambien como tooltip
// ------------------------------------------------------------
constexpr UINT WM_APP_CYCLE_MODE = WM_APP + 1; // del hook, al apretar la tecla de modo
constexpr UINT WM_APP_STATUS     = WM_APP + 2; // del thread de deteccion; wParam 1 = tambien como tooltip (si esta prendido), 2 = tooltip siempre
constexpr UINT WM_APP_TRAY       = WM_APP + 3; // callback del icono de la bandeja
constexpr UINT WM_APP_KEY_CAPTURED = WM_APP + 4; // del hook a la ventana de teclas; wParam = vkCode
constexpr UINT WM_APP_REFRESH = WM_APP + 5;      // a la ventana de teclas: cambio el modo o el estado, repintar
constexpr UINT WM_APP_FIRED = WM_APP + 6;        // del thread de deteccion: se detecto tu Soru (cambiaron los Stats)

static HWND g_mainHwnd = nullptr; // ventana oculta del thread principal, nunca se muestra
static HWND g_kbHwnd = nullptr;   // ventana para elegir la tecla de modo, nullptr = cerrada
static std::mutex g_statusMutex;
static std::wstring g_status = L"Waiting for Roblox";

static void SetStatus(const std::wstring& s, int popup = 0) {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status = s;
    }
    std::cout << Narrow(s) << "\n";
    if (g_mainHwnd) PostMessageW(g_mainHwnd, WM_APP_STATUS, popup, 0);
}
static std::wstring GetStatus() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

// ------------------------------------------------------------
// Memory helpers
// ------------------------------------------------------------
static bool RPM(HANDLE hProc, uintptr_t addr, void* buf, size_t size) {
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(hProc, (LPCVOID)addr, buf, size, &bytesRead) && bytesRead == size;
}
static uint64_t ReadPtr(HANDLE hProc, uintptr_t addr) {
    uint64_t v = 0;
    if (!RPM(hProc, addr, &v, sizeof(v))) return 0;
    return v;
}
static bool IsPlausiblePtr(uint64_t v) {
    return v > 0x10000 && v < 0x7FFFFFFFFFFFULL;
}
static std::string ReadRbxString(HANDLE hProc, uint64_t addr) {
    if (!IsPlausiblePtr(addr)) return "";
    uint8_t raw[32] = { 0 };
    if (!RPM(hProc, addr, raw, sizeof(raw))) return "";
    uint64_t strLen, capacity;
    memcpy(&strLen, raw + 16, 8);
    memcpy(&capacity, raw + 24, 8);
    if (strLen == 0 || strLen > 128) return "";
    auto extractAscii = [](const uint8_t* p, size_t maxLen) -> std::string {
        std::string out;
        for (size_t i = 0; i < maxLen; i++) {
            uint8_t ch = p[i];
            if (ch == 0 || ch < 32 || ch > 126) break;
            out += (char)ch;
        }
        return out;
    };
    if (capacity <= 15) return extractAscii(raw, (size_t)std::min<uint64_t>(strLen, 15));
    if (strLen > capacity || capacity > 4096) return "";
    uint64_t strPtr;
    memcpy(&strPtr, raw, 8);
    if (!IsPlausiblePtr(strPtr)) return "";
    size_t readLen = (size_t)std::min<uint64_t>(strLen, 127);
    std::vector<uint8_t> buf(readLen);
    if (!RPM(hProc, strPtr, buf.data(), readLen)) return "";
    return extractAscii(buf.data(), readLen);
}
static std::string ReadName(HANDLE hProc, uint64_t instance) {
    // instance+InstanceName es un puntero al rbx::string del nombre. Hasta
    // Roblox 0.743 habia 8 bytes de header antes del string (el nombre
    // empezaba en namePtr+8, su length en namePtr+24). En 0.744 el header
    // desaparecio: "Workspace" esta en namePtr+0 y su length (9) en
    // namePtr+16 (el dump lo dice como Instance::Name = 0). Confirmado con un
    // volcado crudo de memoria. Se prueba +0 primero y +8 como respaldo, asi
    // sigue andando si Roblox vuelve al layout viejo. Sin esto TODOS los
    // nombres salian vacios: no se encontraba Players/Workspace, y como
    // DetectChildStride cuenta hijos con nombre, tampoco detectaba el paso 0x10.
    uint64_t namePtr = ReadPtr(hProc, instance + OFF.InstanceName);
    if (!namePtr) return "";
    std::string nm = ReadRbxString(hProc, namePtr);
    return nm.empty() ? ReadRbxString(hProc, namePtr + 8) : nm;
}

// Los hijos de una Instance viven en un std::vector apuntado desde
// instance+ChildrenStart. El paso entre entradas se detecta al atachar
// (DetectChildStride): 8 si son punteros sueltos, 0x10 si son shared_ptr
// (puntero + bloque de control).
static uint64_t g_childStride = 8;

// false = lectura rara (mejor ignorar este poll); true con count 0 = sin hijos
static bool ReadChildRange(HANDLE hProc, uint64_t parent, uint64_t& start, uint64_t& count) {
    start = 0;
    count = 0;
    uint64_t vecPtr = 0;
    if (!RPM(hProc, parent + OFF.ChildrenStart, &vecPtr, sizeof(vecPtr))) return false;
    if (!vecPtr) return true; // sin vector = sin hijos
    uint64_t s = 0, e = 0;
    if (!RPM(hProc, vecPtr, &s, sizeof(s)) || !RPM(hProc, vecPtr + OFF.ChildrenEnd, &e, sizeof(e))) return false;
    if (!s && !e) return true; // vector vacio sin buffer
    if (!s || e < s || (e - s) / g_childStride > MAX_CHILDREN) return false;
    start = s;
    count = (e - s) / g_childStride;
    return true;
}
static std::vector<uint64_t> GetChildren(HANDLE hProc, uint64_t parent) {
    std::vector<uint64_t> kids;
    uint64_t start, count;
    if (!ReadChildRange(hProc, parent, start, count) || count == 0) return kids;
    // todo el vector en una sola lectura, en vez de un RPM por hijo
    size_t perEntry = (size_t)(g_childStride / 8);
    std::vector<uint64_t> raw((size_t)count * perEntry);
    if (!RPM(hProc, start, raw.data(), raw.size() * sizeof(uint64_t))) return kids;
    for (size_t i = 0; i < raw.size(); i += perEntry)
        if (raw[i]) kids.push_back(raw[i]);
    return kids;
}
static uint64_t FindChild(HANDLE hProc, uint64_t parent, const std::string& targetName) {
    for (uint64_t c : GetChildren(hProc, parent))
        if (ReadName(hProc, c) == targetName) return c;
    return 0;
}
// Sonido que acompana a tu Soru. Normal: el hijo se llama "Soru" (efecto Shared.Soru).
// Human V4 (Race.A >= 1): el juego no usa ese efecto sino RaceAwakenings.Human, que
// suena "DodgeQuick" dos veces (en el origen y en el destino) y nunca "Soru".
enum SoruSound { SOUND_NONE = 0, SOUND_SORU, SOUND_V4 };
static SoruSound FindSoruSound(HANDLE hProc, uint64_t part) {
    for (uint64_t c : GetChildren(hProc, part)) {
        std::string nm = ReadName(hProc, c);
        if (nm == "Soru") return SOUND_SORU;
        if (nm == "DodgeQuick") return SOUND_V4;
    }
    return SOUND_NONE;
}
static bool GetInstancePosition(HANDLE hProc, uint64_t instance, float& x, float& y, float& z) {
    uint64_t primAddr = ReadPtr(hProc, instance + OFF.BasePart_Primitive);
    if (!primAddr) return false;
    float pos[3];
    if (!RPM(hProc, primAddr + OFF.Primitive_Position, pos, sizeof(pos))) return false;
    x = pos[0]; y = pos[1]; z = pos[2];
    return true;
}

// Leyendo los hijos del DataModel de a 8 bytes: si solo las posiciones pares
// tienen nombre legible, las impares son bloques de control de shared_ptr y
// el paso real es 0x10. Si todas tienen nombre, son punteros sueltos (8).
static uint64_t DetectChildStride(HANDLE hProc, uint64_t dataModel) {
    uint64_t vecPtr = ReadPtr(hProc, dataModel + OFF.ChildrenStart);
    uint64_t s = vecPtr ? ReadPtr(hProc, vecPtr) : 0;
    uint64_t e = vecPtr ? ReadPtr(hProc, vecPtr + OFF.ChildrenEnd) : 0;
    if (!s || e <= s) return 8;
    uint64_t n = std::min<uint64_t>((e - s) / 8, 128);
    int namedEven = 0, namedOdd = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t c = ReadPtr(hProc, s + i * 8);
        if (c && !ReadName(hProc, c).empty()) ++(i % 2 ? namedOdd : namedEven);
    }
    return (namedEven >= 4 && namedOdd * 4 < namedEven) ? 0x10 : 8;
}

// Watcher incremental: solo lee los hijos agregados desde el ultimo poll.
// Se guia por la CANTIDAD de hijos, no por la direccion del buffer: cuando
// el vector crece (o Roblox lo reemplaza entero) los hijos viejos quedan
// copiados en el mismo orden al principio, asi que lo nuevo es todo lo que
// este despues de la cantidad anterior. Antes un cambio de buffer reseteaba
// el baseline y se perdia justo el hijo que lo habia provocado.
struct WatchRoot {
    std::string label;
    uint64_t addr = 0;
    uint64_t count = 0;
    bool init = false;
};
static std::vector<uint64_t> GetNewChildrenTail(HANDLE hProc, WatchRoot& root) {
    std::vector<uint64_t> kids;
    uint64_t start, count;
    if (!ReadChildRange(hProc, root.addr, start, count)) return kids;
    if (root.init) {
        for (uint64_t i = root.count; i < count; i++) {
            uint64_t child = ReadPtr(hProc, start + i * g_childStride);
            if (child) kids.push_back(child);
        }
    }
    root.init = true;
    root.count = count;
    return kids;
}

// ------------------------------------------------------------
// Process enumeration / module base
// ------------------------------------------------------------
// outTotalSeen (opcional): cuantos procesos se pudieron listar en total, para
// distinguir "no pude ni listar procesos" (bloqueado) de "los liste todos y
// Roblox genuinamente no esta entre ellos".
static std::vector<DWORD> EnumPIDsByName(const wchar_t* name, int* outTotalSeen = nullptr) {
    std::vector<DWORD> pids;
    if (outTotalSeen) *outTotalSeen = -1; // -1 = ni pudimos abrir el snapshot
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return pids;
    if (outTotalSeen) *outTotalSeen = 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (outTotalSeen) (*outTotalSeen)++;
            if (_wcsicmp(pe.szExeFile, name) == 0) pids.push_back(pe.th32ProcessID);
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pids;
}
static uint64_t GetModuleBase(DWORD pid, const wchar_t* moduleName) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me = { sizeof(me) };
    uint64_t base = 0;
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName) == 0) { base = (uint64_t)me.modBaseAddr; break; }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
    return base;
}
struct EnumWinCtx { DWORD pid; bool found; };
static BOOL CALLBACK HasWindowProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = (EnumWinCtx*)lparam;
    DWORD winPid = 0;
    GetWindowThreadProcessId(hwnd, &winPid);
    if (winPid == ctx->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        ctx->found = true;
        return FALSE;
    }
    return TRUE;
}
static bool HasWindow(DWORD pid) {
    EnumWinCtx ctx{ pid, false };
    EnumWindows(HasWindowProc, (LPARAM)&ctx);
    return ctx.found;
}
static bool RobloxIsForeground(DWORD pid) {
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    return fgPid == pid;
}

// "version-XXXXXXXX" sacado del path del .exe de Roblox que esta corriendo
static std::wstring GetRobloxVersionString(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(h, 0, path, &size);
    CloseHandle(h);
    if (!ok) return L"";
    std::wstring p(path);
    size_t pos = p.find(L"version-");
    if (pos == std::wstring::npos) return L"";
    size_t end = p.find(L'\\', pos);
    if (end == std::wstring::npos) end = p.size();
    return p.substr(pos, end - pos);
}

// ------------------------------------------------------------
// HTTP
// ------------------------------------------------------------
static bool HttpGetText(const std::wstring& host, const std::wstring& path, std::string& outBody) {
    // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY dispara deteccion de proxy (WPAD),
    // que puede tardar varios segundos. Conexion directa: mucho mas rapido.
    HINTERNET hSession = WinHttpOpen(L"SoruAutoX/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    bool ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(hRequest, nullptr);

    if (ok) {
        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) ok = false;
    }

    if (ok) {
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
            if (avail == 0) break;
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
            outBody.append(buf.data(), read);
        } while (avail > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok && !outBody.empty();
}

// WinHTTP puede colgarse resolviendo DNS mas alla de los timeouts que le
// pasamos (es una limitacion conocida). Para no trabar el thread de
// deteccion, corremos el fetch en un thread aparte con un limite duro: si no
// contesta a tiempo, seguimos sin esperarlo (el thread se abandona solo).
static bool HttpGetTextBounded(const std::wstring& host, const std::wstring& path, std::string& outBody, int timeoutMs) {
    auto promise = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    std::future<std::pair<bool, std::string>> fut = promise->get_future();

    std::thread([host, path, promise]() {
        std::string body;
        bool ok = HttpGetText(host, path, body);
        promise->set_value({ ok, std::move(body) });
    }).detach();

    if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
        return false; // timeout duro: seguimos, el thread de fondo se descarta solo

    auto result = fut.get();
    outBody = std::move(result.second);
    return result.first;
}

// ------------------------------------------------------------
// Offsets: parseo
// ------------------------------------------------------------
// offsets.json tiene forma: { "Roblox Version": "version-...", "Offsets": { "ClassName": { "Field": 123, ... }, ... } }
static std::optional<uintptr_t> GetOffsetField(const json& data, const char* cls, const char* field) {
    auto offsets = data.find("Offsets");
    if (offsets == data.end() || !offsets->is_object()) return std::nullopt;
    auto entry = offsets->find(cls);
    if (entry == offsets->end() || !entry->is_object()) return std::nullopt;
    auto value = entry->find(field);
    if (value == entry->end()) return std::nullopt;
    if (value->is_number_integer()) return value->get<uintptr_t>();
    if (value->is_string()) { // por si algun dump los trae como "0x1A8"
        const std::string s = value->get<std::string>();
        try {
            size_t used = 0;
            uintptr_t v = (uintptr_t)std::stoull(s, &used, 0);
            if (used == s.size()) return v;
        } catch (...) {}
    }
    return std::nullopt;
}

// Parsea el offsets.json (de red o de cache en disco) y llena "out" solo si
// TODOS los campos que necesitamos aparecen. outVersion = el "Roblox
// Version" que declara el propio dump (vacio si no lo trae).
static bool ParseOffsetsJson(const std::string& body, Offsets& out, std::wstring& outVersion) {
    try {
        json data = json::parse(body);

        Offsets fresh;
        bool okAll = true;
        auto need = [&](const char* cls, const char* field, uintptr_t& dst) {
            auto v = GetOffsetField(data, cls, field);
            if (!v) { okAll = false; std::cout << "  falta campo: " << cls << "::" << field << "\n"; return; }
            dst = *v;
        };
        need("FakeDataModel", "Pointer", fresh.FDM_Pointer);
        need("FakeDataModel", "RealDataModel", fresh.FDM_RealDM);
        need("Instance", "NameContainer", fresh.InstanceName);
        need("Instance", "ChildrenStart", fresh.ChildrenStart);
        need("Instance", "ChildrenEnd", fresh.ChildrenEnd);
        need("Player", "LocalPlayer", fresh.LocalPlayer);
        need("Player", "ModelInstance", fresh.ModelInstance);
        need("BasePart", "Primitive", fresh.BasePart_Primitive);
        need("Primitive", "Position", fresh.Primitive_Position);

        // opcional (no rompe la carga si el dump no lo trae): solo para el
        // diagnostico de "dataModel valido pero Name mal leido" vs "dataModel invalido"
        if (auto v = GetOffsetField(data, "DataModel", "Workspace")) fresh.DM_Workspace = *v;

        if (!okAll) return false;

        outVersion.clear();
        auto ver = data.find("Roblox Version");
        if (ver != data.end() && ver->is_string()) {
            std::string v = ver->get<std::string>();
            outVersion.assign(v.begin(), v.end());
        }
        out = fresh;
        return true;
    } catch (const std::exception& e) {
        // no solo JSON mal formado: tambien un campo con un tipo inesperado
        std::cout << "  offsets.json invalido: " << e.what() << "\n";
        return false;
    }
}

// ------------------------------------------------------------
// Cache local (offsets.json + offsets.version). La carpeta NO tiene un
// nombre fijo: se deriva del ID de esta PC.
// ------------------------------------------------------------
static std::wstring GetMachineId() {
    wchar_t buf[64] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", L"MachineGuid",
        RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD nameLen = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(name, &nameLen);
    return name;
}
static std::wstring HashToHex(const std::wstring& s) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a de 64 bits
    for (wchar_t c : s) { h ^= (uint64_t)c; h *= 1099511628211ULL; }
    wchar_t hex[17];
    swprintf(hex, 17, L"%016llx", h);
    return hex;
}
static std::wstring GetCacheDir() {
    wchar_t* localAppData = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData) != S_OK || !localAppData)
        return L"";
    std::wstring base = localAppData;
    CoTaskMemFree(localAppData);
    return base + L"\\" + HashToHex(GetMachineId());
}
static std::wstring VersionCachePath() {
    std::wstring dir = GetCacheDir();
    return dir.empty() ? L"" : dir + L"\\offsets.version";
}
static std::wstring OffsetsCachePath() {
    std::wstring dir = GetCacheDir();
    return dir.empty() ? L"" : dir + L"\\offsets.json";
}
static bool ReadFileAllW(const std::wstring& path, std::string& outBody) {
    if (path.empty()) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr);
    if (size == INVALID_FILE_SIZE) { CloseHandle(f); return false; }
    outBody.resize(size);
    DWORD read = 0;
    bool ok = ReadFile(f, outBody.data(), size, &read, nullptr) && read == size;
    CloseHandle(f);
    return ok;
}
static void WriteFileAllW(const std::wstring& path, const std::string& body) {
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, body.data(), (DWORD)body.size(), &written, nullptr);
    CloseHandle(f);
}

// ------------------------------------------------------------
// Tecla de modo: validacion, nombre y guardado (settings.cfg en la misma
// carpeta del cache). Si el archivo no existe o no tiene una tecla valida es
// la primera vez y se muestra la ventana para elegirla.
// ------------------------------------------------------------
static bool IsModifierVk(DWORD vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    }
    return false;
}
// Como el hook se come la tecla en todo el sistema, nada de modificadores (se
// romperia shift-lock, atajos, etc.), ni la tecla de Soru, ni click izquierdo
// o derecho (el hook de teclado no los ve, pero un settings.cfg editado a mano
// podria traerlos y se comerian el mouse)
static bool IsAllowedModeKey(DWORD vk) {
    return vk > 0 && vk < 0xFF && vk != VK_PACKET && vk != SORU_VK && !IsModifierVk(vk)
        && vk != VK_LBUTTON && vk != VK_RBUTTON;
}
static std::wstring KeyName(DWORD vk) {
    if (vk == VK_PAUSE) return L"Pause";
    if (vk == VK_NUMLOCK) return L"Num Lock";
    if (vk == VK_SNAPSHOT) return L"Print Screen"; // GetKeyNameText la devuelve como "Sys Req"
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX); // 0xE0xx = tecla extendida
    LONG lp = (LONG)((sc & 0xFF) << 16);
    if ((sc & 0xFF00) == 0xE000 || (sc & 0xFF00) == 0xE100) lp |= 1 << 24;
    wchar_t buf[64] = {};
    if ((sc & 0xFF) && GetKeyNameTextW(lp, buf, 64) > 0) return buf;
    wchar_t hex[16];
    swprintf(hex, 16, L"VK 0x%02X", (unsigned)vk);
    return hex;
}
static std::wstring SettingsPath() {
    std::wstring dir = GetCacheDir();
    return dir.empty() ? L"" : dir + L"\\settings.cfg";
}
// Teclas con las que se escribe. El hook se come la tecla de modo en todo el
// sistema, asi que si es una de estas deja de escribir en las demas apps.
static bool IsTypingKey(DWORD vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return true;
    switch (vk) {
    case VK_SPACE: case VK_RETURN: case VK_TAB: case VK_BACK: case VK_ESCAPE:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_OEM_1: case VK_OEM_2: case VK_OEM_3: case VK_OEM_4: case VK_OEM_5: case VK_OEM_6: case VK_OEM_7:
    case VK_OEM_PLUS: case VK_OEM_COMMA: case VK_OEM_MINUS: case VK_OEM_PERIOD:
        return true;
    }
    return false;
}

// settings.cfg: una linea "Clave=valor" por ajuste. ModeKey=<vk>, Modes=<letras prendidas, ej XZV>,
// TipStyle=<0..3>, TipMs, Indicator=<0..4>, RWindow, MatchWindow, Cooldown (en ms), StatusTip y
// RememberPos = 0 o 1, Accent=<indice>, WinX / WinY (posicion de la ventana, solo con RememberPos).
// Las lineas que no conoce (ej el "ModeTip=" o "Sound=" de una version vieja) se ignoran, y un valor
// fuera de rango vuelve al por defecto.
static std::string SerializeSettings(DWORD vk, unsigned mask, const Opts& o) {
    std::string modes;
    for (int i = 0; i < 4; i++)
        if ((mask >> i) & 1) modes += MODE_LETTERS[i];
    std::string s = "ModeKey=" + std::to_string(vk) + "\nModes=" + modes +
        "\nTipStyle=" + std::to_string(o.tipStyle) + "\nTipMs=" + std::to_string(o.tipMs) +
        "\nIndicator=" + std::to_string(o.indicator) + "\nRWindow=" + std::to_string(o.rWindowMs) +
        "\nMatchWindow=" + std::to_string(o.matchMs) + "\nCooldown=" + std::to_string(o.cooldownMs) +
        "\nStatusTip=" + (o.statusTip ? "1" : "0") + "\nRememberPos=" + (o.rememberPos ? "1" : "0") +
        "\nAccent=" + std::to_string(o.accent) + "\n";
    if (o.rememberPos && g_winX != INT_MIN && g_winY != INT_MIN)
        s += "WinX=" + std::to_string(g_winX) + "\nWinY=" + std::to_string(g_winY) + "\n";
    return s;
}
static bool OptsEqual(const Opts& a, const Opts& b) {
    return a.tipStyle == b.tipStyle && a.tipMs == b.tipMs && a.indicator == b.indicator && a.rWindowMs == b.rWindowMs &&
           a.matchMs == b.matchMs && a.cooldownMs == b.cooldownMs && a.statusTip == b.statusTip &&
           a.rememberPos == b.rememberPos && a.accent == b.accent;
}
// "Clave=<numero>" -> v; false si falta o no es un numero
static bool ParseInt(const std::string& body, const char* key, long& v) {
    size_t p = body.find(key);
    if (p == std::string::npos) return false;
    const char* s = body.c_str() + p + strlen(key);
    char* end = nullptr;
    long x = strtol(s, &end, 10);
    if (end == s) return false;
    v = x;
    return true;
}
// el valor de "Clave=" si esta dentro del rango, si no el por defecto
static int ParseRanged(const std::string& body, const char* key, const Range& r) {
    long v = 0;
    return ParseInt(body, key, v) && v >= r.lo && v <= r.hi ? (int)v : r.def;
}
// "Clave=0/1" -> bool; si falta o no es 0/1 queda def
static bool ParseFlag(const std::string& body, const char* key, bool def) {
    size_t p = body.find(key);
    if (p == std::string::npos) return def;
    char c = p + strlen(key) < body.size() ? body[p + strlen(key)] : 0;
    return c == '1' ? true : c == '0' ? false : def;
}
// false = no hay una tecla valida (primera vez). Todo lo demas es opcional, asi que un
// settings.cfg viejo (solo ModeKey) sigue andando: todos los modos prendidos y los
// ajustes por defecto.
static bool ParseSettings(const std::string& body, DWORD& vk, unsigned& mask, Opts& o) {
    size_t p = body.find("ModeKey=");
    if (p == std::string::npos) return false;
    DWORD k = (DWORD)strtoul(body.c_str() + p + 8, nullptr, 10);
    if (!IsAllowedModeKey(k)) return false;
    vk = k;
    mask = ALL_MODES;
    size_t m = body.find("Modes=");
    if (m != std::string::npos) {
        unsigned parsed = 0;
        for (size_t i = m + 6; i < body.size() && body[i] != '\r' && body[i] != '\n'; i++)
            for (int b = 0; b < 4; b++)
                if (body[i] == MODE_LETTERS[b]) parsed |= 1u << b;
        if (parsed) mask = parsed;
    }
    o = Opts();
    o.statusTip = ParseFlag(body, "StatusTip=", o.statusTip);
    o.rememberPos = ParseFlag(body, "RememberPos=", o.rememberPos);
    long v = 0;
    if (ParseInt(body, "TipStyle=", v) && v >= 0 && v < TIP_STYLES) o.tipStyle = (int)v;
    if (ParseInt(body, "Indicator=", v) && v >= 0 && v < IND_CHOICES) o.indicator = (int)v;
    if (ParseInt(body, "Accent=", v) && v >= 0 && v < ACCENT_COUNT) o.accent = (int)v;
    o.tipMs = ParseRanged(body, "TipMs=", RANGE_TIP_MS);
    o.rWindowMs = ParseRanged(body, "RWindow=", RANGE_R_MS);
    o.matchMs = ParseRanged(body, "MatchWindow=", RANGE_MATCH_MS);
    o.cooldownMs = ParseRanged(body, "Cooldown=", RANGE_COOL_MS);
    return true;
}
// la posicion de la ventana guardada (solo si hay las dos); no es parte de Opts
static void ParseWindowPos(const std::string& body, int& x, int& y) {
    long vx = 0, vy = 0;
    if (ParseInt(body, "WinX=", vx) && ParseInt(body, "WinY=", vy)) { x = (int)vx; y = (int)vy; }
    else { x = INT_MIN; y = INT_MIN; }
}
// los tiempos de la deteccion viven en atomics porque los lee otro thread
static void SyncTiming(const Opts& o) {
    g_rWindowMs.store(o.rWindowMs);
    g_matchMs.store(o.matchMs);
    g_cooldownMs.store(o.cooldownMs);
}
// true = habia una tecla guardada y valida (quedan en g_modeVk / g_modeMask / g_opts)
static bool LoadSettings() {
    std::string body;
    DWORD vk = 0;
    unsigned mask = ALL_MODES;
    Opts o;
    if (!ReadFileAllW(SettingsPath(), body) || !ParseSettings(body, vk, mask, o)) return false;
    g_modeVk.store(vk);
    g_modeMask.store(mask);
    g_opts = o;
    SyncTiming(o);
    ParseWindowPos(body, g_winX, g_winY);
    return true;
}
static void SaveSettings() {
    std::wstring dir = GetCacheDir();
    if (dir.empty()) return;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    WriteFileAllW(SettingsPath(), SerializeSettings(g_modeVk.load(), g_modeMask.load(), g_opts));
}

// ------------------------------------------------------------
// Offsets: carga
// ------------------------------------------------------------
// GET /roblox/version: la version LIVE actual segun offsets.imtheo.lol
// (texto plano, ej "version-abc123...").
static std::wstring GetLiveVersion() {
    std::string body;
    if (!HttpGetTextBounded(L"offsets.imtheo.lol", L"/roblox/version", body, 8000)) return L"";
    std::wstring w(body.begin(), body.end());
    return TrimW(w);
}

static void ApplyOffsets(const Offsets& fresh, const std::wstring& version, const char* source) {
    OFF = fresh;
    g_offsetsLoaded = true;
    g_offsetsVersion = version;
    std::cout << "  offsets OK (" << source << ", " << (version.empty() ? "version desconocida" : Narrow(version)) << ")\n";
}

// Carga offsets para el Roblox que esta corriendo (clientVersion, vacio si
// no se pudo leer del path del .exe):
//  1. si el cache en disco es de ese mismo build, se usa sin tocar la red
//     (anda offline, y no son offsets viejos: son los de ese build exacto)
//  2. si no, metodo oficial del site: GET /roblox/version; si el cache es de
//     esa version se usa, si no se baja GET /offsets.json y se pisa el cache
// Si lo que hay en el site no coincide con el cliente, se carga igual (a
// veces el site o el cliente van un poco adelantados) pero se avisa, y
// EnsureOffsetsFor lo vuelve a intentar cada OFFSETS_RECHECK_MS.
static bool LoadOffsets(const std::wstring& clientVersion) {
    std::wstring dir = GetCacheDir();
    if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);

    std::string cachedBody, cachedVerA;
    Offsets cached;
    std::wstring cachedVer;
    bool haveCache = ReadFileAllW(OffsetsCachePath(), cachedBody) && ParseOffsetsJson(cachedBody, cached, cachedVer);
    if (haveCache && cachedVer.empty() && ReadFileAllW(VersionCachePath(), cachedVerA))
        cachedVer = TrimW(std::wstring(cachedVerA.begin(), cachedVerA.end()));

    if (haveCache && SameVersion(cachedVer, clientVersion)) {
        ApplyOffsets(cached, cachedVer, "cache");
        return true;
    }

    std::wstring liveVersion = GetLiveVersion();
    if (liveVersion.empty()) {
        std::cout << "  no se pudo consultar /roblox/version (sin internet?)\n";
        return false;
    }

    Offsets fresh;
    std::wstring freshVer;
    bool fromCache = haveCache && SameVersion(cachedVer, liveVersion);
    if (fromCache) {
        fresh = cached;
        freshVer = cachedVer;
    } else {
        std::cout << "Bajando offsets.json (version live: " << Narrow(liveVersion) << ")...\n";
        std::string body;
        if (!HttpGetTextBounded(L"offsets.imtheo.lol", L"/offsets.json", body, 8000)) {
            std::cout << "  no se pudo bajar offsets.json\n";
            return false;
        }
        if (!ParseOffsetsJson(body, fresh, freshVer)) return false;
        if (freshVer.empty()) freshVer = liveVersion;
        if (!dir.empty()) {
            WriteFileAllW(OffsetsCachePath(), body);
            WriteFileAllW(VersionCachePath(), Narrow(freshVer));
            SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        }
    }
    ApplyOffsets(fresh, freshVer, fromCache ? "cache" : "descargados");

    if (!clientVersion.empty() && !SameVersion(freshVer, clientVersion))
        std::cout << "  AVISO: los offsets son para " << Narrow(freshVer) << " pero tu Roblox es "
                  << Narrow(clientVersion) << ". Si no conecta, reinicia Roblox (o espera a que el site se actualice).\n";
    return true;
}

// Se llama en cada intento de attach con la version del cliente que esta
// corriendo. Si los offsets cargados ya son de esa version no hace nada. Si
// no, recarga: enseguida si el cliente cambio de version (Roblox se
// actualizo mientras esperabamos), o cada OFFSETS_RECHECK_MS si sigue sin
// coincidir. Antes solo se chequeaba una vez por reconexion, y si Roblox se
// actualizaba en el medio se quedaba intentando con offsets viejos para siempre.
static bool EnsureOffsetsFor(const std::wstring& clientVersion) {
    static std::wstring lastCheckedClient;
    static DWORD lastCheck = 0;
    if (g_offsetsLoaded && SameVersion(g_offsetsVersion, clientVersion)) return true;
    DWORD now = GetTickCount();
    if (g_offsetsLoaded && clientVersion == lastCheckedClient && now - lastCheck < OFFSETS_RECHECK_MS)
        return true; // mismos offsets de antes (best-effort) hasta el proximo re-chequeo
    lastCheckedClient = clientVersion;
    lastCheck = now;
    LoadOffsets(clientVersion);
    return g_offsetsLoaded;
}

// ------------------------------------------------------------
// Attach
// ------------------------------------------------------------
struct AttachState {
    bool ok = false;
    HANDLE hProcess = nullptr;
    DWORD pid = 0;
    std::wstring version;
    uint64_t players = 0;
    std::vector<WatchRoot> roots;
};

// En que paso se quedo el ultimo intento fallido de conexion (se imprime
// cuando cambia, para no spamear la consola cada segundo)
static std::wstring g_lastAttachError = L"not tried yet";

static AttachState AttachRoblox() {
    AttachState r;
    int totalSeen = -1;
    std::vector<DWORD> candidates = EnumPIDsByName(L"RobloxPlayerBeta.exe", &totalSeen);
    if (candidates.empty()) {
        g_lastAttachError = totalSeen < 0
            ? L"process snapshot failed entirely (blocked by AV/security software?)"
            : L"RobloxPlayerBeta.exe not running (or blocked from seeing it)";
        return r;
    }

    std::vector<DWORD> withWindow, withoutWindow;
    for (DWORD pid : candidates) {
        if (HasWindow(pid)) withWindow.push_back(pid); else withoutWindow.push_back(pid);
    }
    std::vector<DWORD>& ordered = !withWindow.empty() ? withWindow : withoutWindow;

    for (DWORD pid : ordered) {
        std::wstring clientVersion = GetRobloxVersionString(pid);
        if (!EnsureOffsetsFor(clientVersion)) {
            g_lastAttachError = L"offsets not loaded (no internet, and no cached offsets for " +
                (clientVersion.empty() ? std::wstring(L"this Roblox version") : clientVersion) + L")";
            return r;
        }

        HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (!h) { g_lastAttachError = L"OpenProcess denied for pid " + std::to_wstring(pid) + L" (permissions / blocked by antivirus?)"; continue; }

        uint64_t modBase = GetModuleBase(pid, L"RobloxPlayerBeta.exe");
        if (!modBase) { g_lastAttachError = L"couldn't read module base (pid " + std::to_wstring(pid) + L")"; CloseHandle(h); continue; }

        uint64_t fakeDM = ReadPtr(h, modBase + OFF.FDM_Pointer);
        uint64_t dataModel = fakeDM ? ReadPtr(h, fakeDM + OFF.FDM_RealDM) : 0;
        if (!dataModel) { g_lastAttachError = L"couldn't resolve DataModel (offsets likely stale for this Roblox version)"; CloseHandle(h); continue; }

        g_childStride = DetectChildStride(h, dataModel);
        DBG("[DBG] paso de hijos: 0x" << std::hex << g_childStride << std::dec << "\n");

        uint64_t players = FindChild(h, dataModel, "Players");
        uint64_t workspace = FindChild(h, dataModel, "Workspace");
        if (!players || !workspace) {
            // diagnostico: si dataModel es basura (puntero "plausible" pero no
            // real), sus hijos van a ser 0 o nombres ilegibles; si es el
            // DataModel real, deberiamos ver nombres normales pero sin
            // Players/Workspace entre ellos (mas raro, offsets de children mal).
            auto kids = GetChildren(h, dataModel);
            std::wstring names;
            int shown = 0;
            for (uint64_t c : kids) {
                if (shown >= 6) break;
                std::string nm = ReadName(h, c);
                if (!names.empty()) names += L", ";
                names += nm.empty() ? L"?" : std::wstring(nm.begin(), nm.end());
                shown++;
            }
            wchar_t hexBuf[32];
            swprintf(hexBuf, 32, L"0x%llx", (unsigned long long)dataModel);
            std::wstring diag = L"couldn't find Players/Workspace. dataModel=" + std::wstring(hexBuf) +
                L", " + std::to_wstring(kids.size()) + L" children: [" + names + L"]";

            // prueba quirurgica: si tenemos DataModel::Workspace, leemos el
            // Workspace DIRECTO (sin buscarlo por nombre) y le pedimos su
            // propio nombre. Si dice literal "Workspace", dataModel es valido
            // de verdad y el problema es 100% la lectura de nombres. Si sale
            // basura tambien, dataModel mismo esta mal.
            if (OFF.DM_Workspace) {
                uint64_t wsDirect = ReadPtr(h, dataModel + OFF.DM_Workspace);
                std::string wsName = wsDirect ? ReadName(h, wsDirect) : "";
                wchar_t wsHex[32];
                swprintf(wsHex, 32, L"0x%llx", (unsigned long long)wsDirect);
                diag += L" | Workspace directo=" + std::wstring(wsHex) + L" nombre='" +
                    (wsName.empty() ? std::wstring(L"?") : std::wstring(wsName.begin(), wsName.end())) + L"'";
            }

            g_lastAttachError = diag;
            CloseHandle(h); continue;
        }

        uint64_t worldOrigin = FindChild(h, workspace, "_WorldOrigin");
        if (!worldOrigin) { g_lastAttachError = L"couldn't find _WorldOrigin (offsets likely stale, or not in a game yet)"; CloseHandle(h); continue; }

        g_lastAttachError = L"OK";

        r.ok = true;
        r.hProcess = h;
        r.pid = pid;
        r.version = clientVersion;
        r.players = players;

        // Nota: "ProxySound" NO es una carpeta fija, Roblox la crea de nuevo como
        // hijo de "Sounds" cada vez que suena algo (igual que "EffectsFolder" bajo
        // _WorldOrigin) y se destruye despues. Por eso no se busca aqui: se detecta
        // como hijo nuevo del root "Sounds" en el loop principal.
        std::vector<std::pair<std::string, uint64_t>> rootDefs = {
            {"_WorldOrigin", worldOrigin},
            {"InteractiveEffects", FindChild(h, worldOrigin, "InteractiveEffects")},
            {"Sounds", FindChild(h, worldOrigin, "Sounds")},
            {"Workspace", workspace},
        };
        for (auto& def : rootDefs) {
            if (!def.second) {
                DBG("[DBG] root '" << def.first << "' NO encontrada\n");
                continue;
            }
            DBG("[DBG] root '" << def.first << "' = 0x" << std::hex << def.second << std::dec << "\n");
            WatchRoot wr;
            wr.label = def.first;
            wr.addr = def.second;
            GetNewChildrenTail(h, wr); // baseline init
            r.roots.push_back(wr);
        }
        return r;
    }
    return r;
}

// Reintenta hasta conectar. Solo actualiza el estado cuando cambia el motivo
// del fallo. announce = avisar con un tooltip cuando conecta.
static AttachState WaitForAttach(bool announce) {
    std::wstring lastShown;
    for (;;) {
        AttachState a = AttachRoblox();
        if (a.ok) {
            DBG("[DBG] pid " << a.pid << ", roots vigiladas: " << a.roots.size() << "\n");
            SetStatus(L"Connected (" + (a.version.empty() ? std::wstring(L"unknown version") : a.version) + L")", announce);
            return a;
        }
        if (g_lastAttachError != lastShown) {
            lastShown = g_lastAttachError;
            SetStatus(L"Waiting: " + lastShown);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

// ------------------------------------------------------------
// Posicion propia
// ------------------------------------------------------------
struct PosSample { float x, y, z; DWORD t; };

// El HumanoidRootPart se busca una sola vez por character, no en cada poll
// (FindChild lee todos los hijos del character y sus nombres: eran decenas
// de lecturas de memoria por vuelta, ~1000 vueltas por segundo).
struct CharCache { uint64_t character = 0; uint64_t hrp = 0; };

static bool GetMyPosition(HANDLE hProc, uint64_t players, CharCache& cc, float& x, float& y, float& z) {
    uint64_t localPlayer = ReadPtr(hProc, players + OFF.LocalPlayer);
    uint64_t character = localPlayer ? ReadPtr(hProc, localPlayer + OFF.ModelInstance) : 0;
    if (!character) { cc = {}; return false; }
    if (character != cc.character || !cc.hrp) {
        cc.character = character;
        cc.hrp = FindChild(hProc, character, "HumanoidRootPart");
    }
    if (cc.hrp && GetInstancePosition(hProc, cc.hrp, x, y, z)) return true;
    cc.hrp = 0; // se vuelve a buscar en el proximo poll
    return false;
}

static double Dist3(float ax, float ay, float az, float bx, float by, float bz) {
    double dx = (double)ax - bx, dy = (double)ay - by, dz = (double)az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
static double MinDistToHistory(const std::deque<PosSample>& hist, float ex, float ey, float ez) {
    double best = 1e18;
    for (auto& p : hist) best = std::min(best, Dist3(ex, ey, ez, p.x, p.y, p.z));
    return best;
}

static void SendKeyPress(char key) {
    WORD vk = (WORD)key;
    WORD scan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[0].ki.wScan = scan;
    inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    if (SendInput(2, inputs, sizeof(INPUT)) != 2)
        std::cout << "  SendInput fallo, GetLastError=" << GetLastError() << "\n";
}

// ------------------------------------------------------------
// Tooltip estilo AHK: el control de tooltip nativo de Windows (el mismo que
// usa ToolTip de AutoHotkey), cerca del mouse, se esconde solo
// ------------------------------------------------------------
static HWND g_tipHwnd = nullptr;
static TTTOOLINFOW g_tipInfo = {};
static std::wstring g_tipText; // el control apunta a este buffer, tiene que seguir vivo
constexpr UINT_PTR TIMER_HIDE_TIP = 1;

static void InitTooltip() {
    // TTS_NOFADE | TTS_NOANIMATE: aparece y desaparece de golpe, sin fade
    g_tipHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP | TTS_NOFADE | TTS_NOANIMATE,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        g_mainHwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_tipInfo.cbSize = sizeof(g_tipInfo);
    g_tipInfo.uFlags = TTF_TRACK | TTF_ABSOLUTE; // posicion manual, como hace AHK
    g_tipInfo.hwnd = g_mainHwnd;
    g_tipInfo.lpszText = (LPWSTR)L"";
    SendMessageW(g_tipHwnd, TTM_ADDTOOLW, 0, (LPARAM)&g_tipInfo);
    SendMessageW(g_tipHwnd, TTM_SETMAXTIPWIDTH, 0, 600); // habilita varias lineas con \n
}

static void HideTip() {
    KillTimer(g_mainHwnd, TIMER_HIDE_TIP);
    SendMessageW(g_tipHwnd, TTM_TRACKACTIVATE, FALSE, (LPARAM)&g_tipInfo);
    ShowWindow(g_tipHwnd, SW_HIDE); // por si el sistema igual intenta animar el cierre
}

static void ShowTip(const std::wstring& text, UINT ms) {
    if (!g_tipHwnd) return;
    g_tipText = text;
    g_tipInfo.lpszText = g_tipText.data();
    SendMessageW(g_tipHwnd, TTM_UPDATETIPTEXTW, 0, (LPARAM)&g_tipInfo);

    // como AHK: abajo a la derecha del mouse, sin salirse del monitor
    POINT pt; GetCursorPos(&pt);
    DWORD size = (DWORD)SendMessageW(g_tipHwnd, TTM_GETBUBBLESIZE, 0, (LPARAM)&g_tipInfo);
    int w = LOWORD(size), h = HIWORD(size);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    int x = pt.x + 16, y = pt.y + 16;
    if (x + w > mi.rcMonitor.right) x = mi.rcMonitor.right - w;
    if (y + h > mi.rcMonitor.bottom) y = pt.y - h - 4;
    x = std::max(x, (int)mi.rcMonitor.left);
    y = std::max(y, (int)mi.rcMonitor.top);

    SendMessageW(g_tipHwnd, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    SendMessageW(g_tipHwnd, TTM_TRACKACTIVATE, TRUE, (LPARAM)&g_tipInfo);
    SetWindowPos(g_tipHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(g_mainHwnd, TIMER_HIDE_TIP, ms, nullptr);
}

// ------------------------------------------------------------
// Icono en la bandeja (como el de un script de AHK): pasar el mouse muestra
// modo + estado, click derecho -> Exit. Sin consola, es la forma de salir.
// ------------------------------------------------------------
static NOTIFYICONDATAW g_nid = {};
static UINT g_taskbarCreatedMsg = 0; // Explorer se reinicio -> hay que volver a agregar el icono
constexpr UINT IDM_EXIT = 1;
constexpr UINT IDM_CHANGE_KEY = 2;
constexpr UINT IDM_MODE_BASE = 10; // +0 = OFF, +1..+4 = X Z C V (en el orden de MODE_LETTERS)
constexpr int IDI_MEOW = 1; // icono de meow.rc

static void UpdateTray(DWORD action) {
    std::string mode = ModeName(g_mode.load());
    std::wstring tip = L"Soru Auto X - Mode: " + std::wstring(mode.begin(), mode.end()) +
        L"\nKey: " + KeyName(g_modeVk.load()) + L"\n" + GetStatus();
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(action, &g_nid);
}

static void AddTrayIcon() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_mainHwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    // el icono propio en el tamano justo para la bandeja; si se compilo sin
    // meow.rc no existe y queda el generico
    if (FAILED(LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MEOW), LIM_SMALL, &g_nid.hIcon)))
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    UpdateTray(NIM_ADD);
}

// ------------------------------------------------------------
// Tooltips del modo. El estilo se elige en Settings: Classic es el tooltip nativo
// de siempre (ShowTip); los otros son ventanas propias, sin marco y con
// transparencia por pixel (UpdateLayeredWindow): Pill, una pastilla chica junto al
// cursor; HUD, la tira de modos arriba al centro con el activo resaltado; Keycap,
// una tecla grande abajo al centro. Aparecen con un fundido, esperan TIP_MS y se
// van. No toman foco ni clicks, asi que no molestan al juego.
// ------------------------------------------------------------
// colores de acento que se eligen en Settings (indice = Opts::accent)
struct KbAccent { COLORREF base, hi; const wchar_t* name; };
static const KbAccent KB_ACCENTS[ACCENT_COUNT] = {
    { RGB(244, 143, 177), RGB(255, 178, 203), L"Pink"   },
    { RGB(167, 139, 250), RGB(204, 188, 253), L"Violet" },
    { RGB(96, 165, 250),  RGB(158, 203, 253), L"Sky"    },
    { RGB(94, 206, 160),  RGB(160, 238, 204), L"Mint"   },
    { RGB(250, 170, 98),  RGB(253, 208, 153), L"Peach"  },
};
static ULONG_PTR g_gdipToken = 0;
static void EnsureGdiplus() {
    if (g_gdipToken) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&g_gdipToken, &in, nullptr);
}
static int TipPx(int v) { // px a 96 dpi -> px reales
    static int dpi = 0;
    if (!dpi) {
        HDC s = GetDC(nullptr);
        dpi = GetDeviceCaps(s, LOGPIXELSX);
        ReleaseDC(nullptr, s);
    }
    return MulDiv(v, dpi, 96);
}
static Gdiplus::Color TipCol(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }
static COLORREF TipMix(COLORREF a, COLORREF b, float t) {
    auto ch = [&](int x, int y) { return (int)(x + (y - x) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}
static void TipPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}
static std::wstring ModeLabel(char m) { return m ? std::wstring(1, (wchar_t)m) : std::wstring(L"OFF"); }
// sombra suave: unas capas translucidas que se agrandan alrededor del cuerpo
static void TipShadow(Gdiplus::Graphics& g, float x, float y, float w, float h, float r) {
    for (int i = 5; i >= 1; i--) {
        float e = (float)TipPx(i) * 1.4f;
        Gdiplus::GraphicsPath p;
        TipPath(p, x - e, y - e + TipPx(2), w + 2 * e, h + 2 * e, r + e);
        Gdiplus::SolidBrush b(Gdiplus::Color(14, 0, 0, 0));
        g.FillPath(&b, &p);
    }
}
static Gdiplus::Bitmap* TipNewBitmap(int bw, int bh, int M, Gdiplus::Graphics** g) { // el cuerpo mide bw x bh, M de margen para sombra / brillo
    auto* bmp = new Gdiplus::Bitmap(bw + 2 * M, bh + 2 * M, PixelFormat32bppPARGB);
    *g = new Gdiplus::Graphics(bmp);
    (*g)->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    (*g)->SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    return bmp;
}
static void TipDrawText(Gdiplus::Graphics& g, const std::wstring& text, Gdiplus::RectF r, float px, COLORREF c) {
    Gdiplus::FontFamily fam(L"Segoe UI");
    Gdiplus::Font font(&fam, px, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush b(TipCol(c));
    g.DrawString(text.c_str(), -1, &font, r, &fmt, &b);
}

// Pill: una pastilla oscura con un punto del color de acento y el modo
static Gdiplus::Bitmap* TipRenderPill(char mode, const KbAccent& ac, int M) {
    int bw = TipPx(mode ? 66 : 84), bh = TipPx(34);
    Gdiplus::Graphics* gp;
    Gdiplus::Bitmap* bmp = TipNewBitmap(bw, bh, M, &gp);
    Gdiplus::Graphics& g = *gp;
    float x = (float)M, y = (float)M, w = (float)bw, h = (float)bh, r = h / 2;
    TipShadow(g, x, y, w, h, r);
    Gdiplus::GraphicsPath body;
    TipPath(body, x, y, w, h, r);
    Gdiplus::LinearGradientBrush fill(Gdiplus::RectF(x, y, w, h), TipCol(RGB(38, 30, 39), 247), TipCol(RGB(20, 16, 21), 247),
        Gdiplus::LinearGradientModeVertical);
    g.FillPath(&fill, &body);
    Gdiplus::GraphicsPath edge;
    TipPath(edge, x + 0.5f, y + 0.5f, w - 1, h - 1, r);
    Gdiplus::Pen pen(mode ? TipCol(ac.base, 175) : TipCol(RGB(86, 72, 86)), 1.0f);
    g.DrawPath(&pen, &edge);
    Gdiplus::Pen hi(TipCol(RGB(255, 255, 255), 32), 1.0f);
    g.DrawLine(&hi, x + r, y + 1.5f, x + w - r, y + 1.5f);
    float cx = x + TipPx(17), cy = y + h / 2, dr = (float)TipPx(4);
    COLORREF dot = mode ? ac.base : RGB(128, 116, 127);
    Gdiplus::SolidBrush halo(TipCol(dot, 55)), db(TipCol(dot));
    g.FillEllipse(&halo, cx - dr * 2, cy - dr * 2, dr * 4, dr * 4);
    g.FillEllipse(&db, cx - dr, cy - dr, dr * 2, dr * 2);
    TipDrawText(g, ModeLabel(mode), Gdiplus::RectF(x + TipPx(28), y, w - TipPx(28) - TipPx(10), h),
        (float)TipPx(mode ? 17 : 14), mode ? RGB(250, 244, 248) : RGB(176, 164, 175));
    delete gp;
    return bmp;
}
// HUD: la tira con OFF y los modos prendidos, el activo resaltado con el acento
static Gdiplus::Bitmap* TipRenderHud(char mode, unsigned mask, const KbAccent& ac, int M) {
    std::vector<char> chips = { 0 };
    for (int i = 0; i < 4; i++) if ((mask >> i) & 1) chips.push_back(MODE_LETTERS[i]);
    int pad = TipPx(8), gap = TipPx(6), chH = TipPx(28), bh = TipPx(44), bw = pad * 2 + gap * ((int)chips.size() - 1);
    for (char c : chips) bw += TipPx(c ? 36 : 48);
    Gdiplus::Graphics* gp;
    Gdiplus::Bitmap* bmp = TipNewBitmap(bw, bh, M, &gp);
    Gdiplus::Graphics& g = *gp;
    float x = (float)M, y = (float)M, w = (float)bw, h = (float)bh, r = h / 2;
    TipShadow(g, x, y, w, h, r);
    Gdiplus::GraphicsPath body;
    TipPath(body, x, y, w, h, r);
    Gdiplus::LinearGradientBrush fill(Gdiplus::RectF(x, y, w, h), TipCol(RGB(34, 27, 35), 244), TipCol(RGB(18, 14, 19), 244),
        Gdiplus::LinearGradientModeVertical);
    g.FillPath(&fill, &body);
    Gdiplus::GraphicsPath edge;
    TipPath(edge, x + 0.5f, y + 0.5f, w - 1, h - 1, r);
    Gdiplus::Pen pen(TipCol(RGB(255, 255, 255), 24), 1.0f);
    g.DrawPath(&pen, &edge);
    float cx = x + pad;
    for (char c : chips) {
        float cw = (float)TipPx(c ? 36 : 48), cy = y + (h - chH) / 2;
        Gdiplus::GraphicsPath chip;
        TipPath(chip, cx, cy, cw, (float)chH, chH / 2.0f);
        bool active = c == mode;
        if (active) {
            Gdiplus::GraphicsPath glow;
            TipPath(glow, cx - 2, cy - 2, cw + 4, (float)chH + 4, chH / 2.0f + 2);
            Gdiplus::Pen gpn(TipCol(ac.base, 70), 3.0f);
            g.DrawPath(&gpn, &glow);
            Gdiplus::LinearGradientBrush cf(Gdiplus::RectF(cx, cy, cw, (float)chH), TipCol(ac.hi), TipCol(ac.base),
                Gdiplus::LinearGradientModeVertical);
            g.FillPath(&cf, &chip);
        } else {
            Gdiplus::SolidBrush cf(TipCol(RGB(255, 255, 255), 12));
            g.FillPath(&cf, &chip);
        }
        TipDrawText(g, ModeLabel(c), Gdiplus::RectF(cx, cy, cw, (float)chH), (float)TipPx(13),
            active ? TipMix(ac.base, RGB(0, 0, 0), 0.82f) : RGB(170, 157, 167));
        cx += cw + gap;
    }
    delete gp;
    return bmp;
}
// Keycap: una tecla grande con el modo, igual a la de la ventana de ajustes
static Gdiplus::Bitmap* TipRenderKeycap(char mode, const KbAccent& ac, int M) {
    int bw = TipPx(88), face = TipPx(64), depth = TipPx(9), bh = face + depth;
    Gdiplus::Graphics* gp;
    Gdiplus::Bitmap* bmp = TipNewBitmap(bw, bh, M, &gp);
    Gdiplus::Graphics& g = *gp;
    float x = (float)M, y = (float)M, w = (float)bw, r = (float)TipPx(16);
    TipShadow(g, x, y + TipPx(4), w, (float)bh - TipPx(4), r);
    for (int i = 6; i >= 1; i--) { // brillo del acento alrededor
        float e = (float)TipPx(i);
        Gdiplus::GraphicsPath p;
        TipPath(p, x - e + 0.5f, y - e + 0.5f, w + 2 * e - 1, (float)bh + 2 * e - 1, r + e);
        Gdiplus::Pen pn(TipCol(mode ? ac.base : RGB(120, 108, 119), (BYTE)(mode ? 70 / i : 24 / i)), 1.0f);
        g.DrawPath(&pn, &p);
    }
    Gdiplus::GraphicsPath side;
    TipPath(side, x, y + TipPx(6), w, (float)bh - TipPx(6), r);
    Gdiplus::SolidBrush sb(TipCol(RGB(13, 10, 14)));
    g.FillPath(&sb, &side);
    Gdiplus::Pen sp(TipCol(RGB(60, 47, 60), 170), 1.0f);
    g.DrawPath(&sp, &side);
    Gdiplus::GraphicsPath fp;
    TipPath(fp, x, y, w, (float)face, r);
    Gdiplus::LinearGradientBrush ff(Gdiplus::RectF(x, y, w, (float)face), TipCol(RGB(66, 53, 66)), TipCol(RGB(38, 31, 40)),
        Gdiplus::LinearGradientModeVertical);
    g.FillPath(&ff, &fp);
    Gdiplus::Pen hi(TipCol(RGB(255, 255, 255), 44), 1.0f);
    g.DrawLine(&hi, x + TipPx(14), y + 1.5f, x + w - TipPx(14), y + 1.5f);
    Gdiplus::GraphicsPath ep;
    TipPath(ep, x + 0.5f, y + 0.5f, w - 1, (float)face - 1, r);
    Gdiplus::Pen epn(mode ? TipCol(ac.base) : TipCol(RGB(96, 80, 96)), 1.6f);
    g.DrawPath(&epn, &ep);
    TipDrawText(g, ModeLabel(mode), Gdiplus::RectF(x, y, w, (float)face), (float)TipPx(mode ? 38 : 25),
        mode ? RGB(250, 244, 248) : RGB(184, 170, 180));
    delete gp;
    return bmp;
}
// donde va el cuerpo de cada estilo (sin el margen); devuelve la esquina de la ventana
static void TipPlace(int style, int winW, int winH, int M, int& x, int& y) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
    if (style == 1) mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int bw = winW - 2 * M, bh = winH - 2 * M, bx, by;
    if (style == 1) { // abajo a la derecha del cursor, sin salirse del monitor (como el Classic)
        bx = pt.x + TipPx(18);
        by = pt.y + TipPx(18);
        if (bx + bw > mi.rcMonitor.right) bx = mi.rcMonitor.right - bw;
        if (by + bh > mi.rcMonitor.bottom) by = pt.y - bh - TipPx(6);
    } else {
        int wkW = mi.rcWork.right - mi.rcWork.left, wkH = mi.rcWork.bottom - mi.rcWork.top;
        bx = mi.rcWork.left + (wkW - bw) / 2;
        by = style == 2 ? mi.rcWork.top + wkH * 7 / 100 : mi.rcWork.top + wkH * 72 / 100 - bh / 2;
    }
    x = bx - M;
    y = by - M;
}

constexpr UINT_PTR TIMER_POP = 3;
static HWND g_popHwnd = nullptr;
static HBITMAP g_popBmp = nullptr;
static int g_popW = 0, g_popH = 0, g_popX = 0, g_popY = 0;
static float g_popAlpha = 0;
static DWORD g_popHoldUntil = 0;
static bool g_popOk = true; // resultado del ultimo UpdateLayeredWindow

// posicion y opacidad de ahora: aparece subiendo un poco y se va bajando
static void PopApply() {
    if (!g_popHwnd || !g_popBmp) return;
    float e = g_popAlpha * g_popAlpha * (3.0f - 2.0f * g_popAlpha);
    int slide = (int)((1.0f - e) * TipPx(8) + 0.5f);
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HGDIOBJ old = SelectObject(mdc, g_popBmp);
    POINT dst = { g_popX, g_popY + slide }, src = { 0, 0 };
    SIZE sz = { g_popW, g_popH };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(255 * e + 0.5f), AC_SRC_ALPHA };
    g_popOk = UpdateLayeredWindow(g_popHwnd, sdc, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA) != FALSE;
    SelectObject(mdc, old);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
}
static void HidePop() {
    g_popAlpha = 0;
    g_popHoldUntil = 0;
    if (!g_popHwnd) return;
    KillTimer(g_popHwnd, TIMER_POP);
    ShowWindow(g_popHwnd, SW_HIDE);
}
static void PopTick() { // sube la opacidad mientras dura el tooltip, la baja despues
    float target = (LONG)(g_popHoldUntil - GetTickCount()) > 0 ? 1.0f : 0.0f;
    if (target > g_popAlpha) g_popAlpha = std::min(target, g_popAlpha + 0.2f);
    else g_popAlpha = std::max(target, g_popAlpha - 0.12f);
    if (g_popAlpha <= 0.0f && target == 0.0f) { HidePop(); return; }
    PopApply();
}
static LRESULT CALLBACK PopWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == TIMER_POP) { PopTick(); return 0; }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT; // los clicks pasan de largo
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// la imagen del tooltip de un estilo; null para Classic (ese no tiene imagen propia)
static Gdiplus::Bitmap* TipRender(int style, char mode, int accent, unsigned mask, int M) {
    EnsureGdiplus();
    const KbAccent& ac = KB_ACCENTS[std::clamp(accent, 0, ACCENT_COUNT - 1)];
    if (style == 1) return TipRenderPill(mode, ac, M);
    if (style == 2) return TipRenderHud(mode, mask, ac, M);
    if (style == 3) return TipRenderKeycap(mode, ac, M);
    return nullptr;
}
// Muestra el tooltip del modo con un estilo (0 Classic, 1 Pill, 2 HUD, 3 Keycap). Lo usan el
// cambio de modo y la vista previa de la ventana de ajustes, que lo llama con el estilo sin guardar.
static void ShowModeTipStyle(int style, char mode, int accent, unsigned mask, int ms) {
    int M = TipPx(12);
    Gdiplus::Bitmap* bmp = TipRender(style, mode, accent, mask, M);
    if (!bmp) { // Classic: el tooltip nativo de siempre, solo la letra en minuscula
        HidePop();
        std::wstring tip;
        for (char c : ModeName(mode)) tip += (wchar_t)towlower((wint_t)(unsigned char)c);
        ShowTip(tip, (UINT)ms);
        return;
    }
    HideTip(); // por si habia un Classic a medio mostrar
    if (g_popBmp) DeleteObject(g_popBmp);
    g_popBmp = nullptr;
    bmp->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &g_popBmp);
    g_popW = (int)bmp->GetWidth();
    g_popH = (int)bmp->GetHeight();
    delete bmp;
    TipPlace(style, g_popW, g_popH, M, g_popX, g_popY);
    if (!g_popHwnd) {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = PopWndProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"SoruAutoXPop";
        RegisterClassW(&wc);
        g_popHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"SoruAutoXPop", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
        if (!g_popHwnd) return;
    }
    g_popHoldUntil = GetTickCount() + (DWORD)ms;
    PopApply();
    ShowWindow(g_popHwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_popHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetTimer(g_popHwnd, TIMER_POP, 15, nullptr);
}
static void ShowModeTip(char mode) {
    ShowModeTipStyle(g_opts.tipStyle, mode, g_opts.accent, g_modeMask.load(), g_opts.tipMs);
}

// ------------------------------------------------------------
// Indicador siempre visible: una pastilla chica en una esquina de la pantalla con el
// modo actual, mientras Roblox tiene el foco (y un rato como vista previa cuando lo
// elegis en Settings). Misma tecnica que los tooltips: ventana con transparencia por
// pixel, sin foco ni clicks. Un timer propio la prende y apaga con un fundido.
// ------------------------------------------------------------
constexpr UINT_PTR TIMER_IND = 4;
static HWND g_indHwnd = nullptr;
static HBITMAP g_indBmp = nullptr;
static int g_indW = 0, g_indH = 0, g_indX = INT_MIN, g_indY = INT_MIN;
static float g_indAlpha = 0;
static int g_indCorner = 0, g_indAccent = 0; // 0 = apagado
static char g_indMode = 0;
static DWORD g_indForceUntil = 0;            // vista previa: se muestra aunque Roblox no tenga el foco
static bool g_indOk = true;                  // resultado del ultimo UpdateLayeredWindow
constexpr float IND_OPACITY = 0.9f;

static Gdiplus::Bitmap* TipRenderIndicator(char mode, const KbAccent& ac, int M) {
    int bw = TipPx(mode ? 54 : 68), bh = TipPx(28);
    Gdiplus::Graphics* gp;
    Gdiplus::Bitmap* bmp = TipNewBitmap(bw, bh, M, &gp);
    Gdiplus::Graphics& g = *gp;
    float x = (float)M, y = (float)M, w = (float)bw, h = (float)bh, r = h / 2;
    TipShadow(g, x, y, w, h, r);
    Gdiplus::GraphicsPath body;
    TipPath(body, x, y, w, h, r);
    Gdiplus::LinearGradientBrush fill(Gdiplus::RectF(x, y, w, h), TipCol(RGB(36, 29, 37), 240), TipCol(RGB(19, 15, 20), 240),
        Gdiplus::LinearGradientModeVertical);
    g.FillPath(&fill, &body);
    Gdiplus::GraphicsPath edge;
    TipPath(edge, x + 0.5f, y + 0.5f, w - 1, h - 1, r);
    Gdiplus::Pen pen(mode ? TipCol(ac.base, 150) : TipCol(RGB(80, 67, 80)), 1.0f);
    g.DrawPath(&pen, &edge);
    float cx = x + TipPx(15), cy = y + h / 2, dr = TipPx(7) / 2.0f;
    COLORREF dot = mode ? ac.base : RGB(128, 116, 127);
    Gdiplus::SolidBrush halo(TipCol(dot, 55)), db(TipCol(dot));
    g.FillEllipse(&halo, cx - dr * 2, cy - dr * 2, dr * 4, dr * 4);
    g.FillEllipse(&db, cx - dr, cy - dr, dr * 2, dr * 2);
    TipDrawText(g, ModeLabel(mode), Gdiplus::RectF(x + TipPx(24), y, w - TipPx(24) - TipPx(8), h),
        (float)TipPx(mode ? 15 : 13), mode ? RGB(250, 244, 248) : RGB(176, 164, 175));
    delete gp;
    return bmp;
}
// la esquina de la pantalla donde esta Roblox (o la principal)
static void IndPlace() {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST), &mi);
    int M = TipPx(12), m = TipPx(18), bw = g_indW - 2 * M, bh = g_indH - 2 * M;
    bool right = g_indCorner == 2 || g_indCorner == 4, bottom = g_indCorner == 3 || g_indCorner == 4;
    int bx = right ? mi.rcWork.right - m - bw : mi.rcWork.left + m;
    int by = bottom ? mi.rcWork.bottom - m - bh : mi.rcWork.top + m;
    g_indX = bx - M;
    g_indY = by - M;
}
static void IndApply() {
    if (!g_indHwnd || !g_indBmp) return;
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HGDIOBJ old = SelectObject(mdc, g_indBmp);
    POINT dst = { g_indX, g_indY }, src = { 0, 0 };
    SIZE sz = { g_indW, g_indH };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(255 * g_indAlpha + 0.5f), AC_SRC_ALPHA };
    g_indOk = UpdateLayeredWindow(g_indHwnd, sdc, &dst, &sz, mdc, &src, 0, &bf, ULW_ALPHA) != FALSE;
    SelectObject(mdc, old);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
}
static void IndTick() {
    bool want = g_indCorner != 0 && ((LONG)(g_indForceUntil - GetTickCount()) > 0 ||
        (g_robloxPid.load() && RobloxIsForeground(g_robloxPid.load())));
    float target = want ? IND_OPACITY : 0.0f;
    float before = g_indAlpha;
    if (target > g_indAlpha) g_indAlpha = std::min(target, g_indAlpha + 0.1f);
    else g_indAlpha = std::max(target, g_indAlpha - 0.07f);
    if (g_indAlpha > 0.0f) {
        int ox = g_indX, oy = g_indY;
        IndPlace(); // sigue a Roblox si se mueve de monitor
        if (g_indAlpha != before || ox != g_indX || oy != g_indY) IndApply();
        if (!IsWindowVisible(g_indHwnd)) ShowWindow(g_indHwnd, SW_SHOWNOACTIVATE);
    } else if (before > 0.0f) {
        ShowWindow(g_indHwnd, SW_HIDE);
    }
    // mientras se mueve, 33 ms; en reposo alcanza con mirar el foco 4 veces por segundo
    bool moving = g_indAlpha != target;
    SetTimer(g_indHwnd, TIMER_IND, moving ? 33 : 250, nullptr);
}
static LRESULT CALLBACK IndWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == TIMER_IND) { IndTick(); return 0; }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
// corner 0 = apagado, 1..4 = esquina; llamar de nuevo con el modo o el acento nuevo
static void IndicatorSet(int corner, char mode, int accent) {
    bool changed = corner != g_indCorner || mode != g_indMode || accent != g_indAccent || (corner && !g_indBmp);
    g_indCorner = corner;
    g_indMode = mode;
    g_indAccent = accent;
    if (!corner && !g_indHwnd) return;
    if (!g_indHwnd) {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = IndWndProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"SoruAutoXInd";
        RegisterClassW(&wc);
        g_indHwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"SoruAutoXInd", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
        if (!g_indHwnd) return;
    }
    if (corner && changed) {
        EnsureGdiplus();
        int M = TipPx(12);
        std::unique_ptr<Gdiplus::Bitmap> bmp(TipRenderIndicator(mode, KB_ACCENTS[std::clamp(accent, 0, ACCENT_COUNT - 1)], M));
        if (g_indBmp) DeleteObject(g_indBmp);
        g_indBmp = nullptr;
        bmp->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &g_indBmp);
        g_indW = (int)bmp->GetWidth();
        g_indH = (int)bmp->GetHeight();
        IndPlace();
        if (g_indAlpha > 0.0f) IndApply(); // ya se ve: cambia el contenido sin parpadeo
    }
    SetTimer(g_indHwnd, TIMER_IND, 33, nullptr); // el timer se encarga de prenderlo, apagarlo y mantenerlo en su lugar
}
static void IndicatorMode(char mode) { IndicatorSet(g_indCorner, mode, g_indAccent); }
// muestra el indicador un rato aunque Roblox no tenga el foco: la vista previa de Settings
static void IndicatorPreview(int corner, char mode, int accent) {
    g_indForceUntil = GetTickCount() + 2500;
    IndicatorSet(corner, mode, accent);
}

// OFF -> X -> Z -> C -> V -> OFF, saltando los modos apagados en la ventana
static char NextMode(char cur, unsigned mask) {
    size_t n = sizeof(MODE_ORDER), idx = 0;
    for (size_t i = 0; i < n; i++) if (MODE_ORDER[i] == cur) { idx = i; break; }
    for (size_t step = 1; step <= n; step++) {
        char c = MODE_ORDER[(idx + step) % n];
        if (c == 0 || ModeEnabled(c, mask)) return c; // OFF siempre entra, asi que siempre hay uno
    }
    return 0;
}
// El modo cambio, con la tecla de modo o desde el menu de la bandeja: se anuncia con el
// tooltip elegido en Settings (Classic es el de siempre: "x", "z", "c", "v", "off") y se
// actualiza todo lo que lo muestra: indicador, bandeja y la ventana de ajustes (el aro).
static void AnnounceMode(char next) {
    g_mode.store(next);
    std::cout << "[modo] -> " << ModeName(next) << "\n";
    ShowModeTip(next);
    IndicatorMode(next);
    UpdateTray(NIM_MODIFY);
    if (g_kbHwnd) PostMessageW(g_kbHwnd, WM_APP_REFRESH, 0, 0);
}
static void CycleMode() {
    AnnounceMode(NextMode(g_mode.load(), g_modeMask.load()));
}

// ------------------------------------------------------------
// Hook de teclado: tecla de modo + R
// ------------------------------------------------------------
// Hook de teclado de bajo nivel (corre en el thread principal):
//  - la tecla de modo (g_modeVk) cicla el modo y el hook se la come, asi nunca
//    llega a la app (Caps Lock nunca se prende)
//  - cada R que llega con Roblox en foco se anota en g_lastRPress
//  - con la ventana de teclas al frente, la proxima tecla va a esa ventana en
//    vez de a las apps (los modificadores y Alt+algo pasan de largo)
// Tiene que volver rapido (Windows desengancha solos los hooks lentos), por
// eso no hace nada pesado: el cambio de modo pasa en MainWndProc.
static bool g_modeKeyHeld = false;
static DWORD g_soruScan = 0; // scancode de R en el layout actual, se calcula en wWinMain

static LRESULT CALLBACK KeyboardHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (const KBDLLHOOKSTRUCT*)lp;
        bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);

        // R: cuenta cualquier keydown, fisico o inyectado. Un macro de spam de
        // R manda input inyectado con down/up casi instantaneos, y polleando
        // GetAsyncKeyState cada 1ms se perdian la mayoria. El hook los ve todos.
        // (el scancode es por si el macro manda solo scancode sin VK)
        if (down && (k->vkCode == SORU_VK || (g_soruScan && k->scanCode == g_soruScan && !(k->flags & LLKHF_EXTENDED)))) {
            DWORD pid = g_robloxPid.load();
            if (pid && RobloxIsForeground(pid)) g_lastRPress.store(GetTickCount());
        }

        // ventana de teclas al frente: captura la tecla en vez de ciclar el
        // modo. Solo si es la ventana activa, para no dejarte sin teclado si
        // te vas a otra app con la ventana abierta.
        if (g_kbHwnd && !(k->flags & (LLKHF_INJECTED | LLKHF_ALTDOWN)) && GetForegroundWindow() == g_kbHwnd) {
            if (down) PostMessageW(g_kbHwnd, WM_APP_KEY_CAPTURED, k->vkCode, 0);
            if (!IsModifierVk(k->vkCode)) return 1;
            return CallNextHookEx(nullptr, code, wp, lp);
        }

        // las inyectadas pasan (ej: ForceCapsLockOff al arrancar)
        if (k->vkCode == g_modeVk.load() && !(k->flags & LLKHF_INJECTED)) {
            if (down && !g_modeKeyHeld) PostMessageW(g_mainHwnd, WM_APP_CYCLE_MODE, 0, 0);
            g_modeKeyHeld = down; // mantenerla apretada no cicla de nuevo
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Si Caps Lock ya estaba prendida al arrancar, la apagamos: despues el hook
// se la come y no habria forma de apagarla mientras esto corra.
static void ForceCapsLockOff() {
    if (!(GetKeyState(VK_CAPITAL) & 1)) return;
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CAPITAL;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// Guarda la tecla y los modos elegidos y los deja activos. Tambien marca el
// setup como hecho, asi la ventana de la primera vez no vuelve a salir.
static void ApplySettings(DWORD vk, unsigned mask, const Opts& opts) {
    g_modeVk.store(vk);
    g_modeMask.store(mask);
    g_opts = opts;
    SyncTiming(opts);
    g_modeKeyHeld = false;
    g_keyConfigured = true;
    SaveSettings();
    char cur = g_mode.load();
    if (cur && !ModeEnabled(cur, mask)) g_mode.store(0); // apagaste el modo que estaba activo: vuelve a OFF
    if (vk == VK_CAPITAL) ForceCapsLockOff(); // si estaba prendida, el hook ya no la deja apagarse sola
    IndicatorSet(opts.indicator, g_mode.load(), opts.accent);
    UpdateTray(NIM_MODIFY);
}

// ------------------------------------------------------------
// Ventana de ajustes. Sale sola la primera vez y despues desde el menu de la
// bandeja. Cinco pestanas:
//   Keybind   la tecla de modo, los modos que entran en el ciclo y el estado en vivo
//             (conexion con Roblox y modo activo)
//   Tooltips  el estilo del tooltip del modo, cuanto dura y el indicador siempre visible
//   Timing    las tres ventanas de tiempo de la deteccion
//   General   Status notices, recordar la posicion de la ventana y el color de acento
//   Stats     contadores de la sesion
// La tecla la captura el hook (ver KeyboardHook), asi anda con cualquier tecla, Caps
// Lock incluida. Sin marco ni controles de Windows: todo se dibuja a mano (GDI+ para
// las formas con bordes suaves, GDI para el texto), con barra de titulo propia,
// esquinas redondeadas (Windows 11), animaciones de hover / switches / pestanas /
// sliders, la tecla como una tecla de verdad que se hunde al elegirla y un temblor
// al rechazarla. Los cambios se aplican al tocar Save; Cancel los descarta. Un
// puntito junto al nombre de una pestana avisa que tiene cambios sin guardar.
// ------------------------------------------------------------
constexpr COLORREF KB_BG_TOP     = RGB(24, 19, 25);
constexpr COLORREF KB_BG_BOT     = RGB(8, 6, 9);
constexpr COLORREF KB_TEXT       = RGB(246, 239, 244);
constexpr COLORREF KB_TEXT_DIM   = RGB(184, 170, 180);
constexpr COLORREF KB_TEXT_FAINT = RGB(122, 108, 119);
constexpr COLORREF KB_LINE       = RGB(56, 44, 56);
constexpr COLORREF KB_CHIP       = RGB(27, 21, 28);
constexpr COLORREF KB_TRACK      = RGB(48, 40, 50);   // riel de un switch apagado
constexpr COLORREF KB_ERROR      = RGB(255, 108, 108);
constexpr COLORREF KB_WARN       = RGB(250, 190, 100);
constexpr COLORREF KB_OK         = RGB(120, 220, 170);
constexpr COLORREF KB_CAP_TOP    = RGB(58, 46, 58);   // cara de la tecla
constexpr COLORREF KB_CAP_BOT    = RGB(35, 28, 37);
constexpr COLORREF KB_CAP_SIDE   = RGB(13, 10, 14);   // el costado, lo que se ve debajo de la cara
// atributos de DwmSetWindowAttribute (solo Windows 11; en 10 fallan y se ignoran)
constexpr DWORD KB_DWMWA_CORNER = 33;       // 2 = esquinas redondeadas
constexpr DWORD KB_DWMWA_BORDER_COLOR = 34; // 0xFFFFFFFE = sin borde de DWM, dibujamos el nuestro

constexpr int KB_CLIENT_W = 340, KB_CLIENT_H = 336, KB_TITLE_H = 40; // px a 96 dpi, KbPx() los escala
constexpr UINT_PTR KB_TIMER = 2;       // animaciones, ~60 fps y solo mientras algo se mueve
constexpr UINT_PTR KB_TIMER_SLOW = 5;  // una vez por segundo y solo en Stats: el "hace 12 s" y el reloj de la sesion
constexpr int KB_TABS = 5;
enum {
    KB_NONE = 0, KB_CLOSE, KB_CANCEL, KB_SAVE,
    KB_TAB_0, KB_TAB_1, KB_TAB_2, KB_TAB_3, KB_TAB_4,         // Keybind, Tooltips, Timing, General, Stats
    KB_MODE_X, KB_MODE_Z, KB_MODE_C, KB_MODE_V,               // Keybind: los modos, en el orden de MODE_LETTERS
    KB_STY_0, KB_STY_1, KB_STY_2, KB_STY_3, KB_SLD_TIP,       // Tooltips: estilo y duracion
    KB_IND_0, KB_IND_1, KB_IND_2, KB_IND_3, KB_IND_4,         // Tooltips: indicador (apagado + 4 esquinas)
    KB_SLD_R, KB_SLD_MATCH, KB_SLD_COOL, KB_TIMING_RESET,     // Timing
    KB_SW_STATUS, KB_SW_REMEMBER,                             // General
    KB_ACC_0, KB_ACC_1, KB_ACC_2, KB_ACC_3, KB_ACC_4, KB_RESET,
    KB_STATS_RESET,                                           // Stats
    KB_NELEM
};
enum { KB_MSG_NOTE = 0, KB_MSG_NEW, KB_MSG_ERROR };
static const wchar_t* KB_TAB_NAMES[KB_TABS] = { L"Keybind", L"Tooltips", L"Timing", L"General", L"Stats" };
// los sliders: que ajuste mueven, su rango y los textos
struct KbSliderDef { int id; const Range* range; int Opts::* field; const wchar_t* title; const wchar_t* sub; bool seconds; };
static const KbSliderDef KB_SLIDERS[4] = {
    { KB_SLD_TIP,   &RANGE_TIP_MS,   &Opts::tipMs,      L"Duration",     nullptr,                                          true  },
    { KB_SLD_R,     &RANGE_R_MS,     &Opts::rWindowMs,  L"R window",     L"How long after pressing R a Soru still counts", false },
    { KB_SLD_MATCH, &RANGE_MATCH_MS, &Opts::matchMs,    L"Sound match",  L"Max gap between your teleport and the Soru sound", false },
    { KB_SLD_COOL,  &RANGE_COOL_MS,  &Opts::cooldownMs, L"Cooldown",     L"Minimum time between two key presses",         false },
};

static int g_kbDpi = 96;
static bool g_kbFirstRun = false;
static bool g_kbRound = false;     // Windows 11: esquinas redondeadas
static int g_kbTab = 0;            // la pestana abierta, 0..KB_TABS-1
static DWORD g_kbPending = 0;      // tecla elegida en la ventana, sin guardar todavia
static unsigned g_kbMask = 0;      // modos prendidos en la ventana, sin guardar todavia
static Opts g_kbOpts;              // ajustes elegidos en la ventana, sin guardar todavia
static std::wstring g_kbMsg;       // linea de estado debajo de los modos
static int g_kbMsgKind = KB_MSG_NOTE;
static int g_kbHover = KB_NONE, g_kbDown = KB_NONE;
static int g_kbDrag = KB_NONE;     // el slider que se esta arrastrando
static float g_kbHot[KB_NELEM] = {}; // 0..1, cuanto "hover" tiene cada elemento (animado)
static float g_kbOn[4] = {};         // 0..1, que tan prendido se ve cada modo (animado)
static float g_kbSw[2] = {};         // 0..1, posicion de los switches Status notices / Remember position (animado)
static float g_kbSeg = 0;            // 0..3, resaltado del selector de estilo (animado)
static float g_kbInd = 0;            // 0..4, resaltado del selector de indicador (animado)
static float g_kbSl[4] = {};         // 0..1, posicion de cada slider (animada)
static float g_kbBar[4] = {};        // 0..1, largo de las barras de Stats (animado)
static float g_kbTabPos = 0;         // 0..KB_TABS-1, donde esta la linea de la pestana (animado)
static float g_kbRing = 0;           // aro del modo activo: indice 0..4 (OFF, X, Z, C, V), fraccionario mientras se desliza
static int g_kbAccPrev = 0;          // acento del que se viene, y cuanto va de la transicion de color
static float g_kbAccT = 1;
static float g_kbFlash = 0, g_kbErr = 0, g_kbFade = 0, g_kbPress = 0;
static bool g_kbTimerOn = false;
static bool g_kbClosing = false; // fundido de salida en curso; al llegar a 0 el timer destruye la ventana
static int g_kbSlide = 0;        // px que la ventana esta corrida hacia abajo (se desliza un poco al aparecer / irse)
static HFONT g_kbFontSmall = nullptr, g_kbFontTiny = nullptr, g_kbFontChip = nullptr, g_kbFontKey = nullptr,
             g_kbFontBtn = nullptr, g_kbFontNum = nullptr;
static HICON g_kbIcon = nullptr;
static const wchar_t* KB_NOTE = L"Current key. It's blocked in other apps.";

static int KbPx(int v) { return MulDiv(v, g_kbDpi, 96); }
static int KbSliderIdx(int id) { for (int i = 0; i < 4; i++) if (KB_SLIDERS[i].id == id) return i; return -1; }
static int KbTabOf(int id) { // en que pestana vive un elemento (-1 = en todas)
    if (id >= KB_MODE_X && id <= KB_MODE_V) return 0;
    if (id >= KB_STY_0 && id <= KB_IND_4) return 1;
    if (id >= KB_SLD_R && id <= KB_TIMING_RESET) return 2;
    if (id >= KB_SW_STATUS && id <= KB_RESET) return 3;
    if (id == KB_STATS_RESET) return 4;
    return -1;
}
static RECT KbTabRect(int i) { return { KbPx(10 + i * 64), KbPx(40), KbPx(10 + (i + 1) * 64), KbPx(72) }; }
// Keybind: la fila de modos, i = 0 es OFF (fijo), 1..4 son X Z C V (se prenden y apagan con un click)
static RECT KbChipRect(int i) {
    int x = (KB_CLIENT_W - 280) / 2; // 48 de OFF + 4 * 36 + 4 huecos de 22, centrado
    for (int k = 0; k < i; k++) x += (k == 0 ? 48 : 36) + 22;
    return { KbPx(x), KbPx(214), KbPx(x + (i == 0 ? 48 : 36)), KbPx(238) };
}
// Tooltips: la fila del estilo (alta, con el selector de 4 abajo), la del slider y la del indicador (selector de 5)
static RECT KbStyleRow() { return { KbPx(24), KbPx(80), KbPx(316), KbPx(146) }; }
static RECT KbSegRect(int i) { return { KbPx(36 + i * 67), KbPx(112), KbPx(36 + (i + 1) * 67), KbPx(138) }; }
static RECT KbIndRow() { return { KbPx(24), KbPx(202), KbPx(316), KbPx(268) }; }
static RECT KbIndRect(int i) { return { KbPx(36 + i * 268 / 5), KbPx(234), KbPx(36 + (i + 1) * 268 / 5), KbPx(260) }; }
// sliders: la fila (titulo, valor y texto) y el riel, que va pegado al borde de abajo de la fila
static RECT KbSliderRow(int id) {
    switch (id) {
    case KB_SLD_TIP:   return { KbPx(24), KbPx(150), KbPx(316), KbPx(198) };
    case KB_SLD_R:     return { KbPx(24), KbPx(82),  KbPx(316), KbPx(136) };
    case KB_SLD_MATCH: return { KbPx(24), KbPx(140), KbPx(316), KbPx(194) };
    }
    return { KbPx(24), KbPx(198), KbPx(316), KbPx(252) };
}
static RECT KbTrack(int id) {
    RECT r = KbSliderRow(id);
    int cy = r.bottom - KbPx(12);
    return { KbPx(36), cy - KbPx(3), KbPx(304), cy + KbPx(3) };
}
// General: 3 filas, las dos primeras con switch y la tercera con los colores
static RECT KbGenRow(int row) { return { KbPx(24), KbPx(84 + row * 44), KbPx(316), KbPx(84 + row * 44 + 40) }; }
static RECT KbSwitchRect(int row) {
    RECT r = KbGenRow(row);
    int cy = (r.top + r.bottom) / 2;
    return { r.right - KbPx(46), cy - KbPx(10), r.right - KbPx(8), cy + KbPx(10) }; // 38 x 20
}
static RECT KbSwatchRect(int i) { // 5 circulos de 20 con 10 de hueco, pegados a la derecha de la fila 2
    RECT r = KbGenRow(2);
    int cy = (r.top + r.bottom) / 2, x = r.right - KbPx(8) - KbPx(140) + i * KbPx(30);
    return { x, cy - KbPx(10), x + KbPx(20), cy + KbPx(10) };
}
static RECT KbRect(int id) {
    switch (id) {
    case KB_CLOSE:        return { KbPx(298), KbPx(6),   KbPx(326), KbPx(34)  };
    case KB_CANCEL:       return { KbPx(24),  KbPx(288), KbPx(166), KbPx(318) };
    case KB_SAVE:         return { KbPx(174), KbPx(288), KbPx(316), KbPx(318) };
    case KB_TIMING_RESET: return { KbPx(36),  KbPx(258), KbPx(200), KbPx(276) };
    case KB_RESET:        return { KbPx(36),  KbPx(226), KbPx(200), KbPx(244) };
    case KB_STATS_RESET:  return { KbPx(226), KbPx(262), KbPx(316), KbPx(280) };
    case KB_TAB_0: case KB_TAB_1: case KB_TAB_2: case KB_TAB_3: case KB_TAB_4:
        return KbTabRect(id - KB_TAB_0);
    case KB_MODE_X: case KB_MODE_Z: case KB_MODE_C: case KB_MODE_V:
        return KbChipRect(id - KB_MODE_X + 1);
    case KB_STY_0: case KB_STY_1: case KB_STY_2: case KB_STY_3:
        return KbSegRect(id - KB_STY_0);
    case KB_IND_0: case KB_IND_1: case KB_IND_2: case KB_IND_3: case KB_IND_4:
        return KbIndRect(id - KB_IND_0);
    case KB_SLD_TIP: case KB_SLD_R: case KB_SLD_MATCH: case KB_SLD_COOL: {
        RECT t = KbTrack(id); // la zona que agarra el mouse es mas alta que el riel y sobra a los costados, por la perilla en las puntas
        return { t.left - KbPx(10), t.top - KbPx(9), t.right + KbPx(10), t.bottom + KbPx(9) };
    }
    case KB_SW_STATUS: return KbGenRow(0);
    case KB_SW_REMEMBER: return KbGenRow(1);
    case KB_ACC_0: case KB_ACC_1: case KB_ACC_2: case KB_ACC_3: case KB_ACC_4:
        return KbSwatchRect(id - KB_ACC_0);
    }
    return {};
}
static bool KbVisible(int id) { // que elementos se ven en la pestana actual
    int t = KbTabOf(id);
    return t < 0 || t == g_kbTab;
}
static int KbHit(int x, int y) {
    POINT pt = { x, y };
    for (int id = KB_CLOSE; id < KB_NELEM; id++) {
        if (!KbVisible(id)) continue;
        RECT r = KbRect(id);
        if (PtInRect(&r, pt)) return id;
    }
    return KB_NONE;
}
static HFONT KbFont(int px, int weight) {
    return CreateFontW(-KbPx(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}
static int KbTextW(HDC dc, const std::wstring& text, HFONT font) {
    SelectObject(dc, font);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    return sz.cx;
}
static COLORREF KbMix(COLORREF a, COLORREF b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [&](int x, int y) { return (int)(x + (y - x) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}
// el acento actual; mientras dura la transicion es una mezcla del anterior y el nuevo
static COLORREF AccBase() { return KbMix(KB_ACCENTS[g_kbAccPrev].base, KB_ACCENTS[g_kbOpts.accent].base, g_kbAccT); }
static COLORREF AccHi() { return KbMix(KB_ACCENTS[g_kbAccPrev].hi, KB_ACCENTS[g_kbOpts.accent].hi, g_kbAccT); }
static COLORREF AccOn() { return KbMix(AccBase(), RGB(0, 0, 0), 0.82f); } // texto oscuro sobre el boton de acento
static Gdiplus::Color KbCol(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}
static Gdiplus::RectF KbRectF(const RECT& rc) {
    return Gdiplus::RectF((float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
}
static void KbRoundPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    if (r < 1.0f) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
    float d = r * 2.0f;
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}
static void KbFillRound(Gdiplus::Graphics& g, const RECT& rc, float r, const Gdiplus::Brush& brush) {
    Gdiplus::GraphicsPath p;
    Gdiplus::RectF f = KbRectF(rc);
    KbRoundPath(p, f.X, f.Y, f.Width, f.Height, r);
    g.FillPath(&brush, &p);
}
static void KbFillRound(Gdiplus::Graphics& g, const RECT& rc, float r, COLORREF c, BYTE alpha = 255) {
    Gdiplus::SolidBrush b(KbCol(c, alpha));
    KbFillRound(g, rc, r, b);
}
static void KbStrokeRound(Gdiplus::Graphics& g, const RECT& rc, float r, COLORREF c, BYTE alpha = 255, float width = 1.0f) {
    Gdiplus::GraphicsPath p; // inset medio pixel para que la linea de 1px caiga justo en el borde
    KbRoundPath(p, rc.left + 0.5f, rc.top + 0.5f, (float)(rc.right - rc.left) - 1.0f, (float)(rc.bottom - rc.top) - 1.0f, r);
    Gdiplus::Pen pen(KbCol(c, alpha), width);
    g.DrawPath(&pen, &p);
}
static void KbText(HDC dc, const std::wstring& text, RECT rc, HFONT font, COLORREF color, UINT align) {
    SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rc, align | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}
// estado de la conexion con Roblox, resumido del texto que muestra la bandeja
static void KbLiveStatus(std::wstring& text, COLORREF& dot) {
    std::wstring s = GetStatus();
    if (s.rfind(L"Connected", 0) == 0) { text = L"Connected"; dot = KB_OK; }
    else if (s.rfind(L"Waiting", 0) == 0) { text = L"Waiting for Roblox"; dot = KB_WARN; }
    else { text = L"Reconnecting"; dot = KB_WARN; }
}
static float KbRingTarget() { // el indice del modo que esta activo ahora (0 = OFF)
    char m = g_mode.load();
    for (int i = 0; i < 4; i++) if (MODE_LETTERS[i] == m) return (float)(i + 1);
    return 0.0f;
}
// sliders: valor <-> posicion 0..1, y como se escribe el valor
static float KbSliderNorm(int idx, int v) {
    const Range& r = *KB_SLIDERS[idx].range;
    return (float)(v - r.lo) / (float)(r.hi - r.lo);
}
static int KbSliderFromT(int idx, float t) { // t 0..1 -> el valor mas cercano que cae en un paso del slider
    const Range& r = *KB_SLIDERS[idx].range;
    int steps = (r.hi - r.lo) / r.step;
    return r.lo + (int)floorf(std::clamp(t, 0.0f, 1.0f) * steps + 0.5f) * r.step;
}
static std::wstring KbSliderText(int idx, int v) {
    wchar_t buf[32];
    if (KB_SLIDERS[idx].seconds) swprintf(buf, 32, L"%.1f s", v / 1000.0);
    else swprintf(buf, 32, L"%d ms", v);
    return buf;
}
// "hace 12 s" / "hace 3 min" para la linea del ultimo envio
static std::wstring KbAgo(DWORD ms) {
    wchar_t buf[32];
    if (ms < 1500) return L"just now";
    if (ms < 60000) swprintf(buf, 32, L"%lu s ago", (unsigned long)(ms / 1000));
    else if (ms < 3600000) swprintf(buf, 32, L"%lu min ago", (unsigned long)(ms / 60000));
    else swprintf(buf, 32, L"%lu h ago", (unsigned long)(ms / 3600000));
    return buf;
}
// true si la pestana tiene algo cambiado y sin guardar
static bool KbTabDirty(int tab) {
    switch (tab) {
    case 0: return g_kbPending != g_modeVk.load() || g_kbMask != g_modeMask.load();
    case 1: return g_kbOpts.tipStyle != g_opts.tipStyle || g_kbOpts.tipMs != g_opts.tipMs || g_kbOpts.indicator != g_opts.indicator;
    case 2: return g_kbOpts.rWindowMs != g_opts.rWindowMs || g_kbOpts.matchMs != g_opts.matchMs || g_kbOpts.cooldownMs != g_opts.cooldownMs;
    case 3: return g_kbOpts.statusTip != g_opts.statusTip || g_kbOpts.rememberPos != g_opts.rememberPos || g_kbOpts.accent != g_opts.accent;
    }
    return false;
}

// un selector segmentado: la caja, el resaltado del acento que se desliza hasta la posicion
// pos (0..n-1) y el hover de cada segmento. rectOf da el rect de cada segmento y hotBase el
// id del primero para buscar su hover.
static void KbSegmentedShapes(Gdiplus::Graphics& g, int n, RECT (*rectOf)(int), float pos, int hotBase, COLORREF acc, COLORREF accHi) {
    RECT first = rectOf(0), last = rectOf(n - 1);
    RECT box = { first.left, first.top, last.right, last.bottom };
    KbFillRound(g, box, (float)KbPx(13), KB_CHIP);
    KbStrokeRound(g, box, (float)KbPx(13), KB_LINE, 190);
    for (int i = 0; i < n; i++) { // el que tiene el mouse encima se aclara un poco
        float hot = g_kbHot[hotBase + i];
        if (hot < 0.01f) continue;
        RECT sr = rectOf(i);
        InflateRect(&sr, -KbPx(2), -KbPx(2));
        KbFillRound(g, sr, (float)KbPx(11), RGB(255, 255, 255), (BYTE)(14 * hot));
    }
    int a = std::clamp((int)floorf(pos), 0, n - 1), b = std::min(a + 1, n - 1);
    float f = pos - (float)a;
    RECT sa = rectOf(a), sb = rectOf(b);
    RECT hl = { (int)(sa.left + (sb.left - sa.left) * f + 0.5f), sa.top, (int)(sa.right + (sb.right - sa.right) * f + 0.5f), sa.bottom };
    InflateRect(&hl, -KbPx(2), -KbPx(2));
    Gdiplus::LinearGradientBrush hb(KbRectF(hl), KbCol(accHi), KbCol(acc), Gdiplus::LinearGradientModeVertical);
    KbFillRound(g, hl, (float)KbPx(11), hb);
}
// un slider: riel, la parte llena con degradado y la perilla, que crece al pasar el mouse / arrastrar
static void KbSliderShapes(Gdiplus::Graphics& g, int id, COLORREF acc, COLORREF accHi) {
    int idx = KbSliderIdx(id);
    RECT t = KbTrack(id);
    float p = std::clamp(g_kbSl[idx], 0.0f, 1.0f), hot = g_kbHot[id];
    bool drag = g_kbDrag == id;
    float x = t.left + (t.right - t.left) * p, cy = (t.top + t.bottom) / 2.0f;
    KbFillRound(g, t, (float)KbPx(3), RGB(255, 255, 255), 24);
    RECT f = { t.left, t.top, (LONG)x, t.bottom };
    if (f.right - f.left > 2) {
        Gdiplus::LinearGradientBrush fb(KbRectF(f), KbCol(acc), KbCol(accHi), Gdiplus::LinearGradientModeHorizontal);
        KbFillRound(g, f, (float)KbPx(3), fb);
    }
    float r = (float)KbPx(7) + KbPx(1) * (drag ? 1.0f : hot);
    if (hot > 0.01f || drag) {
        Gdiplus::SolidBrush halo(KbCol(acc, (BYTE)(drag ? 70 : 50 * hot)));
        g.FillEllipse(&halo, x - r * 1.9f, cy - r * 1.9f, r * 3.8f, r * 3.8f);
    }
    Gdiplus::SolidBrush knob(KbCol(RGB(250, 245, 248)));
    g.FillEllipse(&knob, x - r, cy - r, r * 2, r * 2);
    Gdiplus::Pen ring(KbCol(acc), 2.0f);
    g.DrawEllipse(&ring, x - r + 1.0f, cy - r + 1.0f, r * 2 - 2.0f, r * 2 - 2.0f);
}

static void KbPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    int W = cr.right, H = cr.bottom;
    // todo se dibuja en un bitmap y se copia de una vez, sin parpadeo
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);

    const int tab = g_kbTab;
    const COLORREF acc = AccBase(), accHi = AccHi();

    // medidas (el texto se mide con GDI, hay que hacerlo antes de dibujar las formas)
    std::wstring keyText = KeyName(g_kbPending);
    int capW = std::clamp(KbTextW(dc, keyText, g_kbFontKey) + KbPx(60), KbPx(108), KbPx(292));
    int shake = (int)(sinf((1.0f - g_kbErr) * 3.14159f * 5.0f) * g_kbErr * KbPx(7)); // temblor al rechazar
    int press = (int)(g_kbPress * KbPx(5) + 0.5f);                                    // la cara se hunde al elegir
    int capL = (W - capW) / 2 + shake;
    RECT face = { capL, KbPx(138) + press, capL + capW, KbPx(194) + press };
    RECT side = { capL, KbPx(144), capL + capW, KbPx(202) };
    RECT capAll = { capL, KbPx(138), capL + capW, KbPx(202) };
    bool isNew = g_kbPending != g_modeVk.load();

    const wchar_t* modeNames[5] = { L"OFF", L"X", L"Z", L"C", L"V" };
    RECT chip[5];
    for (int i = 0; i < 5; i++) chip[i] = KbChipRect(i);
    int activeChip = (int)(g_kbRing + 0.5f);
    int tabTextW[KB_TABS];
    for (int i = 0; i < KB_TABS; i++) tabTextW[i] = KbTextW(dc, KB_TAB_NAMES[i], g_kbFontBtn);

    std::wstring stText, modeLetter = g_mode.load() ? std::wstring(1, (wchar_t)g_mode.load()) : std::wstring(L"OFF");
    COLORREF stDot = KB_WARN;
    KbLiveStatus(stText, stDot);
    const std::wstring modePrefix = L"Mode  ";
    int pw1 = KbTextW(dc, stText, g_kbFontChip) + KbPx(32);
    int pw2 = KbTextW(dc, modePrefix, g_kbFontChip) + KbTextW(dc, modeLetter, g_kbFontChip) + KbPx(24);
    int pgap = KbPx(8), px = (W - (pw1 + pgap + pw2)) / 2;
    RECT pill1 = { px, KbPx(84), px + pw1, KbPx(106) };
    RECT pill2 = { px + pw1 + pgap, KbPx(84), px + pw1 + pgap + pw2, KbPx(106) };

    // Stats: los numeros de la sesion (los escribe el thread de deteccion)
    unsigned sent[4], sentTotal = 0, maxSent = 1;
    for (int i = 0; i < 4; i++) { sent[i] = g_stats.sent[i]; sentTotal += sent[i]; maxSent = std::max(maxSent, sent[i]); }
    unsigned detected = g_stats.detected, ignored = g_stats.ignoredOff + g_stats.ignoredFocus;
    RECT tile[3];
    for (int k = 0; k < 3; k++) tile[k] = { KbPx(24 + k * 100), KbPx(84), KbPx(24 + k * 100 + 92), KbPx(132) };

    // 1) formas con bordes suaves (GDI+)
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

        // fondo: degradado carbon con un brillo del color de acento arriba
        Gdiplus::LinearGradientBrush bg(Gdiplus::Rect(0, 0, W, H), KbCol(KB_BG_TOP), KbCol(KB_BG_BOT),
            Gdiplus::LinearGradientModeVertical);
        g.FillRectangle(&bg, 0, 0, W, H);
        {
            Gdiplus::GraphicsPath gp;
            gp.AddEllipse(Gdiplus::RectF((float)(W / 2 - KbPx(200)), (float)-KbPx(140), (float)KbPx(400), (float)KbPx(280)));
            Gdiplus::PathGradientBrush pg(&gp);
            pg.SetCenterColor(KbCol(acc, 30));
            Gdiplus::Color edge = KbCol(acc, 0);
            int n = 1;
            pg.SetSurroundColors(&edge, &n);
            g.FillPath(&pg, &gp);
        }

        // titulo: "meowww!" con degradado del acento
        {
            Gdiplus::FontFamily fam(L"Segoe UI");
            Gdiplus::Font font(&fam, (float)KbPx(15), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::StringFormat fmt;
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            float tx = (float)KbPx(40);
            Gdiplus::LinearGradientBrush tb(Gdiplus::PointF(tx, 0), Gdiplus::PointF(tx + KbPx(78), 0),
                KbCol(accHi), KbCol(KbMix(acc, RGB(0, 0, 0), 0.1f)));
            g.DrawString(L"meowww!", -1, &font, Gdiplus::RectF(tx, 0, (float)KbPx(200), (float)KbPx(KB_TITLE_H)), &fmt, &tb);
        }

        // boton de cerrar: circulo que se prende al pasar el mouse, y una X fina
        RECT cl = KbRect(KB_CLOSE);
        float hc = g_kbHot[KB_CLOSE];
        if (hc > 0.01f) KbFillRound(g, cl, (float)KbPx(14), RGB(222, 70, 92), (BYTE)(255 * hc));
        {
            float cx = (cl.left + cl.right) / 2.0f, cy = (cl.top + cl.bottom) / 2.0f, s = (float)KbPx(5);
            Gdiplus::Pen x(KbCol(KbMix(KB_TEXT_DIM, RGB(255, 255, 255), hc)), (float)KbPx(1) * 1.3f);
            x.SetStartCap(Gdiplus::LineCapRound);
            x.SetEndCap(Gdiplus::LineCapRound);
            g.DrawLine(&x, cx - s, cy - s, cx + s, cy + s);
            g.DrawLine(&x, cx - s, cy + s, cx + s, cy - s);
        }

        // barra de pestanas: linea separadora, la linea del acento que se desliza hasta la pestana activa y
        // un puntito en las pestanas que tienen cambios sin guardar
        {
            Gdiplus::SolidBrush sep(KbCol(RGB(255, 255, 255), 14));
            g.FillRectangle(&sep, 0, KbPx(72) - 1, W, 1);
            RECT t0 = KbTabRect(0);
            float cx = (t0.left + t0.right) / 2.0f + (float)KbPx(64) * g_kbTabPos, hw = (float)KbPx(20);
            Gdiplus::GraphicsPath up;
            KbRoundPath(up, cx - hw, (float)KbPx(70) - 1, hw * 2, (float)KbPx(3), (float)KbPx(1));
            Gdiplus::SolidBrush ub(KbCol(acc));
            g.FillPath(&ub, &up);
            for (int i = 0; i < KB_TABS; i++) {
                if (!KbTabDirty(i)) continue;
                RECT tr = KbTabRect(i);
                float dx = (tr.left + tr.right) / 2.0f + tabTextW[i] / 2.0f + KbPx(5), dy = (float)(tr.top + KbPx(11));
                Gdiplus::SolidBrush dot(KbCol(accHi));
                g.FillEllipse(&dot, dx - KbPx(2), dy - KbPx(2), (float)KbPx(4), (float)KbPx(4));
            }
        }

        if (tab == 0) {
            // estado en vivo: conexion con Roblox y modo activo
            for (RECT pr : { pill1, pill2 }) {
                KbFillRound(g, pr, (float)KbPx(11), KB_CHIP);
                KbStrokeRound(g, pr, (float)KbPx(11), KB_LINE, 190);
            }
            float dx = pill1.left + KbPx(13) + 0.0f, dy = (pill1.top + pill1.bottom) / 2.0f, dr = (float)KbPx(3);
            Gdiplus::SolidBrush halo(KbCol(stDot, 55)), dotb(KbCol(stDot));
            g.FillEllipse(&halo, dx - dr * 2, dy - dr * 2, dr * 4, dr * 4);
            g.FillEllipse(&dotb, dx - dr, dy - dr, dr * 2, dr * 2);

            // la tecla: brillo al elegirla (acento) o rechazarla (rojo), costado, cara con degradado
            float R = (float)KbPx(14);
            COLORREF glow = g_kbErr > 0.0f ? KB_ERROR : acc;
            float glowT = std::max(g_kbFlash, g_kbErr);
            for (int i = 6; i >= 1 && glowT > 0.01f; i--) {
                RECT o = capAll;
                InflateRect(&o, KbPx(i), KbPx(i));
                KbStrokeRound(g, o, R + KbPx(i), glow, (BYTE)(80 * glowT / i));
            }
            KbFillRound(g, side, R, KB_CAP_SIDE);
            KbStrokeRound(g, side, R, KbMix(KB_LINE, KB_ERROR, g_kbErr), 150);
            {
                COLORREF top = KbMix(KB_CAP_TOP, RGB(128, 50, 58), g_kbErr);
                COLORREF bot = KbMix(KB_CAP_BOT, RGB(92, 34, 42), g_kbErr);
                Gdiplus::LinearGradientBrush fb(KbRectF(face), KbCol(top), KbCol(bot), Gdiplus::LinearGradientModeVertical);
                KbFillRound(g, face, R, fb);
                Gdiplus::Pen hi(KbCol(RGB(255, 255, 255), 40), 1.0f); // lineita clara arriba, el brillo del plastico
                g.DrawLine(&hi, (float)(face.left + KbPx(12)), face.top + 1.5f, (float)(face.right - KbPx(12)), face.top + 1.5f);
                COLORREF edge = KbMix(isNew ? acc : RGB(88, 72, 88), KB_ERROR, g_kbErr);
                KbStrokeRound(g, face, R, edge, 255);
            }

            // los modos en orden: OFF siempre esta, los demas se prenden (acento) y apagan (gris) con un click
            for (int i = 0; i < 5; i++) {
                float on = i == 0 ? 0.0f : g_kbOn[i - 1], hot = i == 0 ? 0.0f : g_kbHot[KB_MODE_X + i - 1];
                COLORREF fill = KbMix(KbMix(KB_CHIP, RGB(255, 255, 255), 0.07f * hot), KbMix(KB_CHIP, acc, 0.2f), on);
                COLORREF edge = KbMix(KbMix(KB_LINE, RGB(98, 82, 98), hot), acc, on);
                KbFillRound(g, chip[i], (float)KbPx(12), fill);
                KbStrokeRound(g, chip[i], (float)KbPx(12), edge, (BYTE)(170 + 85 * on));
                if (i < 4) {
                    float cx = (chip[i].right + chip[i + 1].left) / 2.0f, cy = (chip[i].top + chip[i].bottom) / 2.0f, s = (float)KbPx(3);
                    Gdiplus::Pen ar(KbCol(KB_TEXT_FAINT), (float)KbPx(1) * 1.3f);
                    ar.SetStartCap(Gdiplus::LineCapRound);
                    ar.SetEndCap(Gdiplus::LineCapRound);
                    ar.SetLineJoin(Gdiplus::LineJoinRound);
                    Gdiplus::PointF pts[3] = { { cx - s, cy - s * 1.4f }, { cx + s, cy }, { cx - s, cy + s * 1.4f } };
                    g.DrawLines(&ar, pts, 3);
                }
            }
            // aro del modo activo: se desliza de un modo al otro
            {
                int a = std::clamp((int)floorf(g_kbRing), 0, 4), b = std::min(a + 1, 4);
                float f = g_kbRing - (float)a;
                auto lerp = [&](LONG p, LONG q) { return (int)(p + (q - p) * f + 0.5f); };
                RECT ring = { lerp(chip[a].left, chip[b].left), lerp(chip[a].top, chip[b].top),
                              lerp(chip[a].right, chip[b].right), lerp(chip[a].bottom, chip[b].bottom) };
                InflateRect(&ring, KbPx(3), KbPx(3));
                KbStrokeRound(g, ring, (float)KbPx(15), acc, 60, (float)KbPx(3));
                KbStrokeRound(g, ring, (float)KbPx(15), accHi, 235, 1.5f);
            }
        } else if (tab == 1) {
            // Tooltips: estilo (selector de 4), duracion (slider) e indicador (selector de 5 con mini pantallas)
            KbSegmentedShapes(g, TIP_STYLES, KbSegRect, g_kbSeg, KB_STY_0, acc, accHi);
            KbSliderShapes(g, KB_SLD_TIP, acc, accHi);
            KbSegmentedShapes(g, IND_CHOICES, KbIndRect, g_kbInd, KB_IND_0, acc, accHi);
            for (int i = 1; i < IND_CHOICES; i++) { // cada esquina: una pantalla en miniatura con un punto en esa esquina
                RECT s = KbIndRect(i);
                float closeness = std::clamp(1.0f - fabsf(g_kbInd - (float)i), 0.0f, 1.0f);
                COLORREF c = KbMix(KbMix(KB_TEXT_FAINT, KB_TEXT_DIM, g_kbHot[KB_IND_0 + i]), AccOn(), closeness);
                float cx = (s.left + s.right) / 2.0f, cy = (s.top + s.bottom) / 2.0f, gw = (float)KbPx(22), gh = (float)KbPx(14);
                Gdiplus::GraphicsPath screen;
                KbRoundPath(screen, cx - gw / 2, cy - gh / 2, gw, gh, KbPx(5) / 2.0f);
                Gdiplus::Pen pen(KbCol(c), 1.3f);
                g.DrawPath(&pen, &screen);
                bool right = i == 2 || i == 4, bottom = i == 3 || i == 4;
                float dx = right ? cx + gw / 2 - KbPx(5) : cx - gw / 2 + KbPx(5), dy = bottom ? cy + gh / 2 - KbPx(5) : cy - gh / 2 + KbPx(5);
                Gdiplus::SolidBrush dot(KbCol(c));
                g.FillEllipse(&dot, dx - KbPx(2), dy - KbPx(2), (float)KbPx(4), (float)KbPx(4));
            }
            for (RECT r : { KbStyleRow(), KbIndRow() }) { // separadores finos entre las filas
                Gdiplus::SolidBrush sep(KbCol(RGB(255, 255, 255), 10));
                g.FillRectangle(&sep, r.left + KbPx(8), r.bottom + KbPx(2), (r.right - r.left) - KbPx(16), 1);
            }
            {
                RECT r = KbSliderRow(KB_SLD_TIP);
                Gdiplus::SolidBrush sep(KbCol(RGB(255, 255, 255), 10));
                g.FillRectangle(&sep, r.left + KbPx(8), r.bottom + KbPx(2), (r.right - r.left) - KbPx(16), 1);
            }
        } else if (tab == 2) {
            // Timing: tres sliders
            for (int id : { KB_SLD_R, KB_SLD_MATCH, KB_SLD_COOL }) KbSliderShapes(g, id, acc, accHi);
            for (int id : { KB_SLD_R, KB_SLD_MATCH }) {
                RECT r = KbSliderRow(id);
                Gdiplus::SolidBrush sep(KbCol(RGB(255, 255, 255), 10));
                g.FillRectangle(&sep, r.left + KbPx(8), r.bottom + KbPx(1), (r.right - r.left) - KbPx(16), 1);
            }
        } else if (tab == 3) {
            // General: Status notices y Remember position con su switch, y la fila de colores
            for (int row = 0; row < 2; row++) {
                RECT rr = KbGenRow(row);
                KbFillRound(g, rr, (float)KbPx(10), RGB(255, 255, 255), (BYTE)(10 * g_kbHot[KB_SW_STATUS + row]));
                RECT sw = KbSwitchRect(row);
                float t = g_kbSw[row], rad = (sw.bottom - sw.top) / 2.0f;
                KbFillRound(g, sw, rad, KbMix(KB_TRACK, acc, t));
                float d = (float)KbPx(14);
                float kx = sw.left + KbPx(3) + ((sw.right - sw.left) - KbPx(6) - d) * t, ky = (sw.top + sw.bottom) / 2.0f - d / 2;
                Gdiplus::SolidBrush knob(KbCol(KbMix(RGB(176, 164, 174), RGB(255, 255, 255), t)));
                g.FillEllipse(&knob, kx, ky, d, d);
                Gdiplus::SolidBrush sep(KbCol(RGB(255, 255, 255), 10)); // separador fino entre filas
                g.FillRectangle(&sep, rr.left + KbPx(8), rr.bottom + KbPx(2), (rr.right - rr.left) - KbPx(16), 1);
            }
            for (int i = 0; i < ACCENT_COUNT; i++) {
                RECT sr = KbSwatchRect(i);
                float hot = g_kbHot[KB_ACC_0 + i];
                int grow = (int)(KbPx(1) * hot + 0.5f);
                InflateRect(&sr, grow, grow);
                Gdiplus::SolidBrush sb(KbCol(KB_ACCENTS[i].base));
                g.FillEllipse(&sb, (float)sr.left, (float)sr.top, (float)(sr.right - sr.left), (float)(sr.bottom - sr.top));
                if (i == g_kbOpts.accent) { // el elegido lleva un aro
                    InflateRect(&sr, KbPx(3), KbPx(3));
                    Gdiplus::Pen rp(KbCol(RGB(255, 255, 255), 230), 1.6f);
                    g.DrawEllipse(&rp, (float)sr.left + 0.5f, (float)sr.top + 0.5f, (float)(sr.right - sr.left) - 1, (float)(sr.bottom - sr.top) - 1);
                }
            }
        } else {
            // Stats: tres cuadros con numeros y una barra por modo
            for (int k = 0; k < 3; k++) {
                KbFillRound(g, tile[k], (float)KbPx(11), KB_CHIP);
                KbStrokeRound(g, tile[k], (float)KbPx(11), KB_LINE, 190);
            }
            for (int i = 0; i < 4; i++) {
                int y = KbPx(146 + i * 24);
                RECT track = { KbPx(64), y + KbPx(6), KbPx(250), y + KbPx(14) };
                KbFillRound(g, track, (float)KbPx(4), RGB(255, 255, 255), 18);
                RECT fill = { track.left, track.top, track.left + (LONG)((track.right - track.left) * g_kbBar[i]), track.bottom };
                if (fill.right - fill.left > 2) {
                    Gdiplus::LinearGradientBrush fb(KbRectF(fill), KbCol(acc), KbCol(accHi), Gdiplus::LinearGradientModeHorizontal);
                    KbFillRound(g, fill, (float)KbPx(4), fb);
                }
                RECT lc = { KbPx(24), y + KbPx(1), KbPx(52), y + KbPx(19) };
                KbFillRound(g, lc, (float)KbPx(9), KbMix(KB_CHIP, acc, 0.2f));
                KbStrokeRound(g, lc, (float)KbPx(9), acc, 190);
            }
        }

        // botones
        for (int id : { KB_CANCEL, KB_SAVE }) {
            RECT b = KbRect(id);
            float t = g_kbHot[id];
            bool down = g_kbHover == id && g_kbDown == id;
            if (id == KB_SAVE) {
                COLORREF c1 = KbMix(accHi, RGB(255, 255, 255), 0.28f * t);
                COLORREF c2 = KbMix(acc, RGB(255, 235, 242), 0.28f * t);
                if (down) { c1 = KbMix(c1, RGB(0, 0, 0), 0.14f); c2 = KbMix(c2, RGB(0, 0, 0), 0.14f); }
                Gdiplus::LinearGradientBrush sb(KbRectF(b), KbCol(c1), KbCol(c2), Gdiplus::LinearGradientModeVertical);
                KbFillRound(g, b, (float)KbPx(10), sb);
            } else {
                KbFillRound(g, b, (float)KbPx(10), RGB(255, 255, 255), (BYTE)(down ? 3 : 6 + 14 * t));
                KbStrokeRound(g, b, (float)KbPx(10), KbMix(KB_LINE, KB_TEXT_DIM, 0.7f * t));
            }
        }

        // borde exterior de la ventana
        KbStrokeRound(g, cr, g_kbRound ? (float)KbPx(8) : 0.0f, KB_LINE, 230);
    }

    // 2) icono y texto (GDI)
    if (g_kbIcon) {
        int s = KbPx(18);
        DrawIconEx(dc, KbPx(14), (KbPx(KB_TITLE_H) - s) / 2, g_kbIcon, s, s, 0, nullptr, DI_NORMAL);
    }
    for (int i = 0; i < KB_TABS; i++) { // nombres de las pestanas
        float active = std::clamp(1.0f - fabsf(g_kbTabPos - (float)i), 0.0f, 1.0f);
        COLORREF c = KbMix(KbMix(KB_TEXT_FAINT, KB_TEXT_DIM, g_kbHot[KB_TAB_0 + i]), KB_TEXT, active);
        KbText(dc, KB_TAB_NAMES[i], KbTabRect(i), g_kbFontBtn, c, DT_CENTER);
    }
    if (tab == 0) {
        KbText(dc, stText, { pill1.left + KbPx(23), pill1.top, pill1.right, pill1.bottom }, g_kbFontChip, KB_TEXT_DIM, DT_LEFT);
        {
            int x0 = pill2.left + KbPx(12);
            int pre = KbTextW(dc, modePrefix, g_kbFontChip);
            KbText(dc, modePrefix, { x0, pill2.top, pill2.right, pill2.bottom }, g_kbFontChip, KB_TEXT_DIM, DT_LEFT);
            KbText(dc, modeLetter, { x0 + pre, pill2.top, pill2.right, pill2.bottom }, g_kbFontChip, accHi, DT_LEFT);
        }
        KbText(dc, g_kbFirstRun ? L"First time here: press the key you want to use."
                                : L"Press a key, then pick which modes it cycles.",
            { 0, KbPx(112), W, KbPx(130) }, g_kbFontSmall, KB_TEXT_DIM, DT_CENTER);
        KbText(dc, keyText, face, g_kbFontKey, KB_TEXT, DT_CENTER);
        for (int i = 0; i < 5; i++) {
            COLORREF c = KB_TEXT_DIM; // OFF
            if (i > 0) c = KbMix(KbMix(KB_TEXT_FAINT, KB_TEXT_DIM, g_kbHot[KB_MODE_X + i - 1]), accHi, g_kbOn[i - 1]);
            if (i == activeChip) c = KB_TEXT;
            KbText(dc, modeNames[i], chip[i], g_kbFontChip, c, DT_CENTER);
        }
        COLORREF msgColor = g_kbMsgKind == KB_MSG_ERROR ? KB_ERROR : g_kbMsgKind == KB_MSG_NEW ? accHi : KB_TEXT_FAINT;
        KbText(dc, g_kbMsg, { KbPx(16), KbPx(244), W - KbPx(16), KbPx(260) }, g_kbFontSmall, msgColor, DT_CENTER);
        if (IsTypingKey(g_kbPending)) // una tecla con la que se escribe: avisar que va a dejar de escribir en otras apps
            KbText(dc, L"Heads up: " + keyText + L" will stop working in other apps.",
                { KbPx(16), KbPx(260), W - KbPx(16), KbPx(276) }, g_kbFontTiny, KB_WARN, DT_CENTER);
    } else if (tab == 1) {
        static const wchar_t* STYLE_NAME[TIP_STYLES] = { L"Classic", L"Pill", L"HUD", L"Keycap" };
        RECT r0 = KbStyleRow(), rs = KbSliderRow(KB_SLD_TIP), r2 = KbIndRow();
        KbText(dc, L"Tooltip style", { r0.left + KbPx(12), r0.top + KbPx(3), r0.right, r0.top + KbPx(19) }, g_kbFontBtn, KB_TEXT, DT_LEFT);
        KbText(dc, L"How the mode shows up. Click a style to preview it.",
            { r0.left + KbPx(12), r0.top + KbPx(19), r0.right - KbPx(8), r0.top + KbPx(31) }, g_kbFontTiny, KB_TEXT_FAINT, DT_LEFT);
        for (int i = 0; i < TIP_STYLES; i++) { // el nombre se oscurece sobre el resaltado a medida que este llega
            float closeness = std::clamp(1.0f - fabsf(g_kbSeg - (float)i), 0.0f, 1.0f);
            COLORREF c = KbMix(KbMix(KB_TEXT_FAINT, KB_TEXT_DIM, g_kbHot[KB_STY_0 + i]), AccOn(), closeness);
            KbText(dc, STYLE_NAME[i], KbSegRect(i), g_kbFontChip, c, DT_CENTER);
        }
        KbText(dc, L"Duration", { rs.left + KbPx(12), rs.top + KbPx(3), rs.right - KbPx(90), rs.top + KbPx(19) }, g_kbFontBtn, KB_TEXT, DT_LEFT);
        KbText(dc, KbSliderText(0, g_kbOpts.tipMs), { rs.right - KbPx(90), rs.top + KbPx(3), rs.right - KbPx(12), rs.top + KbPx(19) },
            g_kbFontBtn, accHi, DT_RIGHT);
        KbText(dc, L"Always-on indicator", { r2.left + KbPx(12), r2.top + KbPx(3), r2.right, r2.top + KbPx(19) }, g_kbFontBtn, KB_TEXT, DT_LEFT);
        KbText(dc, L"A small pill with your mode while Roblox is focused.",
            { r2.left + KbPx(12), r2.top + KbPx(19), r2.right - KbPx(8), r2.top + KbPx(31) }, g_kbFontTiny, KB_TEXT_FAINT, DT_LEFT);
        {
            float closeness = std::clamp(1.0f - fabsf(g_kbInd - 0.0f), 0.0f, 1.0f); // "Off", la unica opcion con texto
            COLORREF c = KbMix(KbMix(KB_TEXT_FAINT, KB_TEXT_DIM, g_kbHot[KB_IND_0]), AccOn(), closeness);
            KbText(dc, L"Off", KbIndRect(0), g_kbFontChip, c, DT_CENTER);
        }
    } else if (tab == 2) {
        for (int i = 1; i < 4; i++) {
            const KbSliderDef& d = KB_SLIDERS[i];
            RECT rr = KbSliderRow(d.id);
            KbText(dc, d.title, { rr.left + KbPx(12), rr.top + KbPx(3), rr.right - KbPx(90), rr.top + KbPx(19) }, g_kbFontBtn, KB_TEXT, DT_LEFT);
            KbText(dc, KbSliderText(i, g_kbOpts.*d.field), { rr.right - KbPx(90), rr.top + KbPx(3), rr.right - KbPx(12), rr.top + KbPx(19) },
                g_kbFontBtn, accHi, DT_RIGHT);
            KbText(dc, d.sub, { rr.left + KbPx(12), rr.top + KbPx(19), rr.right - KbPx(8), rr.top + KbPx(31) }, g_kbFontTiny, KB_TEXT_FAINT, DT_LEFT);
        }
        KbText(dc, L"Restore timing defaults", KbRect(KB_TIMING_RESET), g_kbFontSmall,
            KbMix(KB_TEXT_FAINT, accHi, g_kbHot[KB_TIMING_RESET]), DT_LEFT);
    } else if (tab == 3) {
        RECT r0 = KbGenRow(0), r1 = KbGenRow(1), r2 = KbGenRow(2);
        KbText(dc, L"Status notices", { r0.left + KbPx(12), r0.top + KbPx(4), KbSwitchRect(0).left - KbPx(8), r0.top + KbPx(22) }, g_kbFontBtn, KB_TEXT, DT_LEFT);
        KbText(dc, L"Tips when it starts and connects",
            { r0.left + KbPx(12), r0.top + KbPx(22), KbSwitchRect(0).left - KbPx(8), r0.top + KbPx(37) }, g_kbFontTiny, KB_TEXT_FAINT, DT_LEFT);
        KbText(dc, L"Remember position", { r1.left + KbPx(12), r1.top + KbPx(4), KbSwitchRect(1).left - KbPx(8), r1.top + KbPx(22) }, g_kbFontBtn, KB_TEXT, DT_LEFT);
        KbText(dc, L"Open this window where you left it",
            { r1.left + KbPx(12), r1.top + KbPx(22), KbSwitchRect(1).left - KbPx(8), r1.top + KbPx(37) }, g_kbFontTiny, KB_TEXT_FAINT, DT_LEFT);
        KbText(dc, L"Accent", { r2.left + KbPx(12), r2.top + KbPx(4), r2.right, r2.top + KbPx(22) }, g_kbFontBtn, KB_TEXT, DT_LEFT);
        KbText(dc, KB_ACCENTS[g_kbOpts.accent].name,
            { r2.left + KbPx(12), r2.top + KbPx(22), r2.right, r2.top + KbPx(37) }, g_kbFontTiny, KB_TEXT_FAINT, DT_LEFT);
        KbText(dc, L"Reset everything to defaults", KbRect(KB_RESET), g_kbFontSmall,
            KbMix(KB_TEXT_FAINT, accHi, g_kbHot[KB_RESET]), DT_LEFT);
    } else {
        static const wchar_t* TILE_LABEL[3] = { L"Detected", L"Sent", L"Ignored" };
        unsigned tileValue[3] = { detected, sentTotal, ignored };
        for (int k = 0; k < 3; k++) {
            RECT t = tile[k];
            KbText(dc, std::to_wstring(tileValue[k]), { t.left, t.top + KbPx(3), t.right, t.top + KbPx(30) }, g_kbFontNum, KB_TEXT, DT_CENTER);
            KbText(dc, TILE_LABEL[k], { t.left, t.top + KbPx(30), t.right, t.top + KbPx(44) }, g_kbFontTiny, KB_TEXT_FAINT, DT_CENTER);
        }
        for (int i = 0; i < 4; i++) {
            int y = KbPx(146 + i * 24);
            KbText(dc, std::wstring(1, (wchar_t)MODE_LETTERS[i]), { KbPx(24), y + KbPx(1), KbPx(52), y + KbPx(19) }, g_kbFontChip, accHi, DT_CENTER);
            KbText(dc, std::to_wstring(sent[i]), { KbPx(256), y, KbPx(316), y + KbPx(20) }, g_kbFontBtn, KB_TEXT_DIM, DT_LEFT);
        }
        DWORD lt = g_stats.lastTick;
        std::wstring dotSep = std::wstring(L"  ") + (wchar_t)0xB7 + L"  "; // separador: espacio, punto medio, espacio
        std::wstring last = !lt ? std::wstring(L"No key sent yet this session.")
            : std::wstring(L"Last: ") + std::wstring(1, (wchar_t)(g_stats.lastMode.load() ? g_stats.lastMode.load() : '?')) + dotSep +
              KbAgo(GetTickCount() - lt) + dotSep + L"gap " + std::to_wstring(g_stats.lastGapMs.load()) + L" ms";
        KbText(dc, last, { KbPx(24), KbPx(244), KbPx(316), KbPx(260) }, g_kbFontSmall, KB_TEXT_DIM, DT_LEFT);
        DWORD up = (GetTickCount() - g_stats.start.load()) / 1000;
        wchar_t clock[32];
        swprintf(clock, 32, L"Session %02lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
        KbText(dc, clock, { KbPx(24), KbPx(262), KbPx(200), KbPx(280) }, g_kbFontSmall, KB_TEXT_FAINT, DT_LEFT);
        KbText(dc, L"Reset stats", KbRect(KB_STATS_RESET), g_kbFontSmall, KbMix(KB_TEXT_FAINT, accHi, g_kbHot[KB_STATS_RESET]), DT_RIGHT);
    }
    KbText(dc, L"Cancel", KbRect(KB_CANCEL), g_kbFontBtn, KbMix(KB_TEXT_DIM, KB_TEXT, g_kbHot[KB_CANCEL]), DT_CENTER);
    KbText(dc, L"Save", KbRect(KB_SAVE), g_kbFontBtn, AccOn(), DT_CENTER);

    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// acerca v a target un poco por tick; true mientras todavia se mueve
static bool KbStep(float& v, float target, float rate) {
    float d = target - v;
    if (fabsf(d) < 0.01f) { v = target; return false; }
    v += d * rate;
    return true;
}
static void KbKick(HWND hwnd) { // prende el timer de animacion si no estaba corriendo
    if (g_kbTimerOn) return;
    g_kbTimerOn = true;
    SetTimer(hwnd, KB_TIMER, 15, nullptr);
}
// Cerrar = fundido de salida; el timer destruye la ventana cuando termina
static void KbClose(HWND hwnd) {
    if (g_kbClosing) return;
    g_kbClosing = true;
    KbKick(hwnd);
}
// la linea de estado segun lo que cambio en la ventana
static void KbRefreshMsg() {
    bool keyNew = g_kbPending != g_modeVk.load(), modesNew = g_kbMask != g_modeMask.load();
    bool optsNew = !OptsEqual(g_kbOpts, g_opts);
    g_kbMsg = keyNew && modesNew ? L"New key and modes. Click Save to apply."
            : keyNew             ? L"New key. Click Save to apply it."
            : modesNew           ? L"Modes changed. Click Save to apply."
            : optsNew            ? L"Settings changed. Click Save to apply."
                                 : KB_NOTE;
    g_kbMsgKind = (keyNew || modesNew || optsNew) ? KB_MSG_NEW : KB_MSG_NOTE;
}
static void KbRepaint(HWND hwnd) {
    InvalidateRect(hwnd, nullptr, FALSE);
    KbKick(hwnd);
}
static void KbSetTab(HWND hwnd, int tab) {
    if (g_kbTab == tab) return;
    g_kbTab = tab;
    g_kbHover = KB_NONE; // el elemento sobre el que estaba el mouse puede haber desaparecido
    if (tab == 4) SetTimer(hwnd, KB_TIMER_SLOW, 1000, nullptr); // Stats: el "hace 12 s" y el reloj necesitan repintar solos
    else KillTimer(hwnd, KB_TIMER_SLOW);
    KbRepaint(hwnd);
}
// click en un modo (i = 0..3 en el orden de MODE_LETTERS): lo prende o lo apaga
static void KbToggleMode(HWND hwnd, int i) {
    unsigned next = g_kbMask ^ (1u << i);
    if (next == 0) { // si no queda ninguno la tecla no haria nada
        g_kbMsg = L"Keep at least one mode on.";
        g_kbMsgKind = KB_MSG_ERROR;
    } else {
        g_kbMask = next;
        KbRefreshMsg();
    }
    KbRepaint(hwnd);
}
static void KbToggleSwitch(HWND hwnd, int which) { // 0 = Status notices, 1 = Remember position
    bool& v = which == 0 ? g_kbOpts.statusTip : g_kbOpts.rememberPos;
    v = !v;
    KbRefreshMsg();
    KbRepaint(hwnd);
}
// el modo con el que se muestra la vista previa: el activo, o si esta en OFF el primero que este prendido
static char KbPreviewMode() {
    char m = g_mode.load();
    for (int i = 0; i < 4 && !m; i++) if ((g_kbMask >> i) & 1) m = MODE_LETTERS[i];
    return m;
}
static void KbPreviewTip() { // el tooltip con lo que hay elegido en la ventana (estilo, color, duracion y modos)
    ShowModeTipStyle(g_kbOpts.tipStyle, KbPreviewMode(), g_kbOpts.accent, g_kbMask, g_kbOpts.tipMs);
}
// click en un estilo de tooltip: lo elige y lo muestra al toque
static void KbSetStyle(HWND hwnd, int i) {
    g_kbOpts.tipStyle = i;
    KbPreviewTip();
    KbRefreshMsg();
    KbRepaint(hwnd);
}
// click en una opcion del indicador: lo muestra un rato aunque Roblox no tenga el foco
static void KbSetIndicator(HWND hwnd, int i) {
    g_kbOpts.indicator = i;
    IndicatorPreview(i, g_mode.load(), g_kbOpts.accent);
    KbRefreshMsg();
    KbRepaint(hwnd);
}
static void KbSetAccent(HWND hwnd, int i) {
    if (i == g_kbOpts.accent) return;
    g_kbAccPrev = g_kbOpts.accent; // el color pasa del anterior al nuevo en vez de saltar
    g_kbOpts.accent = i;
    g_kbAccT = 0;
    if (g_kbOpts.tipStyle != 0) KbPreviewTip(); // Classic no usa el acento
    if (g_kbOpts.indicator) IndicatorPreview(g_kbOpts.indicator, g_mode.load(), i);
    KbRefreshMsg();
    KbRepaint(hwnd);
}
// el slider idx sigue al mouse: x es la coordenada de la ventana
static void KbDragTo(HWND hwnd, int x) {
    int idx = KbSliderIdx(g_kbDrag);
    if (idx < 0) return;
    RECT t = KbTrack(g_kbDrag);
    int v = KbSliderFromT(idx, (float)(x - t.left) / (float)(t.right - t.left));
    int& field = g_kbOpts.*KB_SLIDERS[idx].field;
    if (v != field) { field = v; KbRefreshMsg(); }
    g_kbSl[idx] = KbSliderNorm(idx, v); // la perilla se queda en el paso, no en el pixel
    KbRepaint(hwnd);
}
static void KbResetTiming(HWND hwnd) {
    g_kbOpts.rWindowMs = RANGE_R_MS.def;
    g_kbOpts.matchMs = RANGE_MATCH_MS.def;
    g_kbOpts.cooldownMs = RANGE_COOL_MS.def;
    KbRefreshMsg();
    KbRepaint(hwnd);
}
static void KbReset(HWND hwnd) { // todo lo de la ventana vuelve a los valores de fabrica (se aplica al tocar Save)
    g_kbPending = DEFAULT_MODE_VK;
    g_kbMask = ALL_MODES;
    int oldAccent = g_kbOpts.accent, oldInd = g_kbOpts.indicator;
    g_kbOpts = Opts();
    if (g_kbOpts.accent != oldAccent) { g_kbAccPrev = oldAccent; g_kbAccT = 0; }
    if (g_kbOpts.indicator != oldInd) IndicatorPreview(g_kbOpts.indicator, g_mode.load(), g_kbOpts.accent);
    KbRefreshMsg();
    g_kbFlash = 1.0f;
    KbRepaint(hwnd);
}

static LRESULT CALLBACK KeybindWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_kbFontSmall = KbFont(12, FW_NORMAL);
        g_kbFontTiny = KbFont(11, FW_NORMAL);
        g_kbFontChip = KbFont(11, FW_SEMIBOLD);
        g_kbFontKey = KbFont(26, FW_SEMIBOLD);
        g_kbFontBtn = KbFont(13, FW_SEMIBOLD);
        g_kbFontNum = KbFont(22, FW_SEMIBOLD);
        g_kbIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MEOW), IMAGE_ICON, KbPx(18), KbPx(18), 0);
        return 0;
    case WM_ERASEBKGND:
        return 1; // lo pinta todo WM_PAINT
    case WM_PAINT:
        KbPaint(hwnd);
        return 0;
    case WM_NCHITTEST: { // la barra de titulo arrastra la ventana (salvo la X)
        POINT pt = { (int)(short)LOWORD(lp), (int)(short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < KbPx(KB_TITLE_H) && KbHit(pt.x, pt.y) != KB_CLOSE) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CLOSE: // la X de la barra, Alt+F4, etc: tambien con fundido
        KbClose(hwnd);
        return 0;
    case WM_APP_REFRESH: // cambio el modo activo, el estado de la conexion o los Stats
        KbRepaint(hwnd);
        return 0;
    case WM_APP_KEY_CAPTURED: {
        if (g_kbClosing || g_kbTab != 0) return 0; // la tecla solo se elige en la pestana Keybind
        DWORD vk = (DWORD)wp;
        if (!IsAllowedModeKey(vk)) {
            g_kbMsg = vk == SORU_VK ? std::wstring(L"R is your Soru key, pick another.")
                                    : KeyName(vk) + L" can't be used, pick another.";
            g_kbMsgKind = KB_MSG_ERROR;
            g_kbErr = 1.0f;
        } else {
            g_kbPending = vk;
            KbRefreshMsg();
            g_kbFlash = 1.0f;
            g_kbPress = 1.0f;
        }
        KbRepaint(hwnd);
        return 0;
    }
    case WM_TIMER:
        if (wp == KB_TIMER_SLOW) {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wp == KB_TIMER) {
            bool busy = false;
            for (int id = KB_CLOSE; id < KB_NELEM; id++)
                busy |= KbStep(g_kbHot[id], g_kbHover == id ? 1.0f : 0.0f, 0.35f);
            for (int i = 0; i < 4; i++)
                busy |= KbStep(g_kbOn[i], (g_kbMask >> i) & 1 ? 1.0f : 0.0f, 0.3f);
            busy |= KbStep(g_kbSw[0], g_kbOpts.statusTip ? 1.0f : 0.0f, 0.3f);
            busy |= KbStep(g_kbSw[1], g_kbOpts.rememberPos ? 1.0f : 0.0f, 0.3f);
            busy |= KbStep(g_kbSeg, (float)g_kbOpts.tipStyle, 0.3f);
            busy |= KbStep(g_kbInd, (float)g_kbOpts.indicator, 0.3f);
            for (int i = 0; i < 4; i++) // la perilla sigue al mouse sin demora mientras la arrastras
                if (g_kbDrag != KB_SLIDERS[i].id) busy |= KbStep(g_kbSl[i], KbSliderNorm(i, g_kbOpts.*KB_SLIDERS[i].field), 0.35f);
            {
                unsigned top = 1;
                for (int i = 0; i < 4; i++) top = std::max(top, (unsigned)g_stats.sent[i]);
                for (int i = 0; i < 4; i++) busy |= KbStep(g_kbBar[i], (float)g_stats.sent[i] / (float)top, 0.25f);
            }
            busy |= KbStep(g_kbTabPos, (float)g_kbTab, 0.28f);
            busy |= KbStep(g_kbRing, KbRingTarget(), 0.26f);
            if (g_kbAccT < 1.0f) { g_kbAccT = std::min(1.0f, g_kbAccT + 0.09f); busy |= g_kbAccT < 1.0f; }
            if (g_kbFlash > 0) { g_kbFlash = std::max(0.0f, g_kbFlash - 0.045f); busy |= g_kbFlash > 0; }
            if (g_kbErr > 0) { g_kbErr = std::max(0.0f, g_kbErr - 0.03f); busy |= g_kbErr > 0; }
            if (g_kbPress > 0) { g_kbPress = std::max(0.0f, g_kbPress - 0.12f); busy |= g_kbPress > 0; }
            // fundido de entrada / salida, con un deslizamiento corto: sube al aparecer, baja al irse
            float target = g_kbClosing ? 0.0f : 1.0f;
            if (g_kbFade != target) {
                float step = g_kbClosing ? 0.14f : 0.10f;
                g_kbFade = g_kbFade < target ? std::min(target, g_kbFade + step) : std::max(target, g_kbFade - step);
                float e = g_kbFade * g_kbFade * (3.0f - 2.0f * g_kbFade); // smoothstep
                SetLayeredWindowAttributes(hwnd, 0, (BYTE)(255 * e + 0.5f), LWA_ALPHA);
                int slide = (int)((1.0f - e) * KbPx(10) + 0.5f);
                if (slide != g_kbSlide) {
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    SetWindowPos(hwnd, nullptr, wr.left, wr.top - g_kbSlide + slide, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    g_kbSlide = slide;
                }
                busy |= g_kbFade != target;
            }
            if (g_kbClosing && g_kbFade <= 0.0f) { // ya se fue del todo
                KillTimer(hwnd, KB_TIMER);
                g_kbTimerOn = false;
                DestroyWindow(hwnd);
                return 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            if (!busy) {
                KillTimer(hwnd, KB_TIMER);
                g_kbTimerOn = false;
            }
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (g_kbDrag) { // arrastrando un slider: con el mouse capturado esto llega aunque salgas de la ventana
            KbDragTo(hwnd, (int)(short)LOWORD(lp));
            return 0;
        }
        int hit = KbHit((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        if (hit != g_kbHover) {
            g_kbHover = hit;
            KbKick(hwnd);
        }
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 }; // para enterarnos cuando el mouse se va
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_kbHover = KB_NONE;
        KbKick(hwnd);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && (g_kbHover || g_kbDrag)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        if (g_kbClosing) return 0;
        int hit = KbHit((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        if (KbSliderIdx(hit) >= 0) { // un slider: se arrastra desde el primer click, sin esperar a soltar
            g_kbDrag = hit;
            SetCapture(hwnd);
            KbDragTo(hwnd, (int)(short)LOWORD(lp));
            return 0;
        }
        g_kbDown = hit;
        if (g_kbDown) SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_kbDrag) { // soltaste el slider
            int dragged = g_kbDrag;
            g_kbDrag = KB_NONE;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (dragged == KB_SLD_TIP) KbPreviewTip(); // la duracion se prueba con el tooltip
            KbRepaint(hwnd);
            return 0;
        }
        int pressed = g_kbDown;
        g_kbDown = KB_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        // el click vale solo si soltas sobre el mismo elemento donde apretaste
        if (pressed && pressed == KbHit((int)(short)LOWORD(lp), (int)(short)HIWORD(lp))) {
            if (pressed >= KB_TAB_0 && pressed <= KB_TAB_4) KbSetTab(hwnd, pressed - KB_TAB_0);
            else if (pressed >= KB_MODE_X && pressed <= KB_MODE_V) KbToggleMode(hwnd, pressed - KB_MODE_X);
            else if (pressed >= KB_STY_0 && pressed <= KB_STY_3) KbSetStyle(hwnd, pressed - KB_STY_0);
            else if (pressed >= KB_IND_0 && pressed <= KB_IND_4) KbSetIndicator(hwnd, pressed - KB_IND_0);
            else if (pressed == KB_SW_STATUS) KbToggleSwitch(hwnd, 0);
            else if (pressed == KB_SW_REMEMBER) KbToggleSwitch(hwnd, 1);
            else if (pressed >= KB_ACC_0 && pressed <= KB_ACC_4) KbSetAccent(hwnd, pressed - KB_ACC_0);
            else if (pressed == KB_TIMING_RESET) KbResetTiming(hwnd);
            else if (pressed == KB_RESET) KbReset(hwnd);
            else if (pressed == KB_STATS_RESET) { ResetStats(); KbRepaint(hwnd); }
            else { // Save / Cancel / la X: se cierra la ventana
                if (pressed == KB_SAVE) {
                    ApplySettings(g_kbPending, g_kbMask, g_kbOpts);
                    if (g_opts.statusTip) ShowTip(L"Mode key: " + KeyName(g_kbPending) + L"\n" + CycleText(), STATUS_TIP_MS);
                }
                KbClose(hwnd);
            }
            return 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        g_kbHwnd = nullptr;
        g_kbTimerOn = false; // los timers mueren con la ventana
        for (HFONT* f : { &g_kbFontSmall, &g_kbFontTiny, &g_kbFontChip, &g_kbFontKey, &g_kbFontBtn, &g_kbFontNum }) {
            if (*f) DeleteObject(*f);
            *f = nullptr;
        }
        if (g_kbIcon) DestroyIcon(g_kbIcon);
        g_kbIcon = nullptr;
        // se cerro la ventana de la primera vez sin elegir (Cancel / la X):
        // queda la tecla por defecto y no se vuelve a preguntar
        if (!g_keyConfigured) ApplySettings(g_modeVk.load(), g_modeMask.load(), g_opts);
        // la vista previa del indicador se va: queda lo guardado (si cancelaste, lo de antes)
        g_indForceUntil = 0;
        IndicatorSet(g_opts.indicator, g_mode.load(), g_opts.accent);
        // con "Remember position" la ventana se abre la proxima vez donde quedo
        if (g_opts.rememberPos) {
            RECT wr;
            if (GetWindowRect(hwnd, &wr) && (wr.left != g_winX || wr.top - g_kbSlide != g_winY)) {
                g_winX = wr.left;
                g_winY = wr.top - g_kbSlide; // sin el desplazamiento del fundido
                SaveSettings();
            }
        }
        return 0; // sin PostQuitMessage: esta no es la ventana principal
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenKeybindDialog(bool firstRun) {
    if (g_kbHwnd) { // ya abierta: solo traerla al frente
        SetForegroundWindow(g_kbHwnd);
        return;
    }
    HINSTANCE hi = GetModuleHandleW(nullptr);
    EnsureGdiplus();
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.style = CS_DROPSHADOW; // sombra suave alrededor, la ventana no tiene marco
        wc.lpfnWndProc = KeybindWndProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_MEOW));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = CreateSolidBrush(KB_BG_TOP); // evita el flash blanco antes del primer paint
        wc.lpszClassName = L"SoruAutoXKeybind";
        RegisterClassW(&wc);
        registered = true;
    }

    HDC screen = GetDC(nullptr);
    g_kbDpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);

    g_kbFirstRun = firstRun;
    g_kbTab = 0;
    g_kbTabPos = 0;
    g_kbPending = g_modeVk.load();
    g_kbMask = g_modeMask.load();
    g_kbOpts = g_opts;
    for (int i = 0; i < 4; i++) g_kbOn[i] = (g_kbMask >> i) & 1 ? 1.0f : 0.0f;
    g_kbSw[0] = g_kbOpts.statusTip ? 1.0f : 0.0f;
    g_kbSw[1] = g_kbOpts.rememberPos ? 1.0f : 0.0f;
    g_kbSeg = (float)g_kbOpts.tipStyle;
    g_kbInd = (float)g_kbOpts.indicator;
    for (int i = 0; i < 4; i++) g_kbSl[i] = KbSliderNorm(i, g_kbOpts.*KB_SLIDERS[i].field);
    g_kbDrag = KB_NONE;
    {
        unsigned top = 1;
        for (int i = 0; i < 4; i++) top = std::max(top, (unsigned)g_stats.sent[i]);
        for (int i = 0; i < 4; i++) g_kbBar[i] = (float)g_stats.sent[i] / (float)top;
    }
    g_kbAccPrev = g_kbOpts.accent;
    g_kbAccT = 1.0f;
    g_kbRing = KbRingTarget();
    g_kbMsg = KB_NOTE;
    g_kbMsgKind = KB_MSG_NOTE;
    g_kbHover = g_kbDown = KB_NONE;
    for (float& hot : g_kbHot) hot = 0;
    g_kbFlash = g_kbErr = g_kbFade = g_kbPress = 0;
    g_kbClosing = false;
    g_kbSlide = KbPx(10); // arranca un poco mas abajo y sube mientras aparece

    int w = KbPx(KB_CLIENT_W), h = KbPx(KB_CLIENT_H);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    if (g_opts.rememberPos && g_winX != INT_MIN && g_winY != INT_MIN) { // donde la dejaste, metida dentro del monitor que le toque
        RECT want = { g_winX, g_winY, g_winX + w, g_winY + h };
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST), &mi);
        x = std::clamp(g_winX, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - w));
        y = std::clamp(g_winY, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - h));
    }
    y += g_kbSlide;
    g_kbHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"SoruAutoXKeybind",
        L"meowww!", WS_POPUP, x, y, w, h, nullptr, nullptr, hi, nullptr);
    if (!g_kbHwnd) return;

    DWORD corner = 2; // DWMWCP_ROUND
    DWORD noBorder = 0xFFFFFFFE; // DWMWA_COLOR_NONE
    g_kbRound = SUCCEEDED(DwmSetWindowAttribute(g_kbHwnd, KB_DWMWA_CORNER, &corner, sizeof(corner)));
    DwmSetWindowAttribute(g_kbHwnd, KB_DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));

    SetLayeredWindowAttributes(g_kbHwnd, 0, 0, LWA_ALPHA); // arranca transparente, el timer lo hace aparecer
    ShowWindow(g_kbHwnd, SW_SHOW);
    SetForegroundWindow(g_kbHwnd);
    UpdateWindow(g_kbHwnd);
    KbKick(g_kbHwnd);
}

// ------------------------------------------------------------
// Deteccion (thread propio)
// ------------------------------------------------------------
static void DetectionLoop() {
    AttachState att = WaitForAttach(true);
    g_robloxPid.store(att.pid);

    std::deque<PosSample> history;
    CharCache cc;
    uint64_t lastCharacter = 0;
    DWORD lastTrigger = 0;
    DWORD lastTeleport = 0;  // ultimo salto de posicion propio, 0 = ninguno
    DWORD lastSoruSound = 0; // ultimo sonido "Soru" cerca tuyo, 0 = ninguno
    bool lastSoundV4 = false; // ese sonido fue el "DodgeQuick" de Human V4 (cooldown mas corto, ver TryFire)
    DWORD soundIgnoreUntil = 0; // despues de disparar se ignoran los sonidos un rato, 0 = no
    DWORD posFailSince = 0;  // 0 = no esta fallando ahora mismo
    std::vector<std::pair<uint64_t, DWORD>> pendingProxySounds; // {addr, firstSeenTick} - ProxySound Part, esperando su hijo Sound

    // announce: tooltip al reconectar. Si cerraste Roblox, si; si solo se
    // perdio la posicion un rato (morir, cambio de server) no, para que no
    // salte un cartel en medio del juego.
    auto Reattach = [&](const wchar_t* reason, bool announce) {
        SetStatus(std::wstring(reason) + L", reconnecting...");
        g_robloxPid.store(0);
        CloseHandle(att.hProcess);
        att = WaitForAttach(announce);
        g_robloxPid.store(att.pid);
        // todo esto apuntaba al proceso / DataModel anterior
        history.clear();
        cc = {};
        lastCharacter = 0;
        lastTeleport = 0;
        lastSoruSound = 0;
        lastSoundV4 = false;
        soundIgnoreUntil = 0;
        posFailSince = 0;
        pendingProxySounds.clear();
    };

    auto NearMe = [&](float ex, float ey, float ez, float x, float y, float z) {
        return MinDistToHistory(history, ex, ey, ez) <= MATCH_RADIUS || Dist3(ex, ey, ez, x, y, z) <= MATCH_RADIUS;
    };

    // Solo dispara si fue Soru: hace falta tu propio teleport Y un sonido
    // "Soru" cerca tuyo, con menos de SORU_MATCH_WINDOW_MS entre uno y otro.
    // Se llama cuando llega cada uno, asi que el orden no importa. Otro move
    // que te teleporta no trae ese sonido, y el Soru de otro jugador cerca no
    // te teleporta a vos. Con un macro de spam de R el gate de R esta siempre
    // abierto, asi que este par es el que filtra de verdad.
    auto TryFire = [&](DWORD now) {
        // los tiempos se cambian en la pestana Timing; se leen una vez por llamada
        const DWORD matchMs = (DWORD)g_matchMs.load(), rWindowMs = (DWORD)g_rWindowMs.load(), cooldownMs = (DWORD)g_cooldownMs.load();
        bool teleported = lastTeleport && (now - lastTeleport) <= matchMs;
        bool soruSound = lastSoruSound && (now - lastSoruSound) <= matchMs;
        DWORD lastRPress = g_lastRPress.load();
        // el hook pudo anotar una R despues de que tomamos "now": eso es edad
        // 0, no una resta negativa que da vuelta a ~49 dias y cierra el gate
        LONG rDiff = (LONG)(now - lastRPress);
        DWORD rAge = rDiff > 0 ? (DWORD)rDiff : 0;
        // Human V4: 3 cargas de Soru seguidas, ~300ms entre una y otra. Con el cooldown
        // por defecto (400) la segunda carga caia adentro y no mandaba la tecla.
        const DWORD effCooldownMs = lastSoundV4 ? std::min<DWORD>(cooldownMs, V4_COOLDOWN_MAX_MS) : cooldownMs;
        bool cooldownOk = (now - lastTrigger) > effCooldownMs;
        DBG("  [DBG TryFire] teleported=" << teleported << " soruSound=" << soruSound
            << " rAge=" << rAge << "ms (max " << rWindowMs << ") cooldownOk=" << cooldownOk << "\n");
        if (!(teleported && soruSound && lastRPress && rAge <= rWindowMs && cooldownOk))
            return;

        // cuanto tardo el sonido respecto del teleport (negativo = llego antes):
        // es la demora extra que agrega confirmar que fue Soru
        LONG soundGap = (LONG)(lastSoruSound - lastTeleport);
        lastTrigger = now;
        lastTeleport = lastSoruSound = 0; // este par ya se uso
        soundIgnoreUntil = now + SOUND_DEDUPE_MS; // el 2do "DodgeQuick" del mismo Soru no cuenta como otro
        char mode = g_mode.load();
        std::cout << "lo hizo -> " << ModeName(mode) << " (R hace " << rAge << "ms, sonido Soru "
                  << soundGap << "ms despues del teleport)\n";
        g_stats.detected++;
        if (!mode) {
            g_stats.ignoredOff++;
            if (g_mainHwnd) PostMessageW(g_mainHwnd, WM_APP_FIRED, 0, 0);
            return;
        }
        // SendInput va a la ventana que tenga el foco: si no es Roblox, no mandamos nada
        if (!RobloxIsForeground(att.pid)) {
            std::cout << "  Roblox no esta en foco, no se manda\n";
            g_stats.ignoredFocus++;
            if (g_mainHwnd) PostMessageW(g_mainHwnd, WM_APP_FIRED, 0, 0);
            return;
        }
        SendKeyPress(mode);
        for (int i = 0; i < 4; i++)
            if (MODE_LETTERS[i] == mode) g_stats.sent[i]++;
        g_stats.lastMode = mode;
        g_stats.lastGapMs = (int)soundGap;
        g_stats.lastTick = now ? now : 1; // 0 quiere decir "ninguno"
        if (g_mainHwnd) PostMessageW(g_mainHwnd, WM_APP_FIRED, 0, 0);
    };

    // Un Part con sonido "Soru" adentro: si esta cerca tuyo, cuenta como tu Soru
    auto OnSoruSound = [&](uint64_t part, SoruSound kind, float x, float y, float z, DWORD now) {
        if (soundIgnoreUntil && (LONG)(now - soundIgnoreUntil) < 0)
            return;
        float px, py, pz;
        if (!GetInstancePosition(att.hProcess, part, px, py, pz) || !NearMe(px, py, pz, x, y, z))
            return;
        DBG("[DBG] sonido Soru cerca tuyo (" << (kind == SOUND_V4 ? "V4 DodgeQuick" : "Soru") << ")\n");
        lastSoruSound = now;
        lastSoundV4 = (kind == SOUND_V4);
        TryFire(now);
    };

    while (true) {
        if (WaitForSingleObject(att.hProcess, 0) != WAIT_TIMEOUT)
            Reattach(L"Roblox closed", true);

        DWORD now = GetTickCount();

        float x = 0, y = 0, z = 0;
        if (GetMyPosition(att.hProcess, att.players, cc, x, y, z)) {
            posFailSince = 0;

            // character nuevo (respawn): el salto del lugar donde moriste al
            // spawn no es un teleport, el historial arranca de cero
            if (cc.character != lastCharacter) {
                history.clear();
                lastCharacter = cc.character;
            }

            // Salto de posicion = te teleportaste (HumanoidRootPart.CFrame=...).
            // Solo no alcanza: cualquier move que teleporta hace esto. Tiene que
            // aparecer tambien el sonido "Soru" cerca tuyo (ver TryFire).
            if (!history.empty()) {
                const PosSample& last = history.back();
                double jump = Dist3(x, y, z, last.x, last.y, last.z);
                if (jump >= TELEPORT_JUMP_MIN) {
                    DBG("[DBG] salto de posicion: " << jump << " studs\n");
                    lastTeleport = now;
                    TryFire(now);
                }
            }

            history.push_back({ x, y, z, now });
            while (!history.empty() && (now - history.front().t) > HISTORY_MS) history.pop_front();

            for (auto& root : att.roots) {
                for (uint64_t c : GetNewChildrenTail(att.hProcess, root)) {
                    std::string nm = ReadName(att.hProcess, c);
                    DBG("[DBG] nuevo hijo en '" << root.label << "': '" << nm << "' (0x" << std::hex << c << std::dec << ")\n");

                    // ProxySound: Part creado dinamicamente que recibe un hijo Sound
                    // nombrado segun la habilidad ("Soru", etc). Es lo unico que dice
                    // QUE move fue (TpPart y el salto de posicion los puede generar
                    // cualquier teleport). Su posicion (Part) sirve para el match por
                    // distancia. El hijo puede no estar listo todavia -> reintentar.
                    if (nm == "ProxySound")
                        pendingProxySounds.push_back({ c, now });
                    else if (nm == "Soru")
                        OnSoruSound(c, SOUND_SORU, x, y, z, now);
                }
            }

            // ProxySound pendientes: esperando a ver si su hijo es "Soru"
            // (el chequeo corre en este mismo poll, no uno despues)
            if (!pendingProxySounds.empty()) {
                std::vector<std::pair<uint64_t, DWORD>> stillPending;
                for (auto& pe : pendingProxySounds) {
                    if (SoruSound kind = FindSoruSound(att.hProcess, pe.first)) {
                        OnSoruSound(pe.first, kind, x, y, z, now);
                    } else if (now - pe.second <= PENDING_TIMEOUT_MS) {
                        stillPending.push_back(pe);
                    }
                }
                pendingProxySounds = std::move(stillPending);
            }
        } else {
            // character/HRP no resuelve: puede ser un frame suelto (respawn) o un
            // cambio de server real. Si se sostiene, re-attacheamos todo.
            if (posFailSince == 0) posFailSince = now;
            else if (now - posFailSince > STALE_REATTACH_MS)
                Reattach(L"Lost your position (server change?)", false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
    }
}

// ------------------------------------------------------------
// Ventana oculta del thread principal: recibe los mensajes del hook, del
// thread de deteccion, del icono de la bandeja y el timer del tooltip
// ------------------------------------------------------------
static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_CYCLE_MODE:
        CycleMode();
        return 0;
    case WM_APP_STATUS:
        UpdateTray(NIM_MODIFY);
        if (wp == 2 || (wp && g_opts.statusTip)) ShowTip(L"Soru Auto X\n" + GetStatus(), STATUS_TIP_MS);
        if (g_kbHwnd) PostMessageW(g_kbHwnd, WM_APP_REFRESH, 0, 0); // la ventana de teclas muestra el estado en vivo
        return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (L"Keybind: " + KeyName(g_modeVk.load())).c_str());
            HMENU modes = CreatePopupMenu(); // elegir el modo directo, sin pasar por el ciclo
            {
                char cur = g_mode.load();
                unsigned mask = g_modeMask.load();
                AppendMenuW(modes, MF_STRING | (cur == 0 ? MF_CHECKED : 0), IDM_MODE_BASE, L"OFF");
                for (int i = 0; i < 4; i++) {
                    if (!((mask >> i) & 1)) continue;
                    wchar_t label[2] = { (wchar_t)MODE_LETTERS[i], 0 };
                    AppendMenuW(modes, MF_STRING | (cur == MODE_LETTERS[i] ? MF_CHECKED : 0), IDM_MODE_BASE + 1 + i, label);
                }
            }
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)modes, L"Set mode");
            AppendMenuW(menu, MF_STRING, IDM_CHANGE_KEY, L"Settings...");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd); // si no, el menu no se cierra al clickear afuera
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            DestroyMenu(menu);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_EXIT) DestroyWindow(hwnd);
        else if (LOWORD(wp) == IDM_CHANGE_KEY) OpenKeybindDialog(false);
        else if (LOWORD(wp) >= IDM_MODE_BASE && LOWORD(wp) <= IDM_MODE_BASE + 4) {
            int i = LOWORD(wp) - IDM_MODE_BASE;
            AnnounceMode(i == 0 ? 0 : MODE_LETTERS[i - 1]);
        }
        return 0;
    case WM_APP_FIRED: // la deteccion cambio los Stats: la ventana abierta los muestra en vivo
        if (g_kbHwnd) PostMessageW(g_kbHwnd, WM_APP_REFRESH, 0, 0);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_HIDE_TIP) HideTip();
        return 0;
    case WM_DESTROY: // Exit del menu, o WM_CLOSE de una copia nueva que nos reemplaza
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (g_taskbarCreatedMsg && msg == g_taskbarCreatedMsg) {
        UpdateTray(NIM_ADD);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Como #SingleInstance Force de AHK: sin consola una copia vieja corriendo
// no se ve, y dos copias = dos hooks y la tecla mandada dos veces. Si ya hay
// una, le pedimos que se cierre y esperamos a que suelte el mutex.
static void ReplaceOldInstance() {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\SoruAutoX_SingleInstance");
    if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS) return; // somos la unica (el mutex queda abierto hasta salir)
    if (HWND old = FindWindowW(L"SoruAutoXMain", nullptr)) PostMessageW(old, WM_CLOSE, 0, 0);
    WaitForSingleObject(mutex, 3000); // WAIT_ABANDONED cuando la vieja termina: ahora es nuestro
}

// ------------------------------------------------------------
// main (sin consola: wWinMain = subsistema WINDOWS)
// ------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware(); // sin esto Windows estira las ventanas en pantallas con escala >100% y el texto sale borroso
    ReplaceOldInstance();

    if constexpr (VERBOSE) {
        AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
    }

    // Pide resolucion de timer de 1ms (por defecto Windows redondea sleeps a ~15ms)
    timeBeginPeriod(1);

    // Sin auto-elevar: leer memoria de Roblox no necesita admin, y correr
    // elevado puede hacer que Windows bloquee el SendInput hacia la ventana
    // de Roblox (no-elevada) por UIPI.
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"SoruAutoXMain";
    RegisterClassW(&wc);
    g_mainHwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"SoruAutoXMain", L"Soru Auto X", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    InitTooltip();
    AddTrayIcon();

    g_keyConfigured = LoadSettings(); // false = primera vez (quedan la tecla y los modos por defecto hasta que elijas)
    ResetStats();                      // arranca el reloj de la sesion
    IndicatorSet(g_opts.indicator, 0, g_opts.accent); // el indicador siempre visible, si lo elegiste
    // solo la tecla de Caps Lock se come, si el modo va en otra tecla Caps Lock sigue normal
    if (g_modeVk.load() == VK_CAPITAL) ForceCapsLockOff();
    g_soruScan = MapVirtualKeyW(SORU_VK, MAPVK_VK_TO_VSC);
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, hInst, 0);
    if (!hook) {
        DWORD err = GetLastError();
        SetStatus(L"Keyboard hook failed (error " + std::to_wstring(err) + L"): the mode key won't work", 2);
    } else if (g_keyConfigured && g_opts.statusTip) {
        ShowTip(L"Soru Auto X running\n" + KeyName(g_modeVk.load()) + L": " + CycleText(), STATUS_TIP_MS);
    }

    // La deteccion corre en su propio thread y este solo atiende el hook, el
    // tooltip y la bandeja: la red y los reintentos de attach no traban el
    // cambio de modo, y nada de este lado frena la deteccion.
    std::thread(DetectionLoop).detach();

    // primera vez: ventana para elegir la tecla (necesita el hook para capturarla)
    if (hook && !g_keyConfigured) OpenKeybindDialog(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hook) UnhookWindowsHookEx(hook);
    timeEndPeriod(1);
    // ExitProcess y no return: el thread de deteccion sigue vivo y no hay que
    // correr los destructores estaticos mientras los usa
    ExitProcess(0);
}
