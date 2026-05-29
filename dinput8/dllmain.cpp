#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <cstdarg>
#include <atomic>

#pragma comment(lib, "dbghelp.lib")

// dinput8.dll proxy (KOTOR II 32-bit OpenGL build imports DirectInput8Create).
//
// Borderless / taskbar coverage behaviour is driven by dinput8.ini sitting
// next to the game executable. A worker thread waits for the engine's main
// top-level window to be visible AND stable (no style/size changes for ~2s),
// then performs a single style/position write. We deliberately never re-apply
// in a loop and never subclass the engine's wndproc: racing the engine's own
// AdjustWindowRect + SetWindowLong + SetWindowPos sequence desyncs the GL
// viewport from the actual client rect and crashes the renderer ~15-60s
// later.

typedef HRESULT(WINAPI* LPDIRECTINPUT8CREATE)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

// ---------------------------------------------------------------------------
// Configuration (dinput8.ini).
// ---------------------------------------------------------------------------
enum class BorderlessMode {
    Windowed,   // pure pass-through, no window changes
    Fill,       // engine resolution + black borders (fullscreen backdrop)
    NoFill,     // window stretches to monitor, no black borders
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
static bool             g_forceMonitorRes    = false;
static bool             g_enableConsole      = false;
static bool             g_showSplashScreens  = true;

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

static HWND g_backdropHwnd = NULL;
static HWND g_gameHwndForStack = NULL;

static void SetBreadcrumb(const char* crumb) {
    if (!crumb || !crumb[0]) return;
    if (g_logLockReady.load()) EnterCriticalSection(&g_logLock);
    strncpy_s(g_lastBreadcrumb, crumb, _TRUNCATE);
    if (g_logLockReady.load()) LeaveCriticalSection(&g_logLock);
}

// Re-seat the Fill-mode backdrop behind the game. Only touches the backdrop
// window we own; never moves or resizes the game HWND after the one-shot apply.
static void RestackFillWindows() {
    if (!g_gameHwndForStack || !g_backdropHwnd) return;
    if (!IsWindow(g_gameHwndForStack) || !IsWindow(g_backdropHwnd)) return;

    SetWindowPos(g_backdropHwnd, NULL,
                 g_monitorX, g_monitorY, g_monitorWidth, g_monitorHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // A real HWND in hWndInsertAfter means "place hWnd behind this window".
    SetWindowPos(g_backdropHwnd, g_gameHwndForStack, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// When focused, cover the taskbar via HWND_TOPMOST; release on alt-tab.
static void SetGameAboveTaskbar(bool above) {
    if (!g_hideTaskbar) return;
    if (!g_gameHwndForStack || !IsWindow(g_gameHwndForStack)) return;

    const HWND insertAfter = above ? HWND_TOPMOST : HWND_NOTOPMOST;
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW;

    if (g_backdropHwnd && IsWindow(g_backdropHwnd)) {
        SetWindowPos(g_backdropHwnd, insertAfter, 0, 0, 0, 0, flags);
    }
    SetWindowPos(g_gameHwndForStack, insertAfter, 0, 0, 0, 0, flags);

    if (g_backdropHwnd && IsWindow(g_backdropHwnd)) {
        RestackFillWindows();
    }
}

static VOID CALLBACK ForegroundStackCallback(
    HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD /*idEventThread*/, DWORD /*dwmsEventTime*/)
{
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    if (g_inForegroundCallback.exchange(true)) return;

    SetBreadcrumb("ForegroundStackCallback: entered");
    if (hwnd == g_gameHwndForStack) {
        SetGameAboveTaskbar(true);
    } else if (hwnd != g_backdropHwnd) {
        SetGameAboveTaskbar(false);
    }
    SetBreadcrumb("ForegroundStackCallback: done");
    g_inForegroundCallback.store(false);
}

static DWORD WINAPI DelayedRestackThread(LPVOID /*lpParam*/) {
    static const DWORD kDelaysMs[] = { 400, 1200, 3000 };
    for (DWORD delay : kDelaysMs) {
        Sleep(delay);
        if (!g_backdropHwnd || !g_gameHwndForStack) break;
        if (GetForegroundWindow() != g_gameHwndForStack) continue;
        RestackFillWindows();
        RedrawWindow(g_backdropHwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    return 0;
}

struct TargetWindowData {
    DWORD processId;
    HWND  hwnd;
    int   bestArea;
};

// ---------------------------------------------------------------------------
// Logging.
//
// FileLog always appends a timestamped line to dinput8.log in the game folder
// (independent of EnableConsole) so we have a persistent record + crash trail.
// DebugLog / WorkerLog additionally echo to the console when EnableConsole=1.
// ---------------------------------------------------------------------------
static void FileLogRaw(const char* text) {
    if (g_logPath[0] == L'\0') return;
    const bool lock = g_logLockReady.load();
    if (lock) EnterCriticalSection(&g_logLock);

    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        DWORD written = 0;
        WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
        CloseHandle(h);
    }

    if (lock) LeaveCriticalSection(&g_logLock);
}

static void FileLogV(const char* tag, const char* format, va_list args) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    char line[2048];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "[%04d-%02d-%02d %02d:%02d:%02d.%03d][pid:%lu][tid:%lu]%s ",
                        st.wYear, st.wMonth, st.wDay,
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        GetCurrentProcessId(), GetCurrentThreadId(),
                        tag ? tag : "");
    if (n < 0) n = 0;

    if (format) {
        int m = _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, format, args);
        if (m > 0) n += m;
    }
    if (n > (int)sizeof(line) - 3) n = (int)sizeof(line) - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    line[n]   = '\0';

    FileLogRaw(line);
}

static void FileLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    FileLogV("", format, args);
    va_end(args);
}

static void ConsoleWriteV(const char* format, va_list args) {
    char buf[2048];
    const int n = _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, format, args);
    if (n <= 0) return;

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        WriteConsoleA(hOut, buf, (DWORD)n, &written, NULL);
    } else {
        OutputDebugStringA(buf);
    }
}

static void ConsoleWriteLine(const char* format, ...) {
    va_list args;
    va_start(args, format);
    ConsoleWriteV(format, args);
    va_end(args);
    OutputDebugStringA("\n");
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD written = 0;
        WriteConsoleA(hOut, "\r\n", 2, &written, NULL);
    }
}

static void DebugLog(const char* format, ...) {
    va_list args;
    va_start(args, format);
    FileLogV("", format, args);
    va_end(args);

    if (g_enableConsole) {
        char msg[1900];
        va_start(args, format);
        _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, format, args);
        va_end(args);
        ConsoleWriteLine("[KOTOR2-BORDERLESS] %s", msg);
    }
}

static void WorkerLog(const char* branch, const char* format, ...) {
    char tag[80];
    _snprintf_s(tag, sizeof(tag), _TRUNCATE, "[t:%lu][%s]", GetTickCount(),
                branch ? branch : "");

    va_list args;
    va_start(args, format);
    FileLogV(tag, format, args);
    va_end(args);

    if (g_enableConsole) {
        char msg[1900];
        va_start(args, format);
        _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, format, args);
        va_end(args);
        ConsoleWriteLine("[KOTOR2-BORDERLESS] t=%lu [%s] %s",
                         GetTickCount(), branch ? branch : "", msg);
    }
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
    FileLog("Last breadcrumb: %s", g_lastBreadcrumb);
    FileLog("Exception: 0x%08lX (%s)", code, ExceptionCodeToString(code));
    FileLog("Fault address: 0x%p  (%s)", (void*)faultAddr, faultMod);

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
        ";   NoFill  - Game window covers the monitor; no proxy black backdrop.\r\n"
        ";             The engine may still render below monitor resolution and\r\n"
        ";             show its own margin. Use ForceMonitorResolution=1 to match\r\n"
        ";             native monitor size for a true stretch.\r\n"
        ";   Windowed - Pure pass-through; no window or INI changes.\r\n"
        "Mode=Fill\r\n"
        "\r\n"
        "; Alignment (Fill mode): where the game window sits on the monitor.\r\n"
        ";   Centered, TopLeft, TopRight, BottomLeft, BottomRight,\r\n"
        ";   Top, Bottom, Left, Right\r\n"
        "Alignment=Centered\r\n"
        "\r\n"
        "; HideTaskbar:\r\n"
        ";   1 - Keep the game (and Fill-mode backdrop) above the taskbar.\r\n"
        ";   0 - Leave the desktop and taskbar untouched.\r\n"
        "HideTaskbar=1\r\n"
        "\r\n"
        "; ForceWindowed:\r\n"
        ";   1 - Rewrite swkotor2.ini: FullScreen=0, AllowWindowedMode=1.\r\n"
        ";   0 - Leave swkotor2.ini display mode keys alone.\r\n"
        "ForceWindowed=1\r\n"
        "\r\n"
        "; ForceMonitorResolution (NoFill mode only):\r\n"
        ";   1 - Rewrite swkotor2.ini Width/Height to the monitor size.\r\n"
        ";   0 - Leave Width/Height alone (use with Fill / in-game options).\r\n"
        "ForceMonitorResolution=0\r\n"
        "\r\n"
        "; EnableConsole:\r\n"
        ";   1 - Open a debug console (WriteConsole only; does not redirect stdio).\r\n"
        ";   0 - Run silent (recommended for normal play).\r\n"
        "; Crash reports are always appended to dinput8.log beside the game exe.\r\n"
        "EnableConsole=0\r\n"
        "\r\n"
        "; SplashScreens:\r\n"
        ";   1 - Show BioWare/Obsidian logo splash screens on startup.\r\n"
        ";   0 - Skip splash screens (Steam swkotor2.exe build only).\r\n"
        "SplashScreens=1\r\n"
        "\r\n"
        "; Overlays: KOTOR II uses legacy OpenGL. Discord (including Legacy Overlay)\r\n"
        "; and Steam in-game overlay hook GL and can crash after borderless changes.\r\n"
        "; Disable both for swkotor2.exe. See README.txt and dinput8.log if needed.\r\n";

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

    wchar_t boolBuf[16]{};
    GetPrivateProfileStringW(L"Borderless", L"HideTaskbar", L"1",
                             boolBuf, 16, g_proxyIniPath);
    g_hideTaskbar = ParseIniBool(boolBuf);
    GetPrivateProfileStringW(L"Borderless", L"ForceWindowed", L"1",
                             boolBuf, 16, g_proxyIniPath);
    g_forceWindowed = ParseIniBool(boolBuf);
    GetPrivateProfileStringW(L"Borderless", L"ForceMonitorResolution", L"1",
                             boolBuf, 16, g_proxyIniPath);
    g_forceMonitorRes = ParseIniBool(boolBuf);
    GetPrivateProfileStringW(L"Borderless", L"EnableConsole", L"0",
                             boolBuf, 16, g_proxyIniPath);
    g_enableConsole = ParseIniBool(boolBuf);
    GetPrivateProfileStringW(L"Borderless", L"SplashScreens", L"1",
                             boolBuf, 16, g_proxyIniPath);
    g_showSplashScreens = ParseIniBool(boolBuf);
}

// ---------------------------------------------------------------------------
// Splash screen skip (Steam swkotor2.exe: PreloadInitialAssetsWrapper).
// A single RET at the function entry makes it return without loading logos.
// ---------------------------------------------------------------------------
static constexpr uintptr_t kPreloadInitialAssetsWrapper = 0x73f050;

static unsigned char g_splashOriginalByte = 0;
static bool          g_splashPatchApplied = false;

static bool DisableSplashScreens() {
    void* target = reinterpret_cast<void*>(kPreloadInitialAssetsWrapper);
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        DebugLog("Splash skip: VirtualProtect failed (%lu).", GetLastError());
        return false;
    }

    g_splashOriginalByte = *static_cast<unsigned char*>(target);
    *static_cast<unsigned char*>(target) = 0xC3; // ret

    DWORD ignored = 0;
    VirtualProtect(target, 1, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    g_splashPatchApplied = true;
    DebugLog("Splash skip: patched PreloadInitialAssetsWrapper @ 0x%08X.", (unsigned)kPreloadInitialAssetsWrapper);
    return true;
}

static void RestoreSplashScreens() {
    if (!g_splashPatchApplied) return;

    void* target = reinterpret_cast<void*>(kPreloadInitialAssetsWrapper);
    DWORD oldProtect = 0;
    if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *static_cast<unsigned char*>(target) = g_splashOriginalByte;
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
// Forced (every launch, only when Mode=NoFill AND ForceMonitorResolution=1):
//   - Width / Height = primary monitor resolution
//
// Fill mode never forces resolution: black borders come from keeping the
// engine's chosen render size and painting a backdrop around the window.
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

    if (g_mode == BorderlessMode::NoFill && g_forceMonitorRes) {
        wchar_t wBuf[16]{};
        wchar_t hBuf[16]{};
        swprintf_s(wBuf, L"%ld", g_monitorWidth);
        swprintf_s(hBuf, L"%ld", g_monitorHeight);
        WritePrivateProfileStringW(L"Display Options",  L"Width",  wBuf, g_gameIniPath);
        WritePrivateProfileStringW(L"Display Options",  L"Height", hBuf, g_gameIniPath);
        WritePrivateProfileStringW(L"Graphics Options", L"Width",  wBuf, g_gameIniPath);
        WritePrivateProfileStringW(L"Graphics Options", L"Height", hBuf, g_gameIniPath);
        DebugLog("Forced engine resolution to %ldx%ld in swkotor2.ini.",
                 g_monitorWidth, g_monitorHeight);
    }
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
static const wchar_t* kBackdropClassName = L"KOTOR2BorderlessBackdrop";

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
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
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

// ---------------------------------------------------------------------------
// Borderless worker.
// ---------------------------------------------------------------------------
static bool IsExcludedTopLevelWindow(HWND hwnd) {
    char cls[64]{};
    if (GetClassNameA(hwnd, cls, (int)sizeof(cls)) == 0) return false;
    // Things we created ourselves; never let them become the cached "game" window.
    if (_stricmp(cls, "ConsoleWindowClass") == 0) return true;
    if (_stricmp(cls, "KOTOR2BorderlessBackdrop") == 0) return true;
    return false;
}

// Convert desired client size to outer-window size and position on the monitor.
// SetWindowPos expects outer dimensions; client size must match the GL viewport.
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

// Nudge the window so its outer rect matches the chosen alignment (used after
// SetWindowLong / z-order calls, which can shift the frame by a few pixels).
static void ApplyAlignmentToWindow(HWND hwnd, WindowAlignment align) {
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return;
    const int outerW = wr.right  - wr.left;
    const int outerH = wr.bottom - wr.top;

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

    if (wr.left != x || wr.top != y) {
        SetWindowPos(hwnd, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

static BOOL CALLBACK FindGameWindowCallback(HWND hwnd, LPARAM lParam) {
    TargetWindowData& data = *(TargetWindowData*)lParam;
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
        EnumWindows(FindGameWindowCallback, (LPARAM)&data);
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

    // Phase 3: one-shot apply.
    const LONG kStyleStripMask = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX
                               | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER
                               | WS_DLGFRAME;
    const LONG kExStyleStripMask = WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE
                                 | WS_EX_STATICEDGE    | WS_EX_WINDOWEDGE;

    LONG  style   = GetWindowLongW(gameHwnd, GWL_STYLE);
    LONG  exStyle = GetWindowLongW(gameHwnd, GWL_EXSTYLE);
    LONG  newStyle   = style   & ~kStyleStripMask;
    LONG  newExStyle = exStyle & ~kExStyleStripMask;

    RECT cr{};
    GetClientRect(gameHwnd, &cr);
    int cw = cr.right  - cr.left;
    int ch = cr.bottom - cr.top;

    int desiredClientW = g_monitorWidth;
    int desiredClientH = g_monitorHeight;

    if (g_mode == BorderlessMode::Fill) {
        // Always match the engine's render resolution; black borders come from
        // the fullscreen backdrop, not from stretching the window frame.
        if (cw <= 0 || ch <= 0) { cw = 1024; ch = 768; }
        if (cw > g_monitorWidth)  cw = g_monitorWidth;
        if (ch > g_monitorHeight) ch = g_monitorHeight;
        desiredClientW = cw;
        desiredClientH = ch;
    } else { // NoFill
        desiredClientW = g_monitorWidth;
        desiredClientH = g_monitorHeight;
        constexpr int kResSlack = 16;
        if (cw > 0 && ch > 0
            && (abs(cw - g_monitorWidth) > kResSlack
                || abs(ch - g_monitorHeight) > kResSlack)) {
            WorkerLog("nofill", "engine client %dx%d; window stretched to monitor "
                      "%ldx%ld (engine may still render smaller).",
                      cw, ch, g_monitorWidth, g_monitorHeight);
        }
    }

    // Fill uses a normal (non-topmost) backdrop so Alt+Tab and other apps can
    // cover the game. HideTaskbar may let the taskbar peek when unfocused.
    if (g_mode == BorderlessMode::Fill) {
        g_backdropHwnd = CreateBackdropWindow();
    }

    // Strip topmost if the engine had it; Fill/NoFill letterbox must not use it.
    newExStyle &= ~WS_EX_TOPMOST;

    int targetX = 0, targetY = 0, targetW = 0, targetH = 0;
    ComputeAlignedPlacement(desiredClientW, desiredClientH,
                            newStyle, newExStyle, g_alignment,
                            &targetX, &targetY, &targetW, &targetH);

    WorkerLog("apply", "mode=%d align=%d client %dx%d -> outer %dx%d @ (%d,%d) "
              "backdrop=%p (non-topmost)",
              (int)g_mode, (int)g_alignment,
              desiredClientW, desiredClientH,
              targetW, targetH, targetX, targetY, (void*)g_backdropHwnd);

    SetWindowLongW(gameHwnd, GWL_STYLE,   newStyle);
    SetWindowLongW(gameHwnd, GWL_EXSTYLE, newExStyle);

    SetWindowPos(gameHwnd, HWND_NOTOPMOST, targetX, targetY, targetW, targetH,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    g_gameHwndForStack = gameHwnd;

    ApplyAlignmentToWindow(gameHwnd, g_alignment);

    if (g_backdropHwnd) {
        RestackFillWindows();
        RedrawWindow(g_backdropHwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);

        HANDLE hRestack = CreateThread(NULL, 0, DelayedRestackThread, NULL, 0, NULL);
        if (hRestack) CloseHandle(hRestack);
    }

    static HWINEVENTHOOK foregroundHook = NULL;
    if (!foregroundHook && g_hideTaskbar) {
        SetBreadcrumb("BorderlessWorker: installing foreground hook");
        foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            NULL, ForegroundStackCallback, 0, 0,
            WINEVENT_OUTOFCONTEXT);
    }

    SetBreadcrumb("BorderlessWorker: before SetGameAboveTaskbar");
    SetGameAboveTaskbar(GetForegroundWindow() == g_gameHwndForStack);

    WorkerLog("done", "apply complete; transitioning to message pump.");
    SetBreadcrumb("BorderlessWorker: before GetMessage");

    // Phase 4: message pump for the foreground hook and backdrop repaints.
    // We never touch the game window again from here on.
    if (g_backdropHwnd || g_hideTaskbar) {
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

        // 0) Bring up file logging + the crash handler first, so any fault in
        //    the engine (or in our own init below) is captured to dinput8.log.
        InitializeCriticalSection(&g_logLock);
        g_logLockReady.store(true);

        // 1) Discover paths + monitor before anything else - both INI handling
        //    and the worker key off these.
        DetectExePaths();
        DetectTargetMonitorRect();
        SetBreadcrumb("DllMain: paths detected");

        FileLog("===================================================================");
        FileLog("dinput8 proxy attached. PID=%lu", GetCurrentProcessId());
        FileLog("Game folder: %ls", g_exeDir);
        InstallCrashHandlers();

        // 2) Load dinput8.ini (creates a commented default beside the EXE on
        //    first run) and only allocate a console if the user explicitly
        //    opted in. Allocating a console on a game that doesn't expect one
        //    can destabilise its IO and is a likely cause of mysterious
        //    "crashes after ~30-60s" reports.
        LoadProxyConfig();

        if (g_enableConsole) {
            InitDebugConsole();
            DebugLog("dinput8 borderless proxy attached. PID=%lu", GetCurrentProcessId());
            DebugLog("Config: %ls", g_proxyIniPath[0] ? g_proxyIniPath : L"(path unknown)");
            DebugLog("Mode=%d Alignment=%d HideTaskbar=%d ForceWindowed=%d "
                     "ForceMonitorResolution=%d SplashScreens=%d EnableConsole=1",
                     (int)g_mode, (int)g_alignment, (int)g_hideTaskbar,
                     (int)g_forceWindowed, (int)g_forceMonitorRes,
                     (int)g_showSplashScreens);
            DebugLog("Monitor: %ldx%ld at (%ld,%ld)",
                     g_monitorWidth, g_monitorHeight, g_monitorX, g_monitorY);
        }

        // 3) Rewrite swkotor2.ini (windowed mode + optional forced resolution)
        //    before the engine reads it in WinMain.
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
        if (g_logLockReady.exchange(false)) {
            DeleteCriticalSection(&g_logLock);
        }
    }
    return TRUE;
}
