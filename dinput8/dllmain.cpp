#include <windows.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <cstdarg>
#include <atomic>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "opengl32.lib")

// dinput8.dll proxy (KOTOR II 32-bit OpenGL build imports DirectInput8Create).
//
// Borderless / taskbar coverage behaviour is driven by dinput8.ini sitting
// next to the game executable. A worker thread waits for the engine's main
// top-level window to be visible AND stable (no style/size changes for ~2s),
// then performs a single style/position write. We deliberately never re-apply
// in a loop and never subclass the engine's wndproc: racing the engine's own

typedef HRESULT(WINAPI* LPDIRECTINPUT8CREATE)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

// ---------------------------------------------------------------------------
// Configuration (dinput8.ini).
// ---------------------------------------------------------------------------
enum class BorderlessMode {
    Windowed,   // pure pass-through, no window changes
    Fill,       // engine resolution + black borders (fullscreen backdrop)
    NoFill,     // same sizing/placement as Fill, no black backdrop
};

enum class WindowAlignment {
    Centered,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Top,
    Bottom,
    Left,
    Right,
};

static BorderlessMode   g_mode             = BorderlessMode::Fill;
static WindowAlignment  g_alignment          = WindowAlignment::Centered;
static bool             g_hideTaskbar        = true;
static bool             g_forceWindowed      = true;
static bool             g_enableConsole      = false;
static bool             g_enableLog          = false;
static bool             g_showSplashScreens  = true;
static bool             g_stretchViewport    = true;

// Optional resolution override written into swkotor2.ini before engine init.
// 0 means "leave the game's existing Width/Height alone" (default behaviour).
static int              g_forceWidth         = 0;
static int              g_forceHeight        = 0;

// Detected once at DllMain time from the primary monitor.
static LONG g_monitorX      = 0;
static LONG g_monitorY      = 0;
static LONG g_monitorWidth  = 1920;
static LONG g_monitorHeight = 1080;

static wchar_t g_exeDir[MAX_PATH]       = L"";
static wchar_t g_proxyIniPath[MAX_PATH] = L"";   // dinput8.ini  (ours)
static wchar_t g_gameIniPath[MAX_PATH]  = L"";   // swkotor2.ini (engine's)
static wchar_t g_logPath[MAX_PATH]      = L"";   // dinput8.log  (diagnostics + crashes)

// Last-resort install location if GetModuleFileNameW fails for some reason.
// This is the user's known KOTOR II install root.
static const wchar_t* kFallbackGameDir =
    L"A:\\SteamLibrary\\steamapps\\common\\Knights of the Old Republic II";

// Serialises FileLog writes from the engine, our worker thread, and the
// crash handler. Initialised at the top of DllMain(PROCESS_ATTACH).
static CRITICAL_SECTION   g_logLock;
static std::atomic<bool>  g_logLockReady{ false };
static std::atomic<bool>  g_crashHandled{ false };
static std::atomic<bool>  g_inForegroundCallback{ false };

static char g_lastBreadcrumb[128] = "DllMain: attach";
static DWORD g_lastBreadcrumbTid = 0;

static HWND g_backdropHwnd = NULL;
static HWND g_gameHwndForStack = NULL;

// Saved after the one-shot borderless apply; used to undo engine FMV window shrinks.
static int              g_targetClientW     = 0;
static int              g_targetClientH     = 0;
static WindowAlignment  g_targetPlacement   = WindowAlignment::Centered;
static LONG             g_targetPopupStyle  = 0;
static LONG             g_targetPopupExStyle = 0;
static std::atomic<bool> g_targetLayoutSaved{ false };
static std::atomic<bool> g_inLayoutRestore{ false };
static std::atomic<int>  g_engineShrunkW{ 0 };
static std::atomic<int>  g_engineShrunkH{ 0 };
static std::atomic<DWORD> g_lastFmvShrinkMs{ 0 };

// Defined later (logging section); used by the z-order helpers below.
static void WorkerLog(const char* branch, const char* format, ...);
static bool IsEngineFmvActive();
static void EnsureFillBackdropStacked();
static void HideFillBackdrop();
static void UpdateFillBackdropForSession();

static void SetBreadcrumb(const char* crumb) {
    if (!crumb || !crumb[0]) return;
    const bool lock = g_logLockReady.load();
    if (lock) EnterCriticalSection(&g_logLock);
    strncpy_s(g_lastBreadcrumb, crumb, _TRUNCATE);
    g_lastBreadcrumbTid = GetCurrentThreadId();
    if (lock) LeaveCriticalSection(&g_logLock);
}

static bool IsForeignAppForeground() {
    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == g_gameHwndForStack) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return fgPid != GetCurrentProcessId();
}

// Not alt-tabbed to another app (Chrome, etc.) — same "session" as the game process.
static bool IsGameSessionActive() {
    if (!g_gameHwndForStack || !IsWindow(g_gameHwndForStack)) return false;
    if (IsIconic(g_gameHwndForStack)) return false;
    return !IsForeignAppForeground();
}

// Fill letterbox: same visibility as the game whenever the session is active.
static bool ShouldShowFillBackdrop() {
    if (g_mode != BorderlessMode::Fill) return false;
    if (!g_backdropHwnd || !IsWindow(g_backdropHwnd)) return false;
    return IsGameSessionActive();
}

static void HideFillBackdrop() {
    if (!g_backdropHwnd || !IsWindow(g_backdropHwnd)) return;
    ShowWindow(g_backdropHwnd, SW_HIDE);
}

// Fill mode: monitor-sized black layer stacked directly under the game.
static void EnsureFillBackdropStacked() {
    if (g_mode != BorderlessMode::Fill) return;
    if (!g_backdropHwnd || !IsWindow(g_backdropHwnd)) return;
    if (!g_gameHwndForStack || !IsWindow(g_gameHwndForStack)) return;
    if (IsIconic(g_gameHwndForStack)) return;
    if (!ShouldShowFillBackdrop()) {
        HideFillBackdrop();
        return;
    }

    SetWindowPos(g_backdropHwnd, NULL,
                 g_monitorX, g_monitorY, g_monitorWidth, g_monitorHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // hWndInsertAfter = game HWND → backdrop sits behind the game.
    SetWindowPos(g_backdropHwnd, g_gameHwndForStack, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    RedrawWindow(g_backdropHwnd, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

static void UpdateFillBackdropForSession() {
    if (ShouldShowFillBackdrop())
        EnsureFillBackdropStacked();
    else
        HideFillBackdrop();
}

// Alt-tab / foreign app: hide Fill backdrop and sink the game below normal windows.
static void ApplyUnfocusedGamePlacement() {
    HideFillBackdrop();
    if (!g_gameHwndForStack || !IsWindow(g_gameHwndForStack)) return;
    if (IsIconic(g_gameHwndForStack)) return;
    SetWindowPos(g_gameHwndForStack, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Re-seat helper backdrop (NoFill taskbar shield) behind the game.
static void RestackFillWindows() {
    if (!g_gameHwndForStack || !g_backdropHwnd) return;
    if (!IsWindow(g_gameHwndForStack) || !IsWindow(g_backdropHwnd)) return;
    if (IsIconic(g_gameHwndForStack)) return;
    if (g_mode == BorderlessMode::Fill) {
        UpdateFillBackdropForSession();
        return;
    }
    if (GetForegroundWindow() != g_gameHwndForStack) return;

    SetWindowPos(g_backdropHwnd, NULL,
                 g_monitorX, g_monitorY, g_monitorWidth, g_monitorHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    SetWindowPos(g_backdropHwnd, g_gameHwndForStack, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Fill/NoFill: track foreground so alt-tab can drop the game behind other apps.
// HideTaskbar=1 additionally raises above the taskbar while focused.
static bool UsesFocusZOrder() {
    return g_hideTaskbar || g_mode == BorderlessMode::NoFill;
}

static void ApplyAlignmentToWindow(HWND hwnd, WindowAlignment align);
static WindowAlignment EffectivePlacementForHwnd(HWND hwnd);
static void ComputeAlignedPlacement(
    int desiredClientW, int desiredClientH,
    LONG style, LONG exStyle,
    WindowAlignment align,
    int* outX, int* outY, int* outOuterW, int* outOuterH);
static void RestoreGameClientLayoutIfNeeded();

// True for a short window after the engine shrinks the HWND for Bink FMV.
// We only use a timestamp (not "client smaller than target") so a resolution
// mismatch cannot block alt-tab forever.
static bool IsEngineFmvActive() {
    const DWORD lastMs = g_lastFmvShrinkMs.load();
    if (lastMs == 0) return false;
    return (GetTickCount() - lastMs) < 20000;
}

static void SetGameFocusZOrder(bool gameFocused) {
    if (!g_gameHwndForStack || !IsWindow(g_gameHwndForStack)) return;

    if (!IsGameSessionActive()) {
        ApplyUnfocusedGamePlacement();
        return;
    }

    // Backdrop tracks the game whenever we're not alt-tabbed out (incl. Bink FMV).
    if (g_mode == BorderlessMode::Fill) {
        EnsureFillBackdropStacked();
    }

    const bool fgIsGame = GetForegroundWindow() == g_gameHwndForStack;
    const bool raiseGame =
        gameFocused && (fgIsGame || IsEngineFmvActive());

    if (!raiseGame) {
        // In-process FMV / WM foreground glitch: keep backdrop, don't HWND_BOTTOM.
        if (IsEngineFmvActive()) {
            RestoreGameClientLayoutIfNeeded();
        }
        if (UsesFocusZOrder()) {
            SetWindowPos(g_gameHwndForStack, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        if (g_mode == BorderlessMode::Fill) {
            EnsureFillBackdropStacked();
        }
        return;
    }

    const bool fmvActive = IsEngineFmvActive();
    const HWND insertAfter = g_hideTaskbar ? HWND_TOPMOST : HWND_NOTOPMOST;

    if (IsIconic(g_gameHwndForStack)) {
        ShowWindow(g_gameHwndForStack, SW_RESTORE);
    }
    if (fmvActive) {
        RestoreGameClientLayoutIfNeeded();
    }

    if (UsesFocusZOrder()) {
        ApplyAlignmentToWindow(g_gameHwndForStack,
                               EffectivePlacementForHwnd(g_gameHwndForStack));
        SetWindowPos(g_gameHwndForStack, insertAfter, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }

    if (g_mode == BorderlessMode::Fill) {
        EnsureFillBackdropStacked();
    }
}

// Another app was minimized; the shell may briefly assign foreground to our still-
// visible game HWND before we finish minimizing on alt-tab.
static std::atomic<DWORD> g_lastForeignMinimizeMs{ 0 };

static VOID CALLBACK MinimizeStackCallback(
    HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/)
{
    if (event != EVENT_SYSTEM_MINIMIZESTART) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (!hwnd || hwnd == g_gameHwndForStack) return;
    g_lastForeignMinimizeMs.store(GetTickCount());
}

static bool IsSpuriousForegroundAfterForeignMinimize() {
    const DWORD lastMs = g_lastForeignMinimizeMs.load();
    if (lastMs == 0) return false;
    const DWORD elapsed = GetTickCount() - lastMs;
    return elapsed < 800;
}

static VOID CALLBACK ForegroundStackCallback(
    HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/)
{
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    if (g_inForegroundCallback.exchange(true)) return;

    SetBreadcrumb("ForegroundStackCallback: entered");

    // Get the actual real-time foreground window to be certain
    HWND currentForeground = GetForegroundWindow();

    if (currentForeground == g_gameHwndForStack) {
        if (IsSpuriousForegroundAfterForeignMinimize() && !IsEngineFmvActive()) {
            WorkerLog("focus", "ignore spurious foreground after foreign minimize");
            ApplyUnfocusedGamePlacement();
            ShowWindow(g_gameHwndForStack, SW_MINIMIZE);
        } else {
            SetGameFocusZOrder(true);
        }
    } else if (IsForeignAppForeground()) {
        SetGameFocusZOrder(false);
    } else {
        // Bink / same process, non-game foreground HWND — refresh backdrop, no sink.
        SetGameFocusZOrder(false);
    }

    SetBreadcrumb("ForegroundStackCallback: done");
    g_inForegroundCallback.store(false);
}

static void RestackFillAfterGameLayoutChange() {
    if (!g_backdropHwnd || !g_gameHwndForStack) return;
    if (!IsWindow(g_gameHwndForStack) || !IsWindow(g_backdropHwnd)) return;
    if (IsIconic(g_gameHwndForStack)) return;

    if (g_mode == BorderlessMode::Fill) {
        UpdateFillBackdropForSession();
        return;
    }

    if (GetForegroundWindow() != g_gameHwndForStack) return;
    RestackFillWindows();
}

static void RefreshFocusZOrderIfGameFocused() {
    if (!UsesFocusZOrder()) return;
    if (!g_gameHwndForStack || !IsWindow(g_gameHwndForStack)) return;
    if (IsIconic(g_gameHwndForStack)) return;
    if (GetForegroundWindow() != g_gameHwndForStack) return;
    SetGameFocusZOrder(true);
}

static DWORD WINAPI DelayedRestackThread(LPVOID /*lpParam*/) {
    static const DWORD kDelaysMs[] = { 400, 1200, 3000 };
    for (DWORD delay : kDelaysMs) {
        Sleep(delay);

        if (!g_gameHwndForStack || !IsWindow(g_gameHwndForStack)) break;

        if (IsIconic(g_gameHwndForStack)) continue;

        if (g_backdropHwnd) {
            if (!IsWindow(g_backdropHwnd)) break;

            if (!IsGameSessionActive()) continue;

            if (GetForegroundWindow() == g_gameHwndForStack) {
                WorkerLog("DelayedRestack", "Enforcing HWND_TOPMOST safety check.");
                SetGameFocusZOrder(true);
            } else if (g_mode == BorderlessMode::Fill) {
                SetGameFocusZOrder(false);
            }
        } else {
            RefreshFocusZOrderIfGameFocused();
        }
    }
    return 0;
}

// Engine SetWindowPos can reorder the game behind our backdrop/shield or below
// the taskbar. Re-seat owned helper windows and re-apply focus z-order.
static VOID CALLBACK GameWindowLayoutCallback(
    HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/)
{
    if (event != EVENT_OBJECT_LOCATIONCHANGE) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (hwnd != g_gameHwndForStack) return;

    if (IsIconic(g_gameHwndForStack) && !IsEngineFmvActive()) return;
    if (!IsGameSessionActive()) return;

    RestoreGameClientLayoutIfNeeded();

    if (g_backdropHwnd) {
        RestackFillAfterGameLayoutChange();
    } else {
        RefreshFocusZOrderIfGameFocused();
    }
}

struct TargetWindowData {
    DWORD processId;
    HWND  hwnd;
    int   bestArea;
};

// ---------------------------------------------------------------------------
// Logging.
//
// When EnableLog=1, FileLog appends timestamped lines to dinput8.log in the
// game folder (independent of EnableConsole). DebugLog / WorkerLog also echo
// to the console when EnableConsole=1.
// ---------------------------------------------------------------------------
static void FileLogRaw(const char* text) {
    const bool lock = g_logLockReady.load();
    if (lock) EnterCriticalSection(&g_logLock);

    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
        CloseHandle(h);
    }

    if (lock) LeaveCriticalSection(&g_logLock);
}

static void LogInternal(const char* tag, const char* consolePrefix, const char* format, va_list args) {
    char msg[2048];
    int msgLen = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, format, args);
    if (msgLen < 0) return;

    if (g_enableLog && g_logPath[0] != L'\0') {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char line[4096];
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "[%04d-%02d-%02d %02d:%02d:%02d.%03d][pid:%lu][tid:%lu]%s %s\r\n",
                            st.wYear, st.wMonth, st.wDay,
                            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                            GetCurrentProcessId(), GetCurrentThreadId(),
                            tag ? tag : "", msg);
        if (n > 0) {
            FileLogRaw(line);
        }
    }

    if (g_enableConsole && consolePrefix) {
        char line[4096];
        int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                            "[KOTOR2-BORDERLESS] %s%s\r\n",
                            consolePrefix, msg);
        if (n > 0) {
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
                DWORD written = 0;
                WriteConsoleA(hOut, line, (DWORD)n, &written, NULL);
            } else {
                OutputDebugStringA(line);
            }
        }
    }
}

static void FileLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    LogInternal("", nullptr, format, args);
    va_end(args);
}

static void DebugLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    LogInternal("", "", format, args);
    va_end(args);
}

static void WorkerLog(const char* branch, const char* format, ...) {
    char tag[80];
    _snprintf_s(tag, sizeof(tag), _TRUNCATE, "[t:%lu][%s]", GetTickCount(),
                branch ? branch : "");

    char consolePrefix[128];
    _snprintf_s(consolePrefix, sizeof(consolePrefix), _TRUNCATE, "t=%lu [%s] ",
                GetTickCount(), branch ? branch : "");

    va_list args;
    va_start(args, format);
    LogInternal(tag, consolePrefix, format, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Crash log system.
//
// Unhandled + vectored SEH handlers and CRT abort hooks write a human-readable
// crash report (exception, faulting module + offset, registers, stack walk,
// last breadcrumb) into dinput8.log and optionally a .dmp minidump.
// ---------------------------------------------------------------------------
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevExceptionFilter = nullptr;
static PVOID                        g_vectoredHandler      = nullptr;
static _invalid_parameter_handler   g_prevInvalidParam     = nullptr;

static const char* ExceptionCodeToString(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
    case 0x40010006:                         return "DBG_PRINTEXCEPTION_C";
    case 0x406D1388:                         return "MSVC thread name";
    case 0xE06D7363:                         return "C++ exception (MSVC)";
    default:                                 return "UNKNOWN";
    }
}

static bool IsBenignFirstChanceException(DWORD code, DWORD flags) {
    if (flags & EXCEPTION_NONCONTINUABLE) return false;
    switch (code) {
    case 0x40010006:
    case 0x406D1388:
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_SINGLE_STEP:
        return true;
    default:
        return false;
    }
}

// Resolve the module that owns an address to "name+0xoffset".
static void DescribeAddressModule(DWORD_PTR addr, char* out, size_t outSize) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == 0 || !mbi.AllocationBase) {
        _snprintf_s(out, outSize, _TRUNCATE, "<unknown>");
        return;
    }
    HMODULE mod = (HMODULE)mbi.AllocationBase;
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(mod, path, MAX_PATH) == 0) {
        _snprintf_s(out, outSize, _TRUNCATE, "0x%p", (void*)mod);
        return;
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    DWORD_PTR off = addr - (DWORD_PTR)mod;
    _snprintf_s(out, outSize, _TRUNCATE, "%ls+0x%IX", name, off);
}

// Classify the faulting module so the report makes clear, at a glance, whether
// the crash originated in our proxy, the engine, or an injected third party
// (overlays / GPU drivers). KOTOR II is legacy OpenGL; Discord and Steam
// overlays hook GL entry points and routinely crash inside the GPU driver.
static const char* DescribeFaultOrigin(const char* faultMod) {
    if (!faultMod || !faultMod[0]) return nullptr;

    // Matched as a case-insensitive prefix against the faulting module name
    // ("name.dll+0xoffset"), so entries must be specific enough not to collide.
    struct Known { const char* prefix; const char* note; };
    static const Known kKnown[] = {
        { "discordhook",         "Discord in-game overlay (DiscordHook.dll) - disable the Discord overlay (incl. Legacy Overlay) for swkotor2.exe." },
        { "gameoverlayrenderer", "Steam in-game overlay (GameOverlayRenderer.dll) - disable the Steam overlay for this title." },
        { "nvoglv",              "NVIDIA OpenGL driver (nvoglv*.dll) - on this legacy GL game this is almost always an injected overlay hooking GL, not the proxy." },
        { "atioglxx",            "AMD OpenGL driver - usually an injected overlay hooking GL, not the proxy." },
        { "dinput8",             "the dinput8 proxy itself." },
        { "swkotor2",            "the game engine (swkotor2.exe)." },
    };
    for (const Known& k : kKnown) {
        if (_strnicmp(faultMod, k.prefix, strlen(k.prefix)) == 0) {
            return k.note;
        }
    }
    return nullptr;
}

static void WriteMiniDump(EXCEPTION_POINTERS* ep, const wchar_t* dumpPath) {
    HANDLE hFile = CreateFileW(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers    = FALSE;

    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(
        MiniDumpWithIndirectlyReferencedMemory |
        MiniDumpScanMemory |
        MiniDumpWithThreadInfo);

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                      type, ep ? &mei : NULL, NULL, NULL);
    CloseHandle(hFile);
}

static void WriteStackWalk(EXCEPTION_POINTERS* ep) {
    HANDLE hProcess = GetCurrentProcess();
    HANDLE hThread  = GetCurrentThread();

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitializeW(hProcess, NULL, TRUE);

    CONTEXT ctx = *ep->ContextRecord;

    STACKFRAME64 frame{};
    DWORD machine = IMAGE_FILE_MACHINE_I386;
#ifdef _M_IX86
    frame.AddrPC.Offset    = ctx.Eip;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
#elif defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#endif
    frame.AddrPC.Mode    = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    FileLog("---- Call stack (most recent first) ----");

    for (int i = 0; i < 64; i++) {
        if (!StackWalk64(machine, hProcess, hThread, &frame, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }
        DWORD_PTR pc = (DWORD_PTR)frame.AddrPC.Offset;
        if (pc == 0) break;

        char modDesc[MAX_PATH + 32];
        DescribeAddressModule(pc, modDesc, sizeof(modDesc));

        // Try to resolve a symbol name + line.
        char symBuf[sizeof(SYMBOL_INFO) + 256]{};
        SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen   = 255;
        DWORD64 disp = 0;

        if (SymFromAddr(hProcess, pc, &disp, sym)) {
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(hProcess, pc, &lineDisp, &line)) {
                FileLog("  #%02d 0x%p %s  %s+0x%llX  (%s:%lu)",
                        i, (void*)pc, modDesc, sym->Name,
                        (unsigned long long)disp, line.FileName, line.LineNumber);
            } else {
                FileLog("  #%02d 0x%p %s  %s+0x%llX",
                        i, (void*)pc, modDesc, sym->Name,
                        (unsigned long long)disp);
            }
        } else {
            FileLog("  #%02d 0x%p %s", i, (void*)pc, modDesc);
        }
    }

    SymCleanup(hProcess);
}

static void WriteRegisters(const CONTEXT* c) {
#ifdef _M_IX86
    FileLog("---- Registers (x86) ----");
    FileLog("  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX",
            c->Eax, c->Ebx, c->Ecx, c->Edx);
    FileLog("  ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
            c->Esi, c->Edi, c->Ebp, c->Esp);
    FileLog("  EIP=%08lX EFL=%08lX CS=%04lX DS=%04lX SS=%04lX",
            c->Eip, c->EFlags, c->SegCs, c->SegDs, c->SegSs);
#elif defined(_M_X64)
    FileLog("---- Registers (x64) ----");
    FileLog("  RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX",
            c->Rax, c->Rbx, c->Rcx, c->Rdx);
    FileLog("  RSI=%016llX RDI=%016llX RBP=%016llX RSP=%016llX",
            c->Rsi, c->Rdi, c->Rbp, c->Rsp);
    FileLog("  RIP=%016llX EFL=%08lX", c->Rip, c->EFlags);
#endif
}

static void WriteLoadedModules() {
    FileLog("---- Loaded modules ----");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            FileLog("  0x%p  size=0x%lX  %ls",
                    (void*)me.modBaseAddr, me.modBaseSize, me.szModule);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

static void WriteCrashReport(const char* via, EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    const DWORD code = er ? er->ExceptionCode : 0;
    DWORD_PTR faultAddr = er ? (DWORD_PTR)er->ExceptionAddress : 0;

    char faultMod[MAX_PATH + 32];
    DescribeAddressModule(faultAddr, faultMod, sizeof(faultMod));

    FileLog("===================================================================");
    FileLog("============================ CRASH =================================");
    FileLog("===================================================================");
    FileLog("Captured via: %s", via ? via : "unknown");

    const DWORD crashTid = GetCurrentThreadId();
    if (g_lastBreadcrumbTid != 0 && g_lastBreadcrumbTid != crashTid) {
        // The breadcrumb was set by a different thread than the one that
        // faulted, so it does NOT describe where this crash happened.
        FileLog("Last breadcrumb: %s  (set by tid:%lu, NOT the faulting "
                "tid:%lu - breadcrumb is unrelated to this crash)",
                g_lastBreadcrumb, g_lastBreadcrumbTid, crashTid);
    } else {
        FileLog("Last breadcrumb: %s", g_lastBreadcrumb);
    }

    FileLog("Exception: 0x%08lX (%s)", code, ExceptionCodeToString(code));
    FileLog("Fault address: 0x%p  (%s)", (void*)faultAddr, faultMod);

    const char* origin = DescribeFaultOrigin(faultMod);
    if (origin) {
        FileLog("Fault origin: %s", origin);
    }

    if (er && code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char* op = er->ExceptionInformation[0] == 0 ? "read"
                       : er->ExceptionInformation[0] == 1 ? "write"
                       : er->ExceptionInformation[0] == 8 ? "execute" : "?";
        FileLog("Access violation: tried to %s address 0x%p",
                op, (void*)er->ExceptionInformation[1]);
    }

    if (ep && ep->ContextRecord) {
        WriteRegisters(ep->ContextRecord);
        WriteStackWalk(ep);
    }
    WriteLoadedModules();

    if (g_exeDir[0] != L'\0') {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t dumpPath[MAX_PATH];
        _snwprintf_s(dumpPath, _TRUNCATE,
                     L"%s\\dinput8_crash_%04d%02d%02d_%02d%02d%02d.dmp",
                     g_exeDir, st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond);
        WriteMiniDump(ep, dumpPath);
        FileLog("Minidump written: %ls", dumpPath);
    }

    FileLog("=========================== END CRASH =============================");
}

static void WriteCrtCrashReport(const char* via, const char* detail) {
    if (g_crashHandled.exchange(true)) return;

    FileLog("===================================================================");
    FileLog("============================ CRASH =================================");
    FileLog("===================================================================");
    FileLog("Captured via: %s", via ? via : "CRT");
    FileLog("Last breadcrumb: %s", g_lastBreadcrumb);
    if (detail && detail[0]) FileLog("Detail: %s", detail);
    WriteLoadedModules();
    FileLog("=========================== END CRASH =============================");
}

static LONG WINAPI VectoredCrashHandler(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code  = ep->ExceptionRecord->ExceptionCode;
    const DWORD flags = ep->ExceptionRecord->ExceptionFlags;
    if (IsBenignFirstChanceException(code, flags)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (!g_crashHandled.exchange(true)) {
        WriteCrashReport("VectoredExceptionHandler", ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI UnhandledCrashFilter(EXCEPTION_POINTERS* ep) {
    if (g_crashHandled.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    WriteCrashReport("UnhandledExceptionFilter", ep);

    if (g_prevExceptionFilter) {
        return g_prevExceptionFilter(ep);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static void __cdecl InvalidParameterHandler(
    const wchar_t* expression,
    const wchar_t* function,
    const wchar_t* file,
    unsigned int line,
    uintptr_t /*pReserved*/)
{
    char detail[512];
    _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                "invalid_parameter expr=%ls func=%ls file=%ls line=%u",
                expression ? expression : L"?",
                function ? function : L"?",
                file ? file : L"?", line);
    WriteCrtCrashReport("_invalid_parameter_handler", detail);

    if (g_prevInvalidParam) {
        g_prevInvalidParam(expression, function, file, line, 0);
    }
}

static void AbortSignalHandler(int /*sig*/) {
    WriteCrtCrashReport("SIGABRT", "abort() / SIGABRT");
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
}

static void InstallCrashHandlers() {
    g_prevExceptionFilter = SetUnhandledExceptionFilter(UnhandledCrashFilter);
    g_vectoredHandler     = AddVectoredExceptionHandler(1, VectoredCrashHandler);
    g_prevInvalidParam    = _set_invalid_parameter_handler(InvalidParameterHandler);
    signal(SIGABRT, AbortSignalHandler);
    SetBreadcrumb("DllMain: crash handlers installed");
    FileLog("Crash handlers installed (unhandled + vectored + CRT).");
}

static void UninstallCrashHandlers() {
    if (g_vectoredHandler) {
        RemoveVectoredExceptionHandler(g_vectoredHandler);
        g_vectoredHandler = nullptr;
    }
    SetUnhandledExceptionFilter(g_prevExceptionFilter);
    g_prevExceptionFilter = nullptr;
    _set_invalid_parameter_handler(g_prevInvalidParam);
    g_prevInvalidParam = nullptr;
    signal(SIGABRT, SIG_DFL);
}

static bool ParseIniBool(const wchar_t* value) {
    if (!value || !value[0]) return false;
    if (_wcsicmp(value, L"1") == 0 || _wcsicmp(value, L"true") == 0 ||
        _wcsicmp(value, L"yes") == 0 || _wcsicmp(value, L"on") == 0) {
        return true;
    }
    return false;
}

// AllocConsole alone often leaves the window hidden behind a topmost game.
static void InitDebugConsole() {
    if (!AllocConsole()) {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
    SetConsoleTitleA("KOTOR II Borderless Proxy");

    HWND con = GetConsoleWindow();
    if (con) {
        ShowWindow(con, SW_SHOW);
        SetWindowPos(con, HWND_TOPMOST, 50, 50, 900, 500,
                     SWP_SHOWWINDOW);
    }
}

static HMODULE LoadSystemDll(const wchar_t* dllName) {
    wchar_t sysDir[MAX_PATH]{};
    if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0) return nullptr;

    wchar_t fullPath[MAX_PATH]{};
    if (swprintf_s(fullPath, L"%s\\%s", sysDir, dllName) < 0) return nullptr;

    return LoadLibraryW(fullPath);
}

// ---------------------------------------------------------------------------
// Paths / monitor detection.
// ---------------------------------------------------------------------------
static void DetectExePaths() {
    wchar_t exePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(NULL, exePath, MAX_PATH);

    bool haveDir = false;
    if (n != 0 && n < MAX_PATH) {
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
            wcscpy_s(g_exeDir, exePath);
            haveDir = true;
        }
    }
    if (!haveDir) {
        // Could not resolve the host exe directory; fall back to the known
        // install root so the crash log still lands in the game folder.
        wcscpy_s(g_exeDir, kFallbackGameDir);
    }

    swprintf_s(g_proxyIniPath, L"%s\\dinput8.ini",   g_exeDir);
    swprintf_s(g_gameIniPath,  L"%s\\swkotor2.ini",  g_exeDir);
    swprintf_s(g_logPath,      L"%s\\dinput8.log",   g_exeDir);
}

static void DetectTargetMonitorRect() {
    POINT origin{ 0, 0 };
    HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        g_monitorX      = mi.rcMonitor.left;
        g_monitorY      = mi.rcMonitor.top;
        g_monitorWidth  = mi.rcMonitor.right  - mi.rcMonitor.left;
        g_monitorHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
    } else {
        g_monitorX      = 0;
        g_monitorY      = 0;
        g_monitorWidth  = GetSystemMetrics(SM_CXSCREEN);
        g_monitorHeight = GetSystemMetrics(SM_CYSCREEN);
    }
    if (g_monitorWidth <= 0 || g_monitorHeight <= 0) {
        g_monitorWidth  = 1920;
        g_monitorHeight = 1080;
    }
}

// ---------------------------------------------------------------------------
// dinput8.ini handling.
// ---------------------------------------------------------------------------
static void WriteDefaultProxyIni() {
    // ANSI INI body so GetPrivateProfileInt/String can parse it directly and
    // the user can edit it with notepad without UTF surprises.
    const char* body =
        "[Borderless]\r\n"
        "; Mode controls sizing and whether black borders are drawn.\r\n"
        ";   Fill    - Game stays at the engine's chosen resolution. A fullscreen\r\n"
        ";             black backdrop fills the monitor around the window\r\n"
        ";             (letterboxing / pillarboxing). Use Alignment to position\r\n"
        ";             the game window on that backdrop.\r\n"
        ";   NoFill  - Same engine resolution and Alignment as Fill, but no black\r\n"
        ";             backdrop (desktop shows in the letterbox area).\r\n"
        ";   Windowed - Pure pass-through; no window or INI changes.\r\n"
        "Mode=Fill\r\n"
        "\r\n"
        "; Alignment (Fill / NoFill): where the game window sits on the monitor.\r\n"
        ";   Centered, TopLeft, TopRight, BottomLeft, BottomRight,\r\n"
        ";   Top, Bottom, Left, Right\r\n"
        "Alignment=Centered\r\n"
        "\r\n"
        "; HideTaskbar:\r\n"
        ";   1 - While focused, keep the game (and Fill backdrop) above the taskbar.\r\n"
        ";       Alt-tab away releases topmost so other apps can appear on top.\r\n"
        ";   0 - Leave the taskbar untouched (NoFill still drops behind on alt-tab).\r\n"
        "HideTaskbar=1\r\n"
        "\r\n"
        "; StretchViewport:\r\n"
        ";   1 - Hook glViewport/glScissor to shift up and extend by top chrome band.\r\n"
        ";   0 - HWND/DWM fixes only.\r\n"
        "StretchViewport=1\r\n"
        "\r\n"
        "; ForceWindowed:\r\n"
        ";   1 - Rewrite swkotor2.ini: FullScreen=0, AllowWindowedMode=1.\r\n"
        ";   0 - Leave swkotor2.ini display mode keys alone.\r\n"
        "ForceWindowed=1\r\n"
        "\r\n"
        "; Width / Height: force the engine's render resolution by writing these\r\n"
        "; into swkotor2.ini [Graphics Options] before the game starts. The proxy\r\n"
        "; keeps whatever the engine renders at, so set the resolution HERE rather\r\n"
        "; than expecting a borderless window to upscale.\r\n"
        ";   0    - Leave the game's existing Width/Height alone (default).\r\n"
        ";   >0   - Force this resolution (e.g. Width=1920 Height=1080).\r\n"
        "; NOTE: In windowed mode (ForceWindowed=1) the engine may create a smaller\r\n"
        "; client than Width/Height because of the title bar; the borderless pass\r\n"
        "; reclaims that space and sizes to the configured render resolution.\r\n"
        "; Pick a resolution that fits your monitor, e.g. one notch below native.\r\n"
        "Width=0\r\n"
        "Height=0\r\n"
        "\r\n"
        "; EnableLog:\r\n"
        ";   1 - Append diagnostics and crash reports to dinput8.log beside the exe.\r\n"
        ";   0 - No log file (default).\r\n"
        "EnableLog=0\r\n"
        "\r\n"
        "; EnableConsole:\r\n"
        ";   1 - Open a debug console (WriteConsole only; does not redirect stdio).\r\n"
        ";   0 - Run silent (recommended for normal play).\r\n"
        "EnableConsole=0\r\n"
        "\r\n"
        "; SplashScreens:\r\n"
        ";   1 - Show BioWare/Obsidian logo splash screens on startup.\r\n"
        ";   0 - Skip splash screens (Steam swkotor2.exe build only).\r\n"
        "SplashScreens=1\r\n"
        "\r\n"
        "; Overlays: KOTOR II uses legacy OpenGL. Discord (including Legacy Overlay)\r\n"
        "; and Steam in-game overlay hook GL and can crash after borderless changes.\r\n"
        "; Disable both for swkotor2.exe. See README.txt; set EnableLog=1 for dinput8.log.\r\n";

    HANDLE h = CreateFileW(g_proxyIniPath, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, body, (DWORD)strlen(body), &written, NULL);
    CloseHandle(h);
}

static WindowAlignment ParseAlignment(const wchar_t* value) {
    if (!value || !value[0]) return WindowAlignment::Centered;
    if (_wcsicmp(value, L"TopLeft") == 0)     return WindowAlignment::TopLeft;
    if (_wcsicmp(value, L"TopRight") == 0)    return WindowAlignment::TopRight;
    if (_wcsicmp(value, L"BottomLeft") == 0)  return WindowAlignment::BottomLeft;
    if (_wcsicmp(value, L"BottomRight") == 0) return WindowAlignment::BottomRight;
    if (_wcsicmp(value, L"Top") == 0)         return WindowAlignment::Top;
    if (_wcsicmp(value, L"Bottom") == 0)      return WindowAlignment::Bottom;
    if (_wcsicmp(value, L"Left") == 0)        return WindowAlignment::Left;
    if (_wcsicmp(value, L"Right") == 0)       return WindowAlignment::Right;
    return WindowAlignment::Centered;
}

static bool GetPrivateProfileBoolW(const wchar_t* appName, const wchar_t* keyName, const wchar_t* defaultVal, const wchar_t* iniPath) {
    wchar_t buf[16]{};
    GetPrivateProfileStringW(appName, keyName, defaultVal, buf, 16, iniPath);
    return ParseIniBool(buf);
}

static void LoadProxyConfig() {
    if (g_proxyIniPath[0] == L'\0') return;

    if (GetFileAttributesW(g_proxyIniPath) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaultProxyIni();
    }

    wchar_t modeBuf[32]{};
    GetPrivateProfileStringW(L"Borderless", L"Mode", L"Fill",
                             modeBuf, 32, g_proxyIniPath);
    if (_wcsicmp(modeBuf, L"Windowed") == 0 ||
        _wcsicmp(modeBuf, L"Off") == 0) {
        g_mode = BorderlessMode::Windowed;
    } else if (_wcsicmp(modeBuf, L"NoFill") == 0 ||
               _wcsicmp(modeBuf, L"Stretch") == 0 ||
               _wcsicmp(modeBuf, L"Borderless") == 0 ||
               _wcsicmp(modeBuf, L"Fullscreen") == 0) {
        g_mode = BorderlessMode::NoFill;
    } else if (_wcsicmp(modeBuf, L"Centered") == 0 ||
               _wcsicmp(modeBuf, L"Centred") == 0) {
        // Legacy name: same as Fill + Centered alignment.
        g_mode = BorderlessMode::Fill;
    } else {
        g_mode = BorderlessMode::Fill;
    }

    wchar_t alignBuf[32]{};
    GetPrivateProfileStringW(L"Borderless", L"Alignment", L"Centered",
                             alignBuf, 32, g_proxyIniPath);
    g_alignment = ParseAlignment(alignBuf);

    g_hideTaskbar = GetPrivateProfileBoolW(L"Borderless", L"HideTaskbar", L"1", g_proxyIniPath);
    g_forceWindowed = GetPrivateProfileBoolW(L"Borderless", L"ForceWindowed", L"1", g_proxyIniPath);
    g_enableLog = GetPrivateProfileBoolW(L"Borderless", L"EnableLog", L"0", g_proxyIniPath);
    g_enableConsole = GetPrivateProfileBoolW(L"Borderless", L"EnableConsole", L"0", g_proxyIniPath);
    g_showSplashScreens = GetPrivateProfileBoolW(L"Borderless", L"SplashScreens", L"1", g_proxyIniPath);

    int w = (int)GetPrivateProfileIntW(L"Borderless", L"Width",  0, g_proxyIniPath);
    int h = (int)GetPrivateProfileIntW(L"Borderless", L"Height", 0, g_proxyIniPath);
    g_forceWidth  = (w > 0) ? w : 0;
    g_forceHeight = (h > 0) ? h : 0;
    g_stretchViewport = GetPrivateProfileBoolW(L"Borderless", L"StretchViewport", L"1", g_proxyIniPath);
}

// ---------------------------------------------------------------------------
// Splash screen skip (Steam swkotor2.exe: PreloadInitialAssetsWrapper).
// A single RET at the function entry makes it return without loading logos.
// ---------------------------------------------------------------------------
static constexpr uintptr_t kPreloadInitialAssetsWrapper = 0x73f050;

static unsigned char g_splashOriginalByte = 0;
static bool          g_splashPatchApplied = false;

static bool DisableSplashScreens() {
    auto* target = reinterpret_cast<unsigned char*>(kPreloadInitialAssetsWrapper);
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DebugLog("Splash skip: VirtualProtect failed (%lu).", GetLastError());
        return false;
    }

    g_splashOriginalByte = *target;
    *target = 0xC3; // ret

    DWORD ignored = 0;
    VirtualProtect(target, 1, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    g_splashPatchApplied = true;
    DebugLog("Splash skip: patched PreloadInitialAssetsWrapper @ 0x%08X.", (unsigned)kPreloadInitialAssetsWrapper);
    return true;
}

static void RestoreSplashScreens() {
    if (!g_splashPatchApplied) return;

    auto* target = reinterpret_cast<unsigned char*>(kPreloadInitialAssetsWrapper);
    DWORD oldProtect = 0;
    if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *target = g_splashOriginalByte;
        DWORD ignored = 0;
        VirtualProtect(target, 1, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), target, 1);
    }
    g_splashPatchApplied = false;
}

// ---------------------------------------------------------------------------
// swkotor2.ini enforcement.
//
// Forced (every launch, only when ForceWindowed=1):
//   - FullScreen=0 / AllowWindowedMode=1
//
// Resolution is forced only when Width/Height are set in dinput8.ini. The proxy
// keeps whatever the engine renders at, so the render resolution must be set in
// swkotor2.ini before the engine reads it in WinMain; we can't enlarge the GL
// viewport after init without desyncing/crashing the renderer.
// ---------------------------------------------------------------------------
static void EnforceGameIniValues() {
    if (g_gameIniPath[0] == L'\0') return;

    if (GetFileAttributesW(g_gameIniPath) == INVALID_FILE_ATTRIBUTES) {
        DebugLog("swkotor2.ini not found at %ls (skipping enforcement).", g_gameIniPath);
        return;
    }

    if (g_forceWindowed) {
        WritePrivateProfileStringW(L"Display Options",  L"FullScreen",        L"0", g_gameIniPath);
        WritePrivateProfileStringW(L"Graphics Options", L"FullScreen",        L"0", g_gameIniPath);
        WritePrivateProfileStringW(L"Graphics Options", L"AllowWindowedMode", L"1", g_gameIniPath);
        DebugLog("Enforced FullScreen=0 / AllowWindowedMode=1 in swkotor2.ini.");
    }

    if (g_forceWidth > 0 && g_forceHeight > 0) {
        wchar_t wBuf[16], hBuf[16];
        _snwprintf_s(wBuf, _TRUNCATE, L"%d", g_forceWidth);
        _snwprintf_s(hBuf, _TRUNCATE, L"%d", g_forceHeight);
        WritePrivateProfileStringW(L"Graphics Options", L"Width",  wBuf, g_gameIniPath);
        WritePrivateProfileStringW(L"Graphics Options", L"Height", hBuf, g_gameIniPath);
        DebugLog("Enforced resolution %dx%d in swkotor2.ini [Graphics Options].",
                 g_forceWidth, g_forceHeight);
    }
}

// Render resolution the engine was told to use (dinput8.ini override or game INI).
static bool ReadConfiguredRenderSize(int* outW, int* outH) {
    if (!outW || !outH) return false;
    *outW = 0;
    *outH = 0;

    if (g_forceWidth > 0 && g_forceHeight > 0) {
        *outW = g_forceWidth;
        *outH = g_forceHeight;
        return true;
    }

    if (g_gameIniPath[0] == L'\0') return false;
    if (GetFileAttributesW(g_gameIniPath) == INVALID_FILE_ATTRIBUTES) return false;

    const int w = (int)GetPrivateProfileIntW(L"Graphics Options", L"Width",  0, g_gameIniPath);
    const int h = (int)GetPrivateProfileIntW(L"Graphics Options", L"Height", 0, g_gameIniPath);
    if (w > 0 && h > 0) {
        *outW = w;
        *outH = h;
        return true;
    }
    return false;
}

// After stripping caption/borders, size the client to match the configured render
// resolution and/or reclaim the non-client pixels we removed (fixes ~34px height
// loss and letterbox/crop when INI says 1440 but the bordered client was ~1406).
static void ComputeDesiredClientSize(
    int currentClientW, int currentClientH,
    LONG oldStyle, LONG oldExStyle,
    LONG newStyle, LONG newExStyle,
    int* outW, int* outH)
{
    int desiredW = currentClientW;
    int desiredH = currentClientH;

    RECT rc{ 0, 0, currentClientW, currentClientH };
    RECT rcOld = rc;
    RECT rcNew = rc;
    AdjustWindowRectEx(&rcOld, oldStyle, FALSE, oldExStyle);
    AdjustWindowRectEx(&rcNew, newStyle, FALSE, newExStyle);
    const int reclaimW = (rcOld.right  - rcOld.left) - (rcNew.right  - rcNew.left);
    const int reclaimH = (rcOld.bottom - rcOld.top)  - (rcNew.bottom - rcNew.top);
    if (reclaimW > 0) desiredW = currentClientW + reclaimW;
    if (reclaimH > 0) desiredH = currentClientH + reclaimH;

    int cfgW = 0, cfgH = 0;
    if (ReadConfiguredRenderSize(&cfgW, &cfgH)) {
        if (cfgW > desiredW) desiredW = cfgW;
        if (cfgH > desiredH) desiredH = cfgH;
    }

    if (desiredW > g_monitorWidth)  desiredW = g_monitorWidth;
    if (desiredH > g_monitorHeight) desiredH = g_monitorHeight;

    *outW = desiredW;
    *outH = desiredH;
}

// Borderless popup: no overlapped frame / DWM caption band in the client.
static LONG MakeBorderlessPopupStyle(LONG style) {
    LONG popup = WS_POPUP | WS_VISIBLE;
    if (style & WS_CLIPSIBLINGS)  popup |= WS_CLIPSIBLINGS;
    if (style & WS_CLIPCHILDREN) popup |= WS_CLIPCHILDREN;
    return popup;
}

static LONG MakeBorderlessPopupExStyle(LONG exStyle) {
    const LONG kStrip = WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE
                      | WS_EX_WINDOWEDGE | WS_EX_TOPMOST;
    return exStyle & ~kStrip;
}

// Estimated phantom top band (caption + frame) the engine may still reserve.
static int MeasureTopChromeBandPx(LONG oldStyle, LONG oldExStyle) {
    RECT rc{ 0, 0, 256, 256 };
    RECT rcOld = rc;
    RECT rcPop = rc;
    AdjustWindowRectEx(&rcOld, oldStyle, FALSE, oldExStyle);
    AdjustWindowRectEx(&rcPop, WS_POPUP, FALSE, 0);
    int band = (-rcOld.top) - (-rcPop.top);
    const int metrics = GetSystemMetrics(SM_CYCAPTION)
                      + GetSystemMetrics(SM_CYFRAME)
                      + GetSystemMetrics(SM_CYBORDER);
    if (metrics > band) band = metrics;
    return band > 0 ? band : 0;
}

static void ApplyDwmClientBleed(HWND hwnd) {
    if (!hwnd) return;
    const MARGINS margins{ -1, -1, -1, -1 };
    const HRESULT hr = DwmExtendFrameIntoClientArea(hwnd, &margins);
    if (FAILED(hr)) {
        WorkerLog("dwm", "DwmExtendFrameIntoClientArea failed hr=0x%08lX", hr);
    }
}

// Pin to monitor top-left when the client fills the monitor (avoids phantom top gap).
static WindowAlignment EffectivePlacementForClientSize(int clientW, int clientH) {
    if (clientW >= g_monitorWidth - 1 || clientH >= g_monitorHeight - 1)
        return WindowAlignment::TopLeft;
    return g_alignment;
}

static WindowAlignment EffectivePlacementForHwnd(HWND hwnd) {
    RECT cr{};
    if (!GetClientRect(hwnd, &cr)) return g_alignment;
    const int w = cr.right  - cr.left;
    const int h = cr.bottom - cr.top;
    return EffectivePlacementForClientSize(w, h);
}

// ---------------------------------------------------------------------------
// OpenGL viewport / scissor hook (StretchY for phantom top title band).
// ---------------------------------------------------------------------------
typedef int   GLint;
typedef int   GLsizei;
typedef HDC (WINAPI* PFNWGLGETCURRENTDC)(void);
typedef void (__stdcall* PFNGLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
typedef void (__stdcall* PFNGLSCISSORPROC)(GLint x, GLint y, GLsizei width, GLsizei height);

static std::atomic<bool> g_glHooksInstalled{ false };
static int               g_glTopBand        = 0;
static PFNGLVIEWPORTPROC g_realGlViewport     = nullptr;
static PFNGLSCISSORPROC  g_realGlScissor      = nullptr;

static bool IsGameGlDrawable() {
    if (!g_gameHwndForStack || g_glTopBand <= 0) return false;
    const HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) return false;
    const auto wglGetCurrentDC =
        (PFNWGLGETCURRENTDC)GetProcAddress(gl, "wglGetCurrentDC");
    if (!wglGetCurrentDC) return false;
    const HDC dc = wglGetCurrentDC();
    if (!dc) return false;
    return WindowFromDC(dc) == g_gameHwndForStack;
}

// Reclaim the phantom top caption band on the engine's MAIN full-frame render.
//
// After the borderless pass the engine still reserves ~SM_CYCAPTION pixels of
// top chrome it no longer has, leaving a thin black band / crop at the top. We
// pull that main frame up to the client top and grow its height to fill the gap.
//
// We must NOT touch the letterboxed dialogue / in-engine cutscene sub-rects.
// Those use a large vertical offset (a centered cinematic band), and the engine
// pairs viewport and scissor rects that have to stay in sync. The old code
// rewrote every offset rect (forcing y=0 and extending height), which desynced
// the cinematic viewport from its scissor box: the 3D world got scissored away
// and rendered solid black while the separately-drawn subtitle/UI pass still
// showed. So we only adjust a rect that is (near) full client width AND covers
// the full client height to within the measured band - i.e. the real main frame
// - and pass everything else (cinematics, HUD sub-rects, scissor crops) through
// exactly as the engine set it.
static void AdjustGlFrameRect(GLint* /*x*/, GLint* y, GLsizei* w, GLsizei* h) {
    if (!g_stretchViewport || g_glTopBand <= 0) return;
    if (!IsGameGlDrawable()) return;

    RECT cr{};
    GetClientRect(g_gameHwndForStack, &cr);
    const int clientW = cr.right  - cr.left;
    const int clientH = cr.bottom - cr.top;
    if (clientW <= 0 || clientH <= 0) return;

    const int band = g_glTopBand;
    const int w0   = (int)*w;
    const int h0   = (int)*h;
    const int y0   = (int)*y;
    if (w0 <= 0 || h0 <= 0) return;

    // Tolerance for matching the main frame against the chrome band. Letterbox
    // cinematic bars are far taller than this, so they never qualify.
    const int slack = band + 8;

    const bool nearFullWidth    = w0 >= clientW - 8;
    const bool smallTopOffset   = y0 >= 0 && y0 <= slack;
    const bool coversFullHeight = (y0 + h0) >= clientH - slack;

    // Only the main near-full-screen frame is corrected; anything else is left
    // untouched so cinematic viewport/scissor pairs stay consistent.
    if (!(nearFullWidth && smallTopOffset && coversFullHeight)) return;

    if (y0 > 0) {
        const int expanded = h0 + y0;
        *h = (GLsizei)(expanded > clientH ? clientH : expanded);
        *y = 0;
    }
    if ((int)*h < clientH) {
        const int stretched = (int)*h + band;
        *h = (GLsizei)(stretched > clientH ? clientH : stretched);
    }
}

static void ScaleForShrunkEngine(GLint* x, GLint* y, GLsizei* w, GLsizei* h) {
    const int shrunkW = g_engineShrunkW.load();
    const int shrunkH = g_engineShrunkH.load();
    if (shrunkW <= 0 || shrunkH <= 0) return;

    RECT cr{};
    if (g_gameHwndForStack && GetClientRect(g_gameHwndForStack, &cr)) {
        const int clientW = cr.right  - cr.left;
        const int clientH = cr.bottom - cr.top;
        if (clientW > 0 && clientH > 0) {
            const double scaleX = (double)clientW / shrunkW;
            const double scaleY = (double)clientH / shrunkH;
            *x = (GLint)(*x * scaleX);
            *y = (GLint)(*y * scaleY);
            *w = (GLsizei)(*w * scaleX);
            *h = (GLsizei)(*h * scaleY);
        }
    }
}

static void __stdcall Hook_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLint ax = x, ay = y;
    GLsizei aw = width, ah = height;
    if (g_engineShrunkW.load() > 0 && g_engineShrunkH.load() > 0) {
        ScaleForShrunkEngine(&ax, &ay, &aw, &ah);
    } else {
        AdjustGlFrameRect(&ax, &ay, &aw, &ah);
    }
    if (g_realGlViewport) {
        g_realGlViewport(ax, ay, aw, ah);
    }
}

static void __stdcall Hook_glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLint ax = x, ay = y;
    GLsizei aw = width, ah = height;
    if (g_engineShrunkW.load() > 0 && g_engineShrunkH.load() > 0) {
        ScaleForShrunkEngine(&ax, &ay, &aw, &ah);
    } else {
        AdjustGlFrameRect(&ax, &ay, &aw, &ah);
    }
    if (g_realGlScissor) {
        g_realGlScissor(ax, ay, aw, ah);
    }
}

static bool InstallDetour5Byte(void* target, void* hook, void** trampolineOut) {
    if (!target || !hook || !trampolineOut) return false;

    auto* const t = reinterpret_cast<BYTE*>(target);
    auto* const tramp = VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;

    memcpy(tramp, t, 5);
    auto* const jmpBack = reinterpret_cast<BYTE*>(tramp) + 5;
    jmpBack[0] = 0xE9;
    *reinterpret_cast<DWORD*>(jmpBack + 1) = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(t + 5) - reinterpret_cast<DWORD_PTR>(jmpBack + 5));

    DWORD oldProt = 0;
    if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }

    t[0] = 0xE9;
    *reinterpret_cast<DWORD*>(t + 1) = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(hook) - reinterpret_cast<DWORD_PTR>(t + 5));
    VirtualProtect(t, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), t, 5);

    *trampolineOut = tramp;
    return true;
}

static bool InstallOpenGLViewportHooks(int topBand) {
    if (g_mode == BorderlessMode::Windowed)
        return false;
    if (g_glHooksInstalled.load()) return true;

    g_glTopBand = topBand;

    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) gl = LoadLibraryW(L"opengl32.dll");
    if (!gl) return false;

    void* const pViewport = (void*)GetProcAddress(gl, "glViewport");
    void* const pScissor  = (void*)GetProcAddress(gl, "glScissor");
    if (!pViewport) return false;

    void* trampVp = nullptr;
    if (!InstallDetour5Byte(pViewport, (void*)&Hook_glViewport, &trampVp)) return false;
    g_realGlViewport = (PFNGLVIEWPORTPROC)trampVp;

    if (pScissor) {
        void* trampSc = nullptr;
        if (InstallDetour5Byte(pScissor, (void*)&Hook_glScissor, &trampSc)) {
            g_realGlScissor = (PFNGLSCISSORPROC)trampSc;
        }
    }

    g_glHooksInstalled.store(true);
    return true;
}

// ---------------------------------------------------------------------------
// Backdrop window (Centered + HideTaskbar mode).
//
// Borderless TOPMOST black popup the size of the primary monitor. The game
// window is given WS_EX_TOPMOST as well and explicitly raised above the
// backdrop, so the z-order ends up:
//   game  (topmost, centered)
//   backdrop  (topmost, fullscreen black)
//   Shell_TrayWnd (topmost, fullscreen) ... but underneath both of ours
// which is what "taskbar hidden behind the process" requires.
// ---------------------------------------------------------------------------
static const wchar_t* kBackdropClassName      = L"KOTOR2BorderlessBackdrop";
static const wchar_t* kTaskbarShieldClassName = L"KOTOR2BorderlessTaskbarShield";

static LRESULT CALLBACK BackdropWndProc(HWND hWnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(hdc, &ps.rcPaint, black);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)w;
            RECT rc{};
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            return 1;
        }
        case WM_CLOSE:
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ACTIVATE:
            return 0;
    }
    return DefWindowProcW(hWnd, msg, w, l);
}

// NoFill + HideTaskbar: invisible fullscreen layer above the taskbar so the
// desktop shows through the letterbox while the taskbar stays hidden.
static LRESULT CALLBACK TaskbarShieldWndProc(HWND hWnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_PAINT:
            ValidateRect(hWnd, NULL);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ACTIVATE:
            return 0;
    }
    return DefWindowProcW(hWnd, msg, w, l);
}

static HWND CreateBackdropWindow() {
    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = BackdropWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kBackdropClassName;
    RegisterClassExW(&wc);  // ignore "already registered" errors

    // Intentionally NOT WS_EX_TOPMOST: topmost traps the game above every other
    // app and the backdrop can win the topmost z-order fight on refocus.
    HWND hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        kBackdropClassName, L"",
        WS_POPUP,
        g_monitorX, g_monitorY, g_monitorWidth, g_monitorHeight,
        NULL, NULL, hInst, NULL);

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

static HWND CreateTaskbarShieldWindow() {
    HINSTANCE hInst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = TaskbarShieldWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = kTaskbarShieldClassName;
    RegisterClassExW(&wc);  // ignore "already registered" errors

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kTaskbarShieldClassName, L"",
        WS_POPUP,
        g_monitorX, g_monitorY, g_monitorWidth, g_monitorHeight,
        NULL, NULL, hInst, NULL);

    if (hwnd) {
        SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

// ---------------------------------------------------------------------------
// Borderless worker.
// ---------------------------------------------------------------------------
static bool IsExcludedTopLevelWindow(HWND hwnd) {
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, sizeof(cls) / sizeof(wchar_t)) == 0) return false;
    // Things we created ourselves; never let them become the cached "game" window.
    if (_wcsicmp(cls, L"ConsoleWindowClass") == 0) return true;
    if (_wcsicmp(cls, kBackdropClassName) == 0) return true;
    if (_wcsicmp(cls, kTaskbarShieldClassName) == 0) return true;
    return false;
}

// Convert desired client size to outer-window size and position on the monitor.
// SetWindowPos expects outer dimensions; client size must match the GL viewport.
static void ComputeOuterCoords(int outerW, int outerH, WindowAlignment align, int* outX, int* outY) {
    int x = g_monitorX;
    int y = g_monitorY;
    switch (align) {
    case WindowAlignment::TopLeft:
        break;
    case WindowAlignment::TopRight:
        x += g_monitorWidth - outerW;
        break;
    case WindowAlignment::BottomLeft:
        y += g_monitorHeight - outerH;
        break;
    case WindowAlignment::BottomRight:
        x += g_monitorWidth - outerW;
        y += g_monitorHeight - outerH;
        break;
    case WindowAlignment::Top:
        x += (g_monitorWidth - outerW) / 2;
        break;
    case WindowAlignment::Bottom:
        x += (g_monitorWidth - outerW) / 2;
        y += g_monitorHeight - outerH;
        break;
    case WindowAlignment::Left:
        y += (g_monitorHeight - outerH) / 2;
        break;
    case WindowAlignment::Right:
        x += g_monitorWidth - outerW;
        y += (g_monitorHeight - outerH) / 2;
        break;
    case WindowAlignment::Centered:
    default:
        x += (g_monitorWidth  - outerW) / 2;
        y += (g_monitorHeight - outerH) / 2;
        break;
    }
    *outX = x;
    *outY = y;
}

static void ComputeAlignedPlacement(
    int desiredClientW, int desiredClientH,
    LONG style, LONG exStyle,
    WindowAlignment align,
    int* outX, int* outY, int* outOuterW, int* outOuterH)
{
    RECT rc{ 0, 0, desiredClientW, desiredClientH };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int outerW = rc.right  - rc.left;
    const int outerH = rc.bottom - rc.top;
    *outOuterW = outerW;
    *outOuterH = outerH;

    ComputeOuterCoords(outerW, outerH, align, outX, outY);
}

// Nudge the window so its outer rect matches the chosen alignment (used after
// SetWindowLong / z-order calls, which can shift the frame by a few pixels).
static void ApplyAlignmentToWindow(HWND hwnd, WindowAlignment align) {
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return;
    const int outerW = wr.right  - wr.left;
    const int outerH = wr.bottom - wr.top;

    int x = 0, y = 0;
    ComputeOuterCoords(outerW, outerH, align, &x, &y);

    if (wr.left != x || wr.top != y) {
        SetWindowPos(hwnd, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// The engine shrinks the HWND for Bink FMV (often 640x480). Re-apply our target
// client size so Fill/NoFill keeps the configured render resolution.
static void RestoreGameClientLayoutIfNeeded() {
    if (!g_targetLayoutSaved.load() || g_inLayoutRestore.load()) return;
    if (!g_gameHwndForStack || !IsWindow(g_gameHwndForStack)) return;
    if (IsIconic(g_gameHwndForStack)) {
        if (!IsEngineFmvActive()) return;
        ShowWindow(g_gameHwndForStack, SW_RESTORE);
    }
    if (g_mode == BorderlessMode::Windowed) return;
    if (g_targetClientW <= 0 || g_targetClientH <= 0) return;
    if (!IsGameSessionActive()) return;

    RECT cr{};
    if (!GetClientRect(g_gameHwndForStack, &cr)) return;
    const int cw = cr.right  - cr.left;
    const int ch = cr.bottom - cr.top;
    constexpr int kShrinkSlack = 48;
    if (cw >= g_targetClientW - kShrinkSlack && ch >= g_targetClientH - kShrinkSlack) {
        g_engineShrunkW.store(0);
        g_engineShrunkH.store(0);
        return;
    }

    static DWORD s_lastRestoreMs = 0;
    const DWORD now = GetTickCount();
    if (now - s_lastRestoreMs < 100) return;
    s_lastRestoreMs = now;

    g_inLayoutRestore.store(true);
    g_engineShrunkW.store(cw);
    g_engineShrunkH.store(ch);
    g_lastFmvShrinkMs.store(GetTickCount());

    int targetX = 0, targetY = 0, targetOuterW = 0, targetOuterH = 0;
    ComputeAlignedPlacement(g_targetClientW, g_targetClientH,
                            g_targetPopupStyle, g_targetPopupExStyle,
                            g_targetPlacement,
                            &targetX, &targetY, &targetOuterW, &targetOuterH);

    SetWindowLongW(g_gameHwndForStack, GWL_STYLE,   g_targetPopupStyle);
    SetWindowLongW(g_gameHwndForStack, GWL_EXSTYLE, g_targetPopupExStyle);

    SetWindowPos(g_gameHwndForStack, NULL,
                 targetX, targetY, targetOuterW, targetOuterH,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    ApplyDwmClientBleed(g_gameHwndForStack);
    ApplyAlignmentToWindow(g_gameHwndForStack, g_targetPlacement);

    WorkerLog("layout", "FMV shrink %dx%d -> restore target %dx%d",
              cw, ch, g_targetClientW, g_targetClientH);

    g_inLayoutRestore.store(false);
    UpdateFillBackdropForSession();
}

static BOOL CALLBACK FindGameWindowCallback(HWND hwnd, LPARAM lParam) {
    auto& data = *reinterpret_cast<TargetWindowData*>(lParam);
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(hwnd, &windowProcessId);

    if (windowProcessId != data.processId) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL)  return TRUE;
    if (!IsWindowVisible(hwnd))             return TRUE;
    if (IsExcludedTopLevelWindow(hwnd))     return TRUE;

    RECT clientRect{};
    GetClientRect(hwnd, &clientRect);
    const int w = clientRect.right  - clientRect.left;
    const int h = clientRect.bottom - clientRect.top;
    if (w <= 0 || h <= 0) return TRUE;

    const int area = w * h;
    if (area < 320 * 240) return TRUE;
    if (area > data.bestArea) {
        data.bestArea = area;
        data.hwnd     = hwnd;
    }
    return TRUE;
}

static constexpr int kPollMs        = 250;
static constexpr int kMaxScanTicks  = 1200;  // ~5 min before giving up
static constexpr int kSettleTicks   = 8;     // need 8*250ms = 2s of stability

static DWORD WINAPI BorderlessWorker(LPVOID /*lpParam*/) {
    if (g_mode == BorderlessMode::Windowed) return 0;

    TargetWindowData data{};
    data.processId = GetCurrentProcessId();

    WorkerLog("init", "borderless worker started.");

    // Phase 1: wait until a stable, visible game window exists.
    HWND gameHwnd = NULL;
    for (int tick = 0; tick < kMaxScanTicks; tick++) {
        data.hwnd     = NULL;
        data.bestArea = 0;
        EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&data));
        if (data.hwnd) { gameHwnd = data.hwnd; break; }
        Sleep(kPollMs);
    }
    if (!gameHwnd) {
        WorkerLog("done", "game window never appeared.");
        return 0;
    }

    char cls[64]{};
    GetClassNameA(gameHwnd, cls, (int)sizeof(cls));
    WorkerLog("scan", "found game hwnd=0x%p class='%s'.", (void*)gameHwnd, cls);

    // Phase 2: wait for engine init to settle (style/size unchanged across
    // kSettleTicks consecutive samples). The engine's own
    // AdjustWindowRect/SetWindowLong/SetWindowPos burst during renderer init
    // and again on the first menu transition; we MUST land between those.
    LONG prevStyle   = 0;
    LONG prevExStyle = 0;
    int  prevOuterW  = 0;
    int  prevOuterH  = 0;
    int  stable      = 0;
    bool primed      = false;
    bool engineSettled = false;

    for (int tick = 0; tick < kMaxScanTicks; tick++) {
        LONG style   = GetWindowLongW(gameHwnd, GWL_STYLE);
        LONG exStyle = GetWindowLongW(gameHwnd, GWL_EXSTYLE);

        RECT wr{};
        GetWindowRect(gameHwnd, &wr);
        int outerW = wr.right  - wr.left;
        int outerH = wr.bottom - wr.top;

        if (primed
            && style   == prevStyle
            && exStyle == prevExStyle
            && outerW  == prevOuterW
            && outerH  == prevOuterH) {
            stable++;
            if (stable >= kSettleTicks) {
                engineSettled = true;
                break;
            }
        } else {
            stable = 0;
            prevStyle   = style;
            prevExStyle = exStyle;
            prevOuterW  = outerW;
            prevOuterH  = outerH;
            primed      = true;
        }
        Sleep(kPollMs);
    }

    if (engineSettled) {
        WorkerLog("settled", "engine state stable; computing target.");
    } else {
        WorkerLog("settle", "engine never settled; applying anyway.");
    }

    // Phase 3: one-shot apply (WS_POPUP + DWM bleed + top-band height fix).
    LONG  style   = GetWindowLongW(gameHwnd, GWL_STYLE);
    LONG  exStyle = GetWindowLongW(gameHwnd, GWL_EXSTYLE);
    const LONG popupStyle = MakeBorderlessPopupStyle(style);
    const LONG popupExStyle = MakeBorderlessPopupExStyle(exStyle);

    RECT cr{};
    GetClientRect(gameHwnd, &cr);
    int cw = cr.right  - cr.left;
    int ch = cr.bottom - cr.top;

    if (cw <= 0 || ch <= 0) { cw = 1024; ch = 768; }

    int desiredClientW = 0;
    int desiredClientH = 0;
    ComputeDesiredClientSize(cw, ch, style, exStyle, popupStyle, popupExStyle,
                             &desiredClientW, &desiredClientH);

    int topBand = MeasureTopChromeBandPx(style, exStyle);

    // When the game runs below display resolution it is letterboxed (centered
    // with black borders in Fill mode). ComputeDesiredClientSize adds reclaimH
    // (~34 px of stripped chrome) to the client. The engine's GL viewport still
    // covers only the original render resolution, leaving a gap at the client
    // top. The engine maps mouse coordinates directly from client coordinates
    // (client_y=0 = top of client, NOT top of the render), so every click
    // registers above its visual position by exactly the gap size. Fix: when
    // sub-native, reset the client to the engine's actual render resolution
    // (cw/ch) so client and render are 1:1, and also zero topBand so the GL
    // viewport correction is skipped (it would shift/stretch the render away
    // from the 1:1 layout). For native res (fillsMonitor), keep the existing
    // reclaimH + topBand behaviour so the top chrome strip is reclaimed.
    const int kFillSlack = 2;
    const bool fillsMonitor =
        desiredClientW >= g_monitorWidth  - kFillSlack &&
        desiredClientH >= g_monitorHeight - kFillSlack;
    if (!fillsMonitor) {
        desiredClientW = cw;
        desiredClientH = ch;
        topBand = 0;
    } else if (topBand > 0) {
        const int expandedH = desiredClientH + topBand;
        if (expandedH <= g_monitorHeight) {
            desiredClientH = expandedH;
        } else if (desiredClientH < g_monitorHeight) {
            desiredClientH = g_monitorHeight;
        }
    }

    const WindowAlignment placement =
        EffectivePlacementForClientSize(desiredClientW, desiredClientH);

    // Fill and NoFill keep the engine render resolution (from INI / reclaimed
    // chrome) and Alignment. Only Fill creates the fullscreen black backdrop.

    // Fill uses a black backdrop; NoFill + HideTaskbar uses a transparent shield
    // above the taskbar so the desktop still shows in the letterbox area.
    // Both use the same focus hook to drop behind other apps on alt-tab.
    if (g_mode == BorderlessMode::Fill) {
        g_backdropHwnd = CreateBackdropWindow();
    } else if (g_mode == BorderlessMode::NoFill && g_hideTaskbar) {
        g_backdropHwnd = CreateTaskbarShieldWindow();
    }

    int targetX = 0, targetY = 0, targetW = 0, targetH = 0;
    ComputeAlignedPlacement(desiredClientW, desiredClientH,
                            popupStyle, popupExStyle, placement,
                            &targetX, &targetY, &targetW, &targetH);

    WorkerLog("apply", "mode=%d align=%d popup=1 topBand=%d engine client %dx%d -> "
              "target client %dx%d outer %dx%d @ (%d,%d) backdrop=%p",
              (int)g_mode, (int)placement, topBand, cw, ch,
              desiredClientW, desiredClientH,
              targetW, targetH, targetX, targetY, (void*)g_backdropHwnd);

    SetWindowLongW(gameHwnd, GWL_STYLE,   popupStyle);
    SetWindowLongW(gameHwnd, GWL_EXSTYLE, popupExStyle);

    SetWindowPos(gameHwnd, HWND_NOTOPMOST, targetX, targetY, targetW, targetH,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    ApplyDwmClientBleed(gameHwnd);

    g_gameHwndForStack = gameHwnd;

    ApplyAlignmentToWindow(gameHwnd, placement);

    RECT crAfter{};
    GetClientRect(gameHwnd, &crAfter);
    const int actualW = crAfter.right  - crAfter.left;
    const int actualH = crAfter.bottom - crAfter.top;
    if (actualW != desiredClientW || actualH != desiredClientH) {
        WorkerLog("apply", "post-set client %dx%d (wanted %dx%d)",
                  actualW, actualH, desiredClientW, desiredClientH);
    }

    g_targetClientW      = actualW > 0 ? actualW : desiredClientW;
    g_targetClientH      = actualH > 0 ? actualH : desiredClientH;
    g_targetPlacement    = placement;
    g_targetPopupStyle   = popupStyle;
    g_targetPopupExStyle = popupExStyle;
    g_targetLayoutSaved.store(true);

    if (InstallOpenGLViewportHooks(topBand)) {
        WorkerLog("glhook", "glViewport/glScissor hooks active (topBand=%d, stretch=%d).",
                  topBand, (int)g_stretchViewport);
    } else {
        WorkerLog("glhook", "failed to install OpenGL hooks.");
    }

    static HWINEVENTHOOK layoutHook = NULL;
    if (g_backdropHwnd) {
        RestackFillAfterGameLayoutChange();
    }

    if (!layoutHook && g_targetLayoutSaved.load()) {
        layoutHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            NULL, GameWindowLayoutCallback, 0, 0,
            WINEVENT_OUTOFCONTEXT);
    }

    if (g_backdropHwnd || UsesFocusZOrder()) {
        HANDLE hRestack = CreateThread(NULL, 0, DelayedRestackThread, NULL, 0, NULL);
        if (hRestack) CloseHandle(hRestack);
    }

    static HWINEVENTHOOK foregroundHook = NULL;
    static HWINEVENTHOOK minimizeHook = NULL;
    // Fill: backdrop stacking on layout; HideTaskbar/NoFill need z-order on foreground.
    if (!foregroundHook
        && (UsesFocusZOrder() || g_mode == BorderlessMode::Fill)) {
        SetBreadcrumb("BorderlessWorker: installing foreground hook");
        foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            NULL, ForegroundStackCallback, 0, 0,
            WINEVENT_OUTOFCONTEXT);
        if (UsesFocusZOrder()) {
            minimizeHook = SetWinEventHook(
                EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART,
                NULL, MinimizeStackCallback, 0, 0,
                WINEVENT_OUTOFCONTEXT);
        }
    }

    SetBreadcrumb("BorderlessWorker: before SetGameFocusZOrder");
    SetGameFocusZOrder(GetForegroundWindow() == g_gameHwndForStack);

    WorkerLog("done", "apply complete; transitioning to message pump.");
    SetBreadcrumb("BorderlessWorker: before GetMessage");

    // Phase 4: message pump for hooks. FMV may shrink the HWND; the layout hook
    // restores our saved client size (see RestoreGameClientLayoutIfNeeded).
    if (g_backdropHwnd || UsesFocusZOrder()) {
        MSG msg;
        bool firstDispatch = true;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            if (firstDispatch) {
                SetBreadcrumb("BorderlessWorker: inside message pump (GetMessage)");
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (firstDispatch) {
                SetBreadcrumb("BorderlessWorker: after first DispatchMessage");
                firstDispatch = false;
            }
        }
        SetBreadcrumb("BorderlessWorker: message pump exited");
    }
    return 0;
}

static void StartBorderlessWorkerOnce() {
    static std::atomic<bool> started{ false };
    if (!started.exchange(true)) {
        HANDLE h = CreateThread(NULL, 0, BorderlessWorker, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
}

extern "C" HRESULT WINAPI FakeDirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    static std::atomic<bool> announced{ false };
    if (!announced.exchange(true)) {
        DebugLog("FakeDirectInput8Create invoked by game engine.");
    }
    StartBorderlessWorkerOnce();

    static HMODULE hReal = LoadSystemDll(L"dinput8.dll");
    if (!hReal) {
        DebugLog("Failed to load system dinput8.dll");
        return E_FAIL;
    }

    auto Real = (LPDIRECTINPUT8CREATE)GetProcAddress(hReal, "DirectInput8Create");
    if (!Real) {
        DebugLog("Failed to resolve system DirectInput8Create");
        return E_FAIL;
    }

    return Real(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        InitializeCriticalSection(&g_logLock);
        g_logLockReady.store(true);

        // 1) Discover paths + monitor before anything else - both INI handling
        //    and the worker key off these.
        DetectExePaths();
        DetectTargetMonitorRect();
        SetBreadcrumb("DllMain: paths detected");

        // 2) Load dinput8.ini (creates a commented default beside the EXE on
        //    first run) before logging or crash handlers so EnableLog is known.
        LoadProxyConfig();

        if (g_enableLog) {
            FileLog("===================================================================");
            FileLog("dinput8 proxy attached. PID=%lu", GetCurrentProcessId());
            FileLog("Game folder: %ls", g_exeDir);
            InstallCrashHandlers();
        }

        if (g_enableConsole) {
            // Only allocate a console when the user explicitly opted in.
            // Allocating one on a game that doesn't expect it can destabilise IO.
            InitDebugConsole();
            DebugLog("dinput8 borderless proxy attached. PID=%lu", GetCurrentProcessId());
            DebugLog("Config: %ls", g_proxyIniPath[0] ? g_proxyIniPath : L"(path unknown)");
            DebugLog("Mode=%d Alignment=%d HideTaskbar=%d ForceWindowed=%d "
                     "StretchViewport=%d SplashScreens=%d EnableLog=%d EnableConsole=1",
                     (int)g_mode, (int)g_alignment, (int)g_hideTaskbar,
                     (int)g_forceWindowed, (int)g_stretchViewport,
                     (int)g_showSplashScreens, (int)g_enableLog);
            DebugLog("Monitor: %ldx%ld at (%ld,%ld)",
                     g_monitorWidth, g_monitorHeight, g_monitorX, g_monitorY);
        }

        // 3) Rewrite swkotor2.ini (windowed mode) before the engine reads it in WinMain.
        EnforceGameIniValues();

        // 4) Optionally skip startup splash screens before the engine runs.
        if (!g_showSplashScreens) {
            DisableSplashScreens();
        }

        // 5) Spawn the borderless worker. It does nothing if Mode=Windowed, and
        //    otherwise waits for the engine's main HWND to be visible and
        //    stable for ~2s before its single style/position write.
        StartBorderlessWorkerOnce();
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        RestoreSplashScreens();

        if (g_enableLog) {
            if (!g_crashHandled.load()) {
                DWORD exitCode = 0;
                GetExitCodeProcess(GetCurrentProcess(), &exitCode);
                FileLog("Process detach without prior crash report. PID=%lu "
                        "Last breadcrumb: %s  Process exit code: %lu",
                        GetCurrentProcessId(), g_lastBreadcrumb, exitCode);
            } else {
                FileLog("dinput8 proxy detaching after crash report. PID=%lu",
                        GetCurrentProcessId());
            }
            UninstallCrashHandlers();
        }
        if (g_logLockReady.exchange(false)) {
            DeleteCriticalSection(&g_logLock);
        }
    }
    return TRUE;
}
