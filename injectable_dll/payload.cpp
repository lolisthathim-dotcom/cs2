// payload.cpp — Educational DLL Injection Payload
// Demonstrates: DllMain lifecycle, interactive Win32 overlay, in-process
//               memory read, entity iteration, W2S, 2D/3D box ESP,
//               command-button forcing, and clean self-unload.
//
// Hotkeys:
//   DELETE  — toggle menu
//   END     — unload DLL
//
// Menu features are toggled by clicking their row.
// Build: x64 Release DLL.

#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <atomic>

#pragma comment(lib, "winmm.lib")

// ============================================================================
// Offsets  (Build 14196 — 2026-11-05)
// ============================================================================

namespace offsets {
    constexpr std::ptrdiff_t dwLocalPlayerPawn       = 0x256A948;
    constexpr std::ptrdiff_t dwLocalPlayerController = 0x2542548;
    constexpr std::ptrdiff_t dwEntityList            = 0x271FBB8;
    constexpr std::ptrdiff_t dwViewMatrix            = 0x2570250;

    constexpr std::ptrdiff_t m_iHealth         = 0x34C;
    constexpr std::ptrdiff_t m_pGameSceneNode  = 0x330;
    constexpr std::ptrdiff_t m_pCollision      = 0x340;
    constexpr std::ptrdiff_t m_iTeamNum        = 0x3E7;
    constexpr std::ptrdiff_t m_fFlags          = 0x3F4;

    constexpr std::ptrdiff_t m_vecAbsOrigin    = 0xC8;
    constexpr std::ptrdiff_t m_bDormant        = 0x103;

    constexpr std::ptrdiff_t m_vecMins         = 0x40;
    constexpr std::ptrdiff_t m_vecMaxs         = 0x4C;

    constexpr std::ptrdiff_t m_hPlayerPawn     = 0x92C;
    constexpr std::ptrdiff_t m_bPawnIsAlive    = 0x934;

    constexpr std::ptrdiff_t jump              = 0x2239460;
}

constexpr uint32_t FL_ONGROUND = (1 << 0);

// ============================================================================
// Math
// ============================================================================

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct ViewMatrix4x4 { float m[4][4]; };

// ============================================================================
// ESP modes
// ============================================================================

enum class ESPMode : int { Off = 0, Box2D = 1, Box3D = 2, Count = 3 };
static const char* ESPModeStr[] = { "OFF", " 2D", " 3D" };

// ============================================================================
// Layout constants
// ============================================================================

constexpr int MENU_X = 40,  MENU_Y = 40;
constexpr int MENU_W = 290, MENU_H = 170;
constexpr int TIMER_REDRAW = 1, REDRAW_MS = 16;

// Row geometry (relative to menu top-left)
constexpr int ROW_H     = 24;               // clickable row height
constexpr int ROW1_Y    = 40;               // Jump row, relative to MENU_Y
constexpr int ROW2_Y    = ROW1_Y + 30;      // ESP row
constexpr int ROW_PAD_X = 6;                // inset from menu edges

// Absolute screen rects for hit-testing (computed from constants)
static const RECT MENU_RECT = { MENU_X, MENU_Y, MENU_X + MENU_W, MENU_Y + MENU_H };

static const RECT ROW1_RECT = {
    MENU_X + ROW_PAD_X, MENU_Y + ROW1_Y,
    MENU_X + MENU_W - ROW_PAD_X, MENU_Y + ROW1_Y + ROW_H };

static const RECT ROW2_RECT = {
    MENU_X + ROW_PAD_X, MENU_Y + ROW2_Y,
    MENU_X + MENU_W - ROW_PAD_X, MENU_Y + ROW2_Y + ROW_H };

// ============================================================================
// Colours
// ============================================================================

constexpr COLORREF CLR_TRANSPARENT = RGB(255, 0, 255);
constexpr COLORREF CLR_BG         = RGB(18, 18, 26);
constexpr COLORREF CLR_BORDER     = RGB(75, 60, 130);
constexpr COLORREF CLR_ACCENT     = RGB(130, 90, 220);
constexpr COLORREF CLR_TITLE      = RGB(190, 160, 255);
constexpr COLORREF CLR_LABEL      = RGB(190, 190, 200);
constexpr COLORREF CLR_ON         = RGB(80, 220, 120);
constexpr COLORREF CLR_OFF        = RGB(220, 75, 75);
constexpr COLORREF CLR_DIM        = RGB(100, 100, 115);
constexpr COLORREF CLR_HEADER     = RGB(24, 24, 34);
constexpr COLORREF CLR_ROW_HOVER  = RGB(30, 30, 44);
constexpr COLORREF CLR_ESP_ENEMY  = RGB(220, 50, 50);
constexpr COLORREF CLR_ESP_TEAM   = RGB(50, 140, 220);
constexpr COLORREF CLR_ESP_TEXT   = RGB(240, 240, 240);

// ============================================================================
// Globals
// ============================================================================

static HMODULE            g_OwnModule      = nullptr;
static HANDLE             g_FeatureThread  = nullptr;
static HANDLE             g_MenuThread     = nullptr;
static HWND               g_OverlayWnd     = nullptr;

static std::atomic<bool>  g_Running       { true  };
static std::atomic<bool>  g_MenuVisible   { false };
static std::atomic<bool>  g_JumpEnabled   { false };
static std::atomic<int>   g_ESPMode       { 0     };

static uintptr_t          g_ClientBase    = 0;
static int                g_ScreenW       = 0;
static int                g_ScreenH       = 0;

// ============================================================================
// Memory helpers
// ============================================================================

static uintptr_t GetModuleBase(const wchar_t* name)
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(name));
}

template <typename T>
static bool Mem(uintptr_t addr, T& out)
{
    if (!addr) return false;
    __try { out = *reinterpret_cast<T*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void ForceButton(uintptr_t base, std::ptrdiff_t off, bool press)
{
    __try { *reinterpret_cast<int*>(base + off) = press ? 65537 : 256; }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// ============================================================================
// World-to-screen
// ============================================================================

static bool W2S(const Vec3& w, const ViewMatrix4x4& vm, Vec2& o)
{
    float cw = vm.m[3][0]*w.x + vm.m[3][1]*w.y + vm.m[3][2]*w.z + vm.m[3][3];
    if (cw < 0.001f) return false;
    float inv = 1.f / cw;
    float nx = (vm.m[0][0]*w.x + vm.m[0][1]*w.y + vm.m[0][2]*w.z + vm.m[0][3]) * inv;
    float ny = (vm.m[1][0]*w.x + vm.m[1][1]*w.y + vm.m[1][2]*w.z + vm.m[1][3]) * inv;
    o.x = g_ScreenW * 0.5f + nx * g_ScreenW * 0.5f;
    o.y = g_ScreenH * 0.5f - ny * g_ScreenH * 0.5f;
    return true;
}

// ============================================================================
// Console
// ============================================================================

static FILE* g_pCon = nullptr;

static void ConInit()
{
    AllocConsole();
    SetConsoleTitleA("payload — debug console");
    freopen_s(&g_pCon, "CONOUT$", "w", stdout);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SMALL_RECT sr = { 0, 0, 79, 24 };
    SetConsoleWindowInfo(h, TRUE, &sr);
    printf("========================================\n");
    printf("  payload v2.1  (clickable menu)\n");
    printf("========================================\n");
    printf("  DEL  toggle menu\n");
    printf("  END  unload dll\n");
    printf("========================================\n\n");
}

static void ConShutdown()
{
    if (g_pCon) { fclose(g_pCon); g_pCon = nullptr; }
    FreeConsole();
}

// ============================================================================
// Fonts
// ============================================================================

static HFONT g_hFontTitle = nullptr, g_hFontBody = nullptr, g_hFontESP = nullptr;

static void CreateFonts()
{
    g_hFontTitle = CreateFontA(18,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Consolas");
    g_hFontBody  = CreateFontA(15,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Consolas");
    g_hFontESP   = CreateFontA(13,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Consolas");
}

static void DestroyFonts()
{
    if (g_hFontTitle) { DeleteObject(g_hFontTitle); g_hFontTitle = nullptr; }
    if (g_hFontBody)  { DeleteObject(g_hFontBody);  g_hFontBody  = nullptr; }
    if (g_hFontESP)   { DeleteObject(g_hFontESP);   g_hFontESP   = nullptr; }
}

// ============================================================================
// GDI draw helpers
// ============================================================================

static void DrawOutlinedRect(HDC hdc, HPEN pen, int x, int y, int w, int h)
{
    HGDIOBJ op = SelectObject(hdc, pen);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, x, y, x + w, y + h);
    SelectObject(hdc, ob); SelectObject(hdc, op);
}

static void DrawLine(HDC hdc, HPEN pen, int x1, int y1, int x2, int y2)
{
    HGDIOBJ op = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr); LineTo(hdc, x2, y2);
    SelectObject(hdc, op);
}

static void DrawTextShadowed(HDC hdc, int x, int y, const char* t, COLORREF c)
{
    SetTextColor(hdc, RGB(0,0,0)); TextOutA(hdc, x+1, y+1, t, (int)strlen(t));
    SetTextColor(hdc, c);          TextOutA(hdc, x, y, t, (int)strlen(t));
}

// ============================================================================
// Hit-test helpers
// ============================================================================

static bool PtInRc(int x, int y, const RECT& r)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Toggle WS_EX_TRANSPARENT on the overlay
static void SetClickThrough(HWND hWnd, bool through)
{
    LONG_PTR ex = GetWindowLongPtrA(hWnd, GWL_EXSTYLE);
    if (through) ex |= WS_EX_TRANSPARENT;
    else         ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrA(hWnd, GWL_EXSTYLE, ex);
}

// ============================================================================
// ESP — data + drawing  (unchanged logic, compacted)
// ============================================================================

struct EntData { Vec3 origin, mins, maxs; int health; uint8_t team; };

static bool GetEntData(uintptr_t pawn, EntData& e)
{
    if (!Mem<int>(pawn + offsets::m_iHealth, e.health) || e.health <= 0) return false;
    if (!Mem<uint8_t>(pawn + offsets::m_iTeamNum, e.team)) return false;

    uintptr_t sn = 0;
    if (!Mem<uintptr_t>(pawn + offsets::m_pGameSceneNode, sn) || !sn) return false;
    bool dormant = false; Mem<bool>(sn + offsets::m_bDormant, dormant);
    if (dormant) return false;
    if (!Mem<Vec3>(sn + offsets::m_vecAbsOrigin, e.origin)) return false;

    uintptr_t col = 0;
    if (!Mem<uintptr_t>(pawn + offsets::m_pCollision, col) || !col)
        { e.mins = {-16,-16,0}; e.maxs = {16,16,72}; }
    else {
        if (!Mem<Vec3>(col + offsets::m_vecMins, e.mins)) e.mins = {-16,-16,0};
        if (!Mem<Vec3>(col + offsets::m_vecMaxs, e.maxs)) e.maxs = {16,16,72};
    }
    return true;
}

static void DrawESP2D(HDC hdc, const ViewMatrix4x4& vm, const EntData& e, COLORREF c)
{
    Vec3 feet = e.origin, head = {e.origin.x, e.origin.y, e.origin.z + e.maxs.z};
    Vec2 sf, sh;
    if (!W2S(feet, vm, sf) || !W2S(head, vm, sh)) return;
    float bh = sf.y - sh.y; if (bh < 4.f) return;
    float bw = bh * 0.45f, cx = (sf.x + sh.x) * 0.5f;
    int l=(int)(cx-bw*.5f), t=(int)sh.y, w=(int)bw, h=(int)bh;

    HPEN pb = CreatePen(PS_SOLID, 3, RGB(0,0,0));
    DrawOutlinedRect(hdc, pb, l, t, w, h); DeleteObject(pb);
    HPEN pc = CreatePen(PS_SOLID, 1, c);
    DrawOutlinedRect(hdc, pc, l, t, w, h); DeleteObject(pc);

    SelectObject(hdc, g_hFontESP); SetBkMode(hdc, TRANSPARENT);
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.0f %.0f %.0f", e.origin.x, e.origin.y, e.origin.z);
    DrawTextShadowed(hdc, l, t - 14, buf, CLR_ESP_TEXT);
}

static void DrawESP3D(HDC hdc, const ViewMatrix4x4& vm, const EntData& e, COLORREF c)
{
    Vec3 o=e.origin, mn=e.mins, mx=e.maxs;
    Vec3 cn[8] = {
        {o.x+mn.x,o.y+mn.y,o.z+mn.z},{o.x+mx.x,o.y+mn.y,o.z+mn.z},
        {o.x+mx.x,o.y+mx.y,o.z+mn.z},{o.x+mn.x,o.y+mx.y,o.z+mn.z},
        {o.x+mn.x,o.y+mn.y,o.z+mx.z},{o.x+mx.x,o.y+mn.y,o.z+mx.z},
        {o.x+mx.x,o.y+mx.y,o.z+mx.z},{o.x+mn.x,o.y+mx.y,o.z+mx.z},
    };
    Vec2 s[8];
    for (int i=0;i<8;i++) if (!W2S(cn[i],vm,s[i])) return;

    // edge pairs: bottom(0-1,1-2,2-3,3-0) top(4-5,5-6,6-7,7-4) vert(0-4,1-5,2-6,3-7)
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};

    HPEN pb = CreatePen(PS_SOLID, 3, RGB(0,0,0));
    for (auto& ed : edges) DrawLine(hdc,pb,(int)s[ed[0]].x,(int)s[ed[0]].y,(int)s[ed[1]].x,(int)s[ed[1]].y);
    DeleteObject(pb);
    HPEN pc = CreatePen(PS_SOLID, 1, c);
    for (auto& ed : edges) DrawLine(hdc,pc,(int)s[ed[0]].x,(int)s[ed[0]].y,(int)s[ed[1]].x,(int)s[ed[1]].y);
    DeleteObject(pc);

    float minY=s[4].y, minX=s[4].x;
    for (int i=5;i<8;i++) if (s[i].y<minY){minY=s[i].y;minX=s[i].x;}

    SelectObject(hdc, g_hFontESP); SetBkMode(hdc, TRANSPARENT);
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.0f %.0f %.0f", e.origin.x, e.origin.y, e.origin.z);
    DrawTextShadowed(hdc, (int)minX, (int)minY - 14, buf, CLR_ESP_TEXT);
}

static void RenderESP(HDC hdc)
{
    ESPMode mode = static_cast<ESPMode>(g_ESPMode.load(std::memory_order_relaxed));
    if (mode == ESPMode::Off || !g_ClientBase) return;

    ViewMatrix4x4 vm;
    if (!Mem<ViewMatrix4x4>(g_ClientBase + offsets::dwViewMatrix, vm)) return;

    uintptr_t localPawn = 0; uint8_t localTeam = 0;
    if (Mem<uintptr_t>(g_ClientBase + offsets::dwLocalPlayerPawn, localPawn) && localPawn)
        Mem<uint8_t>(localPawn + offsets::m_iTeamNum, localTeam);

    uintptr_t entList = 0;
    if (!Mem<uintptr_t>(g_ClientBase + offsets::dwEntityList, entList) || !entList) return;

    for (int i = 1; i <= 63; i++)
    {
        uintptr_t chunk = 0;
        if (!Mem<uintptr_t>(entList + 0x8 * ((i >> 9) + 1), chunk) || !chunk) continue;
        uintptr_t ctrl = 0;
        if (!Mem<uintptr_t>(chunk + 0x78 * (i & 0x1FF), ctrl) || !ctrl) continue;

        bool alive = false; Mem<bool>(ctrl + offsets::m_bPawnIsAlive, alive);
        if (!alive) continue;

        uint32_t ph = 0;
        if (!Mem<uint32_t>(ctrl + offsets::m_hPlayerPawn, ph) || ph == 0xFFFFFFFF) continue;

        uintptr_t pch = 0;
        if (!Mem<uintptr_t>(entList + 0x8 * ((ph >> 9) + 1), pch) || !pch) continue;
        uintptr_t pawn = 0;
        if (!Mem<uintptr_t>(pch + 0x78 * (ph & 0x1FF), pawn) || !pawn) continue;

        if (pawn == localPawn) continue;

        EntData ent;
        if (!GetEntData(pawn, ent)) continue;

        COLORREF clr = (ent.team == localTeam) ? CLR_ESP_TEAM : CLR_ESP_ENEMY;
        if (mode == ESPMode::Box2D) DrawESP2D(hdc, vm, ent, clr);
        else                        DrawESP3D(hdc, vm, ent, clr);
    }
}

// ============================================================================
// Menu — paint a single feature row  (returns bottom Y)
// ============================================================================

static int PaintRow(HDC hdc, const RECT& menu, int y,
                    const char* label, const char* tag,
                    COLORREF tagClr, bool hover)
{
    // Hover highlight across the full row
    if (hover)
    {
        RECT rh = { menu.left + 2, MENU_Y + y, menu.right - 2, MENU_Y + y + ROW_H };
        HBRUSH bh = CreateSolidBrush(CLR_ROW_HOVER);
        FillRect(hdc, &rh, bh); DeleteObject(bh);
    }

    int absY = MENU_Y + y + 3;  // vertically centre text in row

    // Label
    SetTextColor(hdc, CLR_LABEL);
    RECT rl = { menu.left + 16, absY, menu.left + 200, absY + 18 };
    DrawTextA(hdc, label, -1, &rl, DT_LEFT | DT_SINGLELINE);

    // Pill
    bool on = (tagClr == CLR_ON);
    RECT rp = { menu.right - 78, MENU_Y + y + 1, menu.right - 16, MENU_Y + y + ROW_H - 1 };
    HBRUSH bp = CreateSolidBrush(on ? RGB(30,60,40) : RGB(60,30,30));
    FillRect(hdc, &rp, bp); DeleteObject(bp);
    HPEN pp = CreatePen(PS_SOLID, 1, tagClr);
    HGDIOBJ op = SelectObject(hdc, pp);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rp.left, rp.top, rp.right, rp.bottom);
    SelectObject(hdc, ob); SelectObject(hdc, op); DeleteObject(pp);
    SetTextColor(hdc, tagClr);
    DrawTextA(hdc, tag, -1, &rp, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    return y + ROW_H;
}

// ============================================================================
// Overlay — full paint
// ============================================================================

static void PaintOverlay(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    // Clear to transparent
    RECT rcAll; GetClientRect(hWnd, &rcAll);
    HBRUSH brT = CreateSolidBrush(CLR_TRANSPARENT);
    FillRect(hdc, &rcAll, brT); DeleteObject(brT);

    // ESP
    RenderESP(hdc);

    // Menu
    if (!g_MenuVisible.load(std::memory_order_relaxed))
    { EndPaint(hWnd, &ps); return; }

    // Cursor position for hover detection
    POINT cur; GetCursorPos(&cur);
    // Window is at (0,0) fullscreen, so screen == client coords.

    RECT rc = MENU_RECT;

    // Background
    HBRUSH brBg = CreateSolidBrush(CLR_BG);
    FillRect(hdc, &rc, brBg); DeleteObject(brBg);

    // Border
    HPEN penBd = CreatePen(PS_SOLID, 2, CLR_BORDER);
    HGDIOBJ oP = SelectObject(hdc, penBd);
    HGDIOBJ oB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oB); SelectObject(hdc, oP); DeleteObject(penBd);

    // Header
    RECT rH = { rc.left+1, rc.top+1, rc.right-1, rc.top+30 };
    HBRUSH bH = CreateSolidBrush(CLR_HEADER); FillRect(hdc, &rH, bH); DeleteObject(bH);

    // Accent stripe
    RECT rS = { rc.left+1, rc.top+30, rc.right-1, rc.top+32 };
    HBRUSH bA = CreateSolidBrush(CLR_ACCENT); FillRect(hdc, &rS, bA); DeleteObject(bA);

    SetBkMode(hdc, TRANSPARENT);

    // Title
    SelectObject(hdc, g_hFontTitle);
    SetTextColor(hdc, CLR_TITLE);
    RECT rT = { rc.left+12, rc.top+6, rc.right-10, rc.top+28 };
    DrawTextA(hdc, "PAYLOAD", -1, &rT, DT_LEFT | DT_SINGLELINE);
    SetTextColor(hdc, CLR_DIM);
    DrawTextA(hdc, "v2.1", -1, &rT, DT_RIGHT | DT_SINGLELINE);

    SelectObject(hdc, g_hFontBody);

    // Row 1: Jump
    {
        bool on    = g_JumpEnabled.load(std::memory_order_relaxed);
        bool hover = PtInRc(cur.x, cur.y, ROW1_RECT);
        PaintRow(hdc, rc, ROW1_Y,
                 "Jump", on ? "  ON " : " OFF ",
                 on ? CLR_ON : CLR_OFF, hover);
    }

    // Row 2: ESP
    {
        int  em    = g_ESPMode.load(std::memory_order_relaxed);
        bool on    = (em != 0);
        bool hover = PtInRc(cur.x, cur.y, ROW2_RECT);
        PaintRow(hdc, rc, ROW2_Y,
                 "ESP", ESPModeStr[em],
                 on ? CLR_ON : CLR_OFF, hover);
    }

    // Separator
    int sy = MENU_Y + ROW2_Y + ROW_H + 8;
    HPEN pSep = CreatePen(PS_SOLID, 1, RGB(45,45,60));
    oP = SelectObject(hdc, pSep);
    MoveToEx(hdc, rc.left+14, sy, nullptr);
    LineTo(hdc, rc.right-14, sy);
    SelectObject(hdc, oP); DeleteObject(pSep);

    // Footer
    SetTextColor(hdc, CLR_DIM);
    RECT rF = { rc.left+16, sy+8, rc.right-10, sy+24 };
    DrawTextA(hdc, "click to toggle   DEL close   END eject", -1,
              &rF, DT_LEFT | DT_SINGLELINE);

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
        PaintOverlay(hWnd);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_REDRAW)
            InvalidateRect(hWnd, nullptr, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    // ---- Interactive hit-testing ----
    // Only the menu box captures clicks; everything else passes through.
    case WM_NCHITTEST:
    {
        if (!g_MenuVisible.load(std::memory_order_relaxed))
            return HTTRANSPARENT;

        int x = (short)LOWORD(lp);
        int y = (short)HIWORD(lp);
        if (PtInRc(x, y, MENU_RECT))
            return HTCLIENT;
        return HTTRANSPARENT;
    }

    // Show arrow cursor when hovering over the menu
    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return TRUE;

    // Prevent the overlay from stealing focus when clicked
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    // ---- Click handling ----
    case WM_LBUTTONDOWN:
    {
        // lParam is client coords (== screen coords for our fullscreen window)
        int mx = (short)LOWORD(lp);
        int my = (short)HIWORD(lp);

        if (PtInRc(mx, my, ROW1_RECT))
        {
            bool val = !g_JumpEnabled.load(std::memory_order_relaxed);
            g_JumpEnabled.store(val, std::memory_order_relaxed);
            printf("[click] jump %s\n", val ? "ON" : "OFF");
        }
        else if (PtInRc(mx, my, ROW2_RECT))
        {
            int next = (g_ESPMode.load(std::memory_order_relaxed) + 1)
                       % (int)ESPMode::Count;
            g_ESPMode.store(next, std::memory_order_relaxed);
            printf("[click] esp %s\n", ESPModeStr[next]);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_REDRAW);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hWnd, msg, wp, lp);
    }
}

// ============================================================================
// Menu thread
// ============================================================================

static DWORD WINAPI MenuThreadProc(LPVOID /*p*/)
{
    ConInit();

    g_ScreenW = GetSystemMetrics(SM_CXSCREEN);
    g_ScreenH = GetSystemMetrics(SM_CYSCREEN);
    printf("[init] screen %d x %d\n", g_ScreenW, g_ScreenH);

    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance      = g_OwnModule;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(CLR_TRANSPARENT);
    wc.lpszClassName = "PayloadOverlayClass";
    RegisterClassExA(&wc);
    CreateFonts();

    // Fullscreen overlay — no WS_EX_TRANSPARENT by default;
    // WM_NCHITTEST handles click-through selectively.
    g_OverlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        "PayloadOverlayClass", nullptr, WS_POPUP,
        0, 0, g_ScreenW, g_ScreenH,
        nullptr, nullptr, g_OwnModule, nullptr);

    SetLayeredWindowAttributes(g_OverlayWnd, CLR_TRANSPARENT, 0, LWA_COLORKEY);
    ShowWindow(g_OverlayWnd, SW_SHOWNOACTIVATE);
    SetTimer(g_OverlayWnd, TIMER_REDRAW, REDRAW_MS, nullptr);

    printf("[init] overlay created\n");

    MSG msg;
    bool prevDel = false;

    while (g_Running.load(std::memory_order_relaxed))
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { g_Running.store(false); goto cleanup; }
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }

        // DELETE — toggle menu
        bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (del && !prevDel)
        {
            bool vis = !g_MenuVisible.load(std::memory_order_relaxed);
            g_MenuVisible.store(vis, std::memory_order_relaxed);
            printf("[menu] %s\n", vis ? "opened" : "closed");
        }
        prevDel = del;

        // END — unload
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
// Feature thread — jump
// ============================================================================

static DWORD WINAPI FeatureThreadProc(LPVOID /*p*/)
{
    timeBeginPeriod(1);

    while (g_Running.load(std::memory_order_relaxed))
    { g_ClientBase = GetModuleBase(L"client.dll"); if (g_ClientBase) break; Sleep(200); }
    printf("[init] client.dll @ 0x%llX\n", (unsigned long long)g_ClientBase);

    while (g_Running.load(std::memory_order_relaxed))
    {
        bool active = g_JumpEnabled.load(std::memory_order_relaxed) &&
                      (GetAsyncKeyState(VK_SPACE) & 0x8000);
        if (active) {
            uintptr_t pawn = 0;
            if (Mem<uintptr_t>(g_ClientBase + offsets::dwLocalPlayerPawn, pawn) && pawn) {
                uint32_t fl = 0;
                if (Mem<uint32_t>(pawn + offsets::m_fFlags, fl))
                    ForceButton(g_ClientBase, offsets::jump, (fl & FL_ONGROUND) != 0);
            }
        } else if (g_ClientBase) {
            ForceButton(g_ClientBase, offsets::jump, false);
        }
        Sleep(1);
    }

    if (g_ClientBase) ForceButton(g_ClientBase, offsets::jump, false);
    timeEndPeriod(1);
    return 0;
}

// ============================================================================
// DllMain
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_OwnModule = hModule;
        g_MenuThread    = CreateThread(nullptr,0,MenuThreadProc,   nullptr,0,nullptr);
        g_FeatureThread = CreateThread(nullptr,0,FeatureThreadProc,nullptr,0,nullptr);
    } else if (dwReason == DLL_PROCESS_DETACH) {
        g_Running.store(false, std::memory_order_relaxed);
        if (g_FeatureThread) { WaitForSingleObject(g_FeatureThread,2000); CloseHandle(g_FeatureThread); }
        if (g_MenuThread)    { WaitForSingleObject(g_MenuThread,   2000); CloseHandle(g_MenuThread);    }
    }
    return TRUE;
}
