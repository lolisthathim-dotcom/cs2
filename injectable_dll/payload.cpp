// payload.cpp — Educational DLL Injection Payload
// Demonstrates: DllMain lifecycle, Win32 overlay menu, in-process memory read,
//               command-button forcing, and clean self-unload.
//
// Hotkeys:
//   DELETE  — toggle menu overlay
//   INSERT  — toggle jump feature (when menu is visible)
//   END     — unload DLL
//
// Build: x64 Release DLL.  All names are placeholders.

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <atomic>

// timeBeginPeriod / timeEndPeriod
#pragma comment(lib, "winmm.lib")

// ============================================================================
// Offsets  (Build 14196 — 2026-11-05)
// ============================================================================

namespace offsets {
    constexpr std::ptrdiff_t dwLocalPlayerPawn = 0x256A948;  // client.dll
    constexpr std::ptrdiff_t m_fFlags          = 0x3F4;      // C_BaseEntity (uint32)
    constexpr std::ptrdiff_t m_iHealth         = 0x34C;      // C_BaseEntity (int32)
    constexpr std::ptrdiff_t jump              = 0x2239460;  // buttons (client.dll)
}

constexpr uint32_t FL_ONGROUND = (1 << 0);

// ============================================================================
// Menu layout / style
// ============================================================================

constexpr int   MENU_X      = 40;
constexpr int   MENU_Y      = 40;
constexpr int   MENU_W      = 290;
constexpr int   MENU_H      = 135;
constexpr BYTE  MENU_ALPHA  = 240;              // overlay opacity (0–255)
constexpr int   TIMER_REDRAW = 1;
constexpr int   REDRAW_MS    = 33;               // ~30 fps repaint

// Colours
constexpr COLORREF CLR_BG       = RGB(18, 18, 26);
constexpr COLORREF CLR_BORDER   = RGB(75, 60, 130);
constexpr COLORREF CLR_ACCENT   = RGB(130, 90, 220);
constexpr COLORREF CLR_TITLE    = RGB(190, 160, 255);
constexpr COLORREF CLR_LABEL    = RGB(190, 190, 200);
constexpr COLORREF CLR_ON       = RGB(80, 220, 120);
constexpr COLORREF CLR_OFF      = RGB(220, 75, 75);
constexpr COLORREF CLR_DIM      = RGB(100, 100, 115);
constexpr COLORREF CLR_HEADER   = RGB(24, 24, 34);

// ============================================================================
// Globals
// ============================================================================

static HMODULE            g_OwnModule     = nullptr;
static HANDLE             g_FeatureThread = nullptr;
static HANDLE             g_MenuThread    = nullptr;
static HWND               g_OverlayWnd    = nullptr;

static std::atomic<bool>  g_Running      { true  };
static std::atomic<bool>  g_MenuVisible  { false };
static std::atomic<bool>  g_JumpEnabled  { false };

// Cached for console display
static uintptr_t          g_ClientBase   = 0;

// ============================================================================
// Helpers
// ============================================================================

static uintptr_t GetModuleBase(const wchar_t* name)
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(name));
}

template <typename T>
static bool Mem(uintptr_t addr, T& out)
{
    if (!addr) return false;
    __try {
        out = *reinterpret_cast<T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ForceButton(uintptr_t clientBase, std::ptrdiff_t btnOff, bool press)
{
    uintptr_t addr = clientBase + btnOff;
    __try {
        *reinterpret_cast<int*>(addr) = press ? 65537 : 256;
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ============================================================================
// Console — setup / log
// ============================================================================

static FILE* g_pCon = nullptr;

static void ConInit()
{
    AllocConsole();
    SetConsoleTitleA("payload — debug console");
    freopen_s(&g_pCon, "CONOUT$", "w", stdout);

    // Style the console window a bit
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SMALL_RECT sr = { 0, 0, 79, 24 };
    SetConsoleWindowInfo(hOut, TRUE, &sr);

    printf("========================================\n");
    printf("  payload — educational demo\n");
    printf("========================================\n");
    printf("  DEL   toggle menu\n");
    printf("  INS   toggle jump (menu must be open)\n");
    printf("  END   unload dll\n");
    printf("========================================\n\n");
}

static void ConShutdown()
{
    if (g_pCon) { fclose(g_pCon); g_pCon = nullptr; }
    FreeConsole();
}

// ============================================================================
// Overlay — paint
// ============================================================================

static HFONT g_hFontTitle = nullptr;
static HFONT g_hFontBody  = nullptr;

static void CreateFonts()
{
    g_hFontTitle = CreateFontA(
        18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
    g_hFontBody = CreateFontA(
        15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
}

static void DestroyFonts()
{
    if (g_hFontTitle) { DeleteObject(g_hFontTitle); g_hFontTitle = nullptr; }
    if (g_hFontBody)  { DeleteObject(g_hFontBody);  g_hFontBody  = nullptr; }
}

static void PaintMenu(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    // ---- background ----
    RECT rc;
    GetClientRect(hWnd, &rc);

    HBRUSH brBg = CreateSolidBrush(CLR_BG);
    FillRect(hdc, &rc, brBg);
    DeleteObject(brBg);

    // ---- border ----
    HPEN penBorder = CreatePen(PS_SOLID, 2, CLR_BORDER);
    HGDIOBJ oldPen = SelectObject(hdc, penBorder);
    HGDIOBJ oldBr  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, 0, 0, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(penBorder);

    // ---- header bar ----
    RECT rcHeader = { 1, 1, rc.right - 1, 30 };
    HBRUSH brHeader = CreateSolidBrush(CLR_HEADER);
    FillRect(hdc, &rcHeader, brHeader);
    DeleteObject(brHeader);

    // accent stripe under header
    RECT rcStripe = { 1, 30, rc.right - 1, 32 };
    HBRUSH brAccent = CreateSolidBrush(CLR_ACCENT);
    FillRect(hdc, &rcStripe, brAccent);
    DeleteObject(brAccent);

    SetBkMode(hdc, TRANSPARENT);

    // ---- title ----
    SelectObject(hdc, g_hFontTitle);
    SetTextColor(hdc, CLR_TITLE);
    RECT rcTitle = { 12, 6, rc.right - 10, 28 };
    DrawTextA(hdc, "PAYLOAD", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);

    // version tag right-aligned
    SetTextColor(hdc, CLR_DIM);
    DrawTextA(hdc, "v1.0", -1, &rcTitle, DT_RIGHT | DT_SINGLELINE);

    // ---- feature row ----
    SelectObject(hdc, g_hFontBody);
    int y = 44;

    // label
    SetTextColor(hdc, CLR_LABEL);
    RECT rcLabel = { 16, y, 180, y + 18 };
    DrawTextA(hdc, "[1]  Jump", -1, &rcLabel, DT_LEFT | DT_SINGLELINE);

    // status pill
    bool on = g_JumpEnabled.load(std::memory_order_relaxed);
    const char* tag = on ? "  ON " : " OFF ";
    COLORREF tagClr = on ? CLR_ON : CLR_OFF;

    // pill background
    RECT rcPill = { 218, y - 2, 274, y + 18 };
    HBRUSH brPill = CreateSolidBrush(on ? RGB(30, 60, 40) : RGB(60, 30, 30));
    FillRect(hdc, &rcPill, brPill);
    DeleteObject(brPill);

    // pill border
    HPEN penPill = CreatePen(PS_SOLID, 1, tagClr);
    oldPen = SelectObject(hdc, penPill);
    oldBr  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rcPill.left, rcPill.top, rcPill.right, rcPill.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(penPill);

    SetTextColor(hdc, tagClr);
    DrawTextA(hdc, tag, -1, &rcPill, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    // ---- separator ----
    y += 30;
    HPEN penSep = CreatePen(PS_SOLID, 1, RGB(45, 45, 60));
    oldPen = SelectObject(hdc, penSep);
    MoveToEx(hdc, 14, y, nullptr);
    LineTo(hdc, rc.right - 14, y);
    SelectObject(hdc, oldPen);
    DeleteObject(penSep);

    // ---- footer / hint ----
    y += 10;
    SetTextColor(hdc, CLR_DIM);
    SelectObject(hdc, g_hFontBody);
    RECT rcFoot = { 16, y, rc.right - 10, y + 16 };
    DrawTextA(hdc, "DEL menu   INS toggle   END unload", -1,
              &rcFoot, DT_LEFT | DT_SINGLELINE);

    EndPaint(hWnd, &ps);
}

// ============================================================================
// Overlay — WndProc
// ============================================================================

static LRESULT CALLBACK OverlayProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        PaintMenu(hWnd);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_REDRAW)
            InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1; // skip — we fill everything in WM_PAINT

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_REDRAW);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hWnd, msg, wp, lp);
    }
}

// ============================================================================
// Menu thread — overlay window + input polling
// ============================================================================

static DWORD WINAPI MenuThreadProc(LPVOID /*p*/)
{
    // ---- Console ----
    ConInit();

    // ---- Register class ----
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance      = g_OwnModule;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(CLR_BG);
    wc.lpszClassName = "PayloadOverlayClass";
    RegisterClassExA(&wc);

    CreateFonts();

    // ---- Create window (initially hidden) ----
    g_OverlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        "PayloadOverlayClass",
        nullptr,
        WS_POPUP,
        MENU_X, MENU_Y, MENU_W, MENU_H,
        nullptr, nullptr, g_OwnModule, nullptr);

    SetLayeredWindowAttributes(g_OverlayWnd, 0, MENU_ALPHA, LWA_ALPHA);
    SetTimer(g_OverlayWnd, TIMER_REDRAW, REDRAW_MS, nullptr);

    printf("[init] overlay window created\n");

    // ---- Message pump with hotkey polling ----
    MSG msg;
    bool prevDel = false;
    bool prevIns = false;

    while (g_Running.load(std::memory_order_relaxed))
    {
        // Drain window messages (non-blocking)
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_Running.store(false, std::memory_order_relaxed);
                goto cleanup;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // --- Hotkey: DELETE — toggle menu ---
        bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (del && !prevDel)
        {
            bool vis = !g_MenuVisible.load(std::memory_order_relaxed);
            g_MenuVisible.store(vis, std::memory_order_relaxed);
            ShowWindow(g_OverlayWnd, vis ? SW_SHOWNOACTIVATE : SW_HIDE);
            printf("[menu] %s\n", vis ? "opened" : "closed");
        }
        prevDel = del;

        // --- Hotkey: INSERT — toggle jump (only when menu open) ---
        bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (ins && !prevIns && g_MenuVisible.load(std::memory_order_relaxed))
        {
            bool val = !g_JumpEnabled.load(std::memory_order_relaxed);
            g_JumpEnabled.store(val, std::memory_order_relaxed);
            printf("[feature] jump %s\n", val ? "ON" : "OFF");
        }
        prevIns = ins;

        // --- Hotkey: END — unload ---
        if (GetAsyncKeyState(VK_END) & 1)
        {
            printf("[core] END pressed — unloading\n");
            g_Running.store(false, std::memory_order_relaxed);
            break;
        }

        Sleep(5);
    }

cleanup:
    if (g_OverlayWnd) { DestroyWindow(g_OverlayWnd); g_OverlayWnd = nullptr; }
    UnregisterClassA("PayloadOverlayClass", g_OwnModule);
    DestroyFonts();
    ConShutdown();
    return 0;
}

// ============================================================================
// Feature thread — jump logic (tight loop)
// ============================================================================

static DWORD WINAPI FeatureThreadProc(LPVOID /*p*/)
{
    // ---------------------------------------------------------------
    // FIX 1:  Force the system timer to 1 ms resolution.
    //
    // Without this, Sleep(1) actually sleeps ~15.6 ms (the default
    // Windows scheduler quantum).  That means we poll at ~64 Hz —
    // far too slow to catch every landing frame at 128-tick.
    // ---------------------------------------------------------------
    timeBeginPeriod(1);

    // Wait for client.dll
    while (g_Running.load(std::memory_order_relaxed))
    {
        g_ClientBase = GetModuleBase(L"client.dll");
        if (g_ClientBase) break;
        Sleep(200);
    }
    printf("[init] client.dll @ 0x%llX\n",
           static_cast<unsigned long long>(g_ClientBase));

    // ---------------------------------------------------------------
    // FIX 2:  State-based instead of press-sleep-release.
    //
    // Old code:  ForceButton(true) → Sleep(1) → ForceButton(false)
    //   Problem: the ~15 ms gap meant the game often never saw the
    //   press, or saw it on the wrong tick.  The release could land
    //   before the game even sampled the button.
    //
    // New code:  Every iteration, set the button to match the ground
    //   state.  On ground → button stays pressed.  Airborne → button
    //   stays released.  The game's own tick naturally picks up the
    //   transition — zero timing dependency on our side.
    // ---------------------------------------------------------------
    while (g_Running.load(std::memory_order_relaxed))
    {
        bool featureActive = g_JumpEnabled.load(std::memory_order_relaxed) &&
                             (GetAsyncKeyState(VK_SPACE) & 0x8000);

        if (featureActive)
        {
            uintptr_t pawn = 0;
            if (Mem<uintptr_t>(g_ClientBase + offsets::dwLocalPlayerPawn, pawn) && pawn)
            {
                uint32_t flags = 0;
                if (Mem<uint32_t>(pawn + offsets::m_fFlags, flags))
                {
                    // Ground → press, airborne → release.
                    // The game reads this on its next tick and acts on the
                    // transition from released→pressed as a single +jump.
                    ForceButton(g_ClientBase, offsets::jump,
                                (flags & FL_ONGROUND) != 0);
                }
            }
        }
        else
        {
            // Feature off or space released — ensure the button is cleared
            // so the game doesn't see a stuck +jump.
            if (g_ClientBase)
                ForceButton(g_ClientBase, offsets::jump, false);
        }

        Sleep(1); // now truly ~1 ms thanks to timeBeginPeriod(1)
    }

    // Cleanup: release the button and restore timer resolution
    if (g_ClientBase)
        ForceButton(g_ClientBase, offsets::jump, false);
    timeEndPeriod(1);

    return 0;
}

// ============================================================================
// DllMain
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        g_OwnModule = hModule;

        // Two threads: one for the UI/console, one for the feature loop
        g_MenuThread    = CreateThread(nullptr, 0, MenuThreadProc,    nullptr, 0, nullptr);
        g_FeatureThread = CreateThread(nullptr, 0, FeatureThreadProc, nullptr, 0, nullptr);
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_Running.store(false, std::memory_order_relaxed);

        // Give threads time to clean up
        if (g_FeatureThread) { WaitForSingleObject(g_FeatureThread, 2000); CloseHandle(g_FeatureThread); }
        if (g_MenuThread)    { WaitForSingleObject(g_MenuThread,    2000); CloseHandle(g_MenuThread);    }
    }

    return TRUE;
}
