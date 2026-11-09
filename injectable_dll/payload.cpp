// payload.cpp — Educational DLL Injection Payload
// Demonstrates: DllMain lifecycle, Win32 overlay, in-process memory read,
//               entity iteration, world-to-screen projection, 2D/3D box ESP,
//               command-button forcing, and clean self-unload.
//
// Hotkeys:
//   DELETE  — toggle menu overlay
//   INSERT  — toggle jump feature     (menu must be open)
//   HOME    — cycle ESP mode: OFF → 2D → 3D → OFF  (menu must be open)
//   END     — unload DLL
//
// Build: x64 Release DLL.  All names are placeholders.

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
    // client.dll globals
    constexpr std::ptrdiff_t dwLocalPlayerPawn       = 0x256A948;
    constexpr std::ptrdiff_t dwLocalPlayerController = 0x2542548;
    constexpr std::ptrdiff_t dwEntityList            = 0x271FBB8;
    constexpr std::ptrdiff_t dwViewMatrix            = 0x2570250;

    // C_BaseEntity
    constexpr std::ptrdiff_t m_iHealth         = 0x34C;   // int32
    constexpr std::ptrdiff_t m_pGameSceneNode  = 0x330;   // CGameSceneNode*
    constexpr std::ptrdiff_t m_pCollision      = 0x340;   // CCollisionProperty*
    constexpr std::ptrdiff_t m_iTeamNum        = 0x3E7;   // uint8
    constexpr std::ptrdiff_t m_fFlags          = 0x3F4;   // uint32

    // CGameSceneNode
    constexpr std::ptrdiff_t m_vecAbsOrigin    = 0xC8;    // Vector (VectorWS)
    constexpr std::ptrdiff_t m_bDormant        = 0x103;   // bool

    // CCollisionProperty
    constexpr std::ptrdiff_t m_vecMins         = 0x40;    // Vector
    constexpr std::ptrdiff_t m_vecMaxs         = 0x4C;    // Vector

    // CBasePlayerController
    constexpr std::ptrdiff_t m_hPawn           = 0x6BC;   // CHandle
    constexpr std::ptrdiff_t m_iszPlayerName   = 0x6FC;   // char[128]

    // CCSPlayerController
    constexpr std::ptrdiff_t m_hPlayerPawn     = 0x92C;   // CHandle
    constexpr std::ptrdiff_t m_bPawnIsAlive    = 0x934;   // bool

    // buttons (client.dll)
    constexpr std::ptrdiff_t jump              = 0x2239460;
}

constexpr uint32_t FL_ONGROUND = (1 << 0);

// ============================================================================
// Math types
// ============================================================================

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };

struct ViewMatrix4x4 {
    float m[4][4];
};

// ============================================================================
// ESP modes
// ============================================================================

enum class ESPMode : int { Off = 0, Box2D = 1, Box3D = 2, Count = 3 };
static const char* ESPModeStr[] = { "OFF", " 2D", " 3D" };

// ============================================================================
// Menu layout / style
// ============================================================================

constexpr int   MENU_X       = 40;
constexpr int   MENU_Y       = 40;
constexpr int   MENU_W       = 290;
constexpr int   MENU_H       = 170;
constexpr int   TIMER_REDRAW = 1;
constexpr int   REDRAW_MS    = 16;               // ~60 fps repaint

constexpr COLORREF CLR_TRANSPARENT = RGB(255, 0, 255);  // color-key (invisible)
constexpr COLORREF CLR_BG          = RGB(18, 18, 26);
constexpr COLORREF CLR_BORDER      = RGB(75, 60, 130);
constexpr COLORREF CLR_ACCENT      = RGB(130, 90, 220);
constexpr COLORREF CLR_TITLE       = RGB(190, 160, 255);
constexpr COLORREF CLR_LABEL       = RGB(190, 190, 200);
constexpr COLORREF CLR_ON          = RGB(80, 220, 120);
constexpr COLORREF CLR_OFF         = RGB(220, 75, 75);
constexpr COLORREF CLR_DIM         = RGB(100, 100, 115);
constexpr COLORREF CLR_HEADER      = RGB(24, 24, 34);

// ESP colours
constexpr COLORREF CLR_ESP_ENEMY   = RGB(220, 50, 50);
constexpr COLORREF CLR_ESP_TEAM    = RGB(50, 140, 220);
constexpr COLORREF CLR_ESP_TEXT    = RGB(240, 240, 240);

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
static std::atomic<int>   g_ESPMode       { 0     };  // ESPMode cast to int

static uintptr_t          g_ClientBase    = 0;
static int                g_ScreenW       = 0;
static int                g_ScreenH       = 0;

// ============================================================================
// Helpers — memory
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
// Helpers — world-to-screen projection
// ============================================================================

static bool WorldToScreen(const Vec3& world, const ViewMatrix4x4& vm, Vec2& out)
{
    // Clip-space W
    float w = vm.m[3][0] * world.x + vm.m[3][1] * world.y +
              vm.m[3][2] * world.z + vm.m[3][3];

    if (w < 0.001f) return false; // behind camera

    float invW = 1.0f / w;

    float nx = (vm.m[0][0] * world.x + vm.m[0][1] * world.y +
                vm.m[0][2] * world.z + vm.m[0][3]) * invW;
    float ny = (vm.m[1][0] * world.x + vm.m[1][1] * world.y +
                vm.m[1][2] * world.z + vm.m[1][3]) * invW;

    // NDC → screen
    out.x = (g_ScreenW * 0.5f) + (nx * g_ScreenW * 0.5f);
    out.y = (g_ScreenH * 0.5f) - (ny * g_ScreenH * 0.5f);

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

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SMALL_RECT sr = { 0, 0, 79, 24 };
    SetConsoleWindowInfo(hOut, TRUE, &sr);

    printf("========================================\n");
    printf("  payload — educational demo  v2.0\n");
    printf("========================================\n");
    printf("  DEL   toggle menu\n");
    printf("  INS   toggle jump  (menu open)\n");
    printf("  HOME  cycle ESP    (menu open)\n");
    printf("  END   unload dll\n");
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

static HFONT g_hFontTitle   = nullptr;
static HFONT g_hFontBody    = nullptr;
static HFONT g_hFontESP     = nullptr;

static void CreateFonts()
{
    g_hFontTitle = CreateFontA(18, 0, 0, 0, FW_BOLD, 0,0,0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
    g_hFontBody = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0,0,0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
    g_hFontESP = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0,0,0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
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
    HGDIOBJ old = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, x, y, x + w, y + h);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, old);
}

static void DrawLine(HDC hdc, HPEN pen, int x1, int y1, int x2, int y2)
{
    HGDIOBJ old = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    SelectObject(hdc, old);
}

static void DrawTextOutlined(HDC hdc, int x, int y, const char* text, COLORREF clr)
{
    // Shadow
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, x + 1, y + 1, text, (int)strlen(text));
    // Main
    SetTextColor(hdc, clr);
    TextOutA(hdc, x, y, text, (int)strlen(text));
}

// ============================================================================
// ESP — entity iteration + drawing
// ============================================================================

struct EntityData {
    Vec3     origin;
    Vec3     mins, maxs;
    int      health;
    uint8_t  team;
    bool     valid;
};

static bool GetEntityData(uintptr_t pawn, EntityData& ent)
{
    ent.valid = false;

    // Health
    if (!Mem<int>(pawn + offsets::m_iHealth, ent.health) || ent.health <= 0)
        return false;

    // Team
    if (!Mem<uint8_t>(pawn + offsets::m_iTeamNum, ent.team))
        return false;

    // Scene node → position + dormant check
    uintptr_t sceneNode = 0;
    if (!Mem<uintptr_t>(pawn + offsets::m_pGameSceneNode, sceneNode) || !sceneNode)
        return false;

    bool dormant = false;
    Mem<bool>(sceneNode + offsets::m_bDormant, dormant);
    if (dormant) return false;

    if (!Mem<Vec3>(sceneNode + offsets::m_vecAbsOrigin, ent.origin))
        return false;

    // Collision → bounding box
    uintptr_t collision = 0;
    if (!Mem<uintptr_t>(pawn + offsets::m_pCollision, collision) || !collision)
    {
        // Fallback: use standard player bounds
        ent.mins = { -16.f, -16.f, 0.f };
        ent.maxs = {  16.f,  16.f, 72.f };
    }
    else
    {
        if (!Mem<Vec3>(collision + offsets::m_vecMins, ent.mins)) ent.mins = { -16,-16, 0 };
        if (!Mem<Vec3>(collision + offsets::m_vecMaxs, ent.maxs)) ent.maxs = {  16, 16, 72 };
    }

    ent.valid = true;
    return true;
}

static void DrawESP2D(HDC hdc, const ViewMatrix4x4& vm, const EntityData& ent, COLORREF clr)
{
    // Project feet + head to get a 2D bounding rect
    Vec3 feet = ent.origin;
    Vec3 head = { ent.origin.x, ent.origin.y, ent.origin.z + ent.maxs.z };

    Vec2 sFeet, sHead;
    if (!WorldToScreen(feet, vm, sFeet)) return;
    if (!WorldToScreen(head, vm, sHead)) return;

    float boxH = sFeet.y - sHead.y;
    if (boxH < 4.f) return; // too small / too far

    float boxW = boxH * 0.45f; // width proportional to height
    float cx   = (sFeet.x + sHead.x) * 0.5f;

    int left   = (int)(cx - boxW * 0.5f);
    int top    = (int)(sHead.y);
    int right  = (int)(cx + boxW * 0.5f);
    int bottom = (int)(sFeet.y);
    int w      = right - left;
    int h      = bottom - top;

    // Outline (black behind, coloured on top)
    HPEN penBlack = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
    DrawOutlinedRect(hdc, penBlack, left, top, w, h);
    DeleteObject(penBlack);

    HPEN pen = CreatePen(PS_SOLID, 1, clr);
    DrawOutlinedRect(hdc, pen, left, top, w, h);
    DeleteObject(pen);

    // Position text at top-left corner of box
    SelectObject(hdc, g_hFontESP);
    SetBkMode(hdc, TRANSPARENT);
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.0f %.0f %.0f",
                ent.origin.x, ent.origin.y, ent.origin.z);
    DrawTextOutlined(hdc, left, top - 14, buf, CLR_ESP_TEXT);
}

static void DrawESP3D(HDC hdc, const ViewMatrix4x4& vm, const EntityData& ent, COLORREF clr)
{
    // Build 8 corners of the AABB in world space
    Vec3 o = ent.origin;
    Vec3 mn = ent.mins;
    Vec3 mx = ent.maxs;

    Vec3 corners[8] = {
        { o.x + mn.x, o.y + mn.y, o.z + mn.z },  // 0: bottom near-left
        { o.x + mx.x, o.y + mn.y, o.z + mn.z },  // 1: bottom near-right
        { o.x + mx.x, o.y + mx.y, o.z + mn.z },  // 2: bottom far-right
        { o.x + mn.x, o.y + mx.y, o.z + mn.z },  // 3: bottom far-left
        { o.x + mn.x, o.y + mn.y, o.z + mx.z },  // 4: top near-left
        { o.x + mx.x, o.y + mn.y, o.z + mx.z },  // 5: top near-right
        { o.x + mx.x, o.y + mx.y, o.z + mx.z },  // 6: top far-right
        { o.x + mn.x, o.y + mx.y, o.z + mx.z },  // 7: top far-left
    };

    Vec2 sc[8];
    for (int i = 0; i < 8; i++)
    {
        if (!WorldToScreen(corners[i], vm, sc[i]))
            return; // any corner behind camera → skip
    }

    // Draw 12 edges of the box
    // Black outline pass
    HPEN penBlack = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
    // Bottom face
    DrawLine(hdc, penBlack, (int)sc[0].x,(int)sc[0].y, (int)sc[1].x,(int)sc[1].y);
    DrawLine(hdc, penBlack, (int)sc[1].x,(int)sc[1].y, (int)sc[2].x,(int)sc[2].y);
    DrawLine(hdc, penBlack, (int)sc[2].x,(int)sc[2].y, (int)sc[3].x,(int)sc[3].y);
    DrawLine(hdc, penBlack, (int)sc[3].x,(int)sc[3].y, (int)sc[0].x,(int)sc[0].y);
    // Top face
    DrawLine(hdc, penBlack, (int)sc[4].x,(int)sc[4].y, (int)sc[5].x,(int)sc[5].y);
    DrawLine(hdc, penBlack, (int)sc[5].x,(int)sc[5].y, (int)sc[6].x,(int)sc[6].y);
    DrawLine(hdc, penBlack, (int)sc[6].x,(int)sc[6].y, (int)sc[7].x,(int)sc[7].y);
    DrawLine(hdc, penBlack, (int)sc[7].x,(int)sc[7].y, (int)sc[4].x,(int)sc[4].y);
    // Verticals
    DrawLine(hdc, penBlack, (int)sc[0].x,(int)sc[0].y, (int)sc[4].x,(int)sc[4].y);
    DrawLine(hdc, penBlack, (int)sc[1].x,(int)sc[1].y, (int)sc[5].x,(int)sc[5].y);
    DrawLine(hdc, penBlack, (int)sc[2].x,(int)sc[2].y, (int)sc[6].x,(int)sc[6].y);
    DrawLine(hdc, penBlack, (int)sc[3].x,(int)sc[3].y, (int)sc[7].x,(int)sc[7].y);
    DeleteObject(penBlack);

    // Colored pass
    HPEN pen = CreatePen(PS_SOLID, 1, clr);
    DrawLine(hdc, pen, (int)sc[0].x,(int)sc[0].y, (int)sc[1].x,(int)sc[1].y);
    DrawLine(hdc, pen, (int)sc[1].x,(int)sc[1].y, (int)sc[2].x,(int)sc[2].y);
    DrawLine(hdc, pen, (int)sc[2].x,(int)sc[2].y, (int)sc[3].x,(int)sc[3].y);
    DrawLine(hdc, pen, (int)sc[3].x,(int)sc[3].y, (int)sc[0].x,(int)sc[0].y);
    DrawLine(hdc, pen, (int)sc[4].x,(int)sc[4].y, (int)sc[5].x,(int)sc[5].y);
    DrawLine(hdc, pen, (int)sc[5].x,(int)sc[5].y, (int)sc[6].x,(int)sc[6].y);
    DrawLine(hdc, pen, (int)sc[6].x,(int)sc[6].y, (int)sc[7].x,(int)sc[7].y);
    DrawLine(hdc, pen, (int)sc[7].x,(int)sc[7].y, (int)sc[4].x,(int)sc[4].y);
    DrawLine(hdc, pen, (int)sc[0].x,(int)sc[0].y, (int)sc[4].x,(int)sc[4].y);
    DrawLine(hdc, pen, (int)sc[1].x,(int)sc[1].y, (int)sc[5].x,(int)sc[5].y);
    DrawLine(hdc, pen, (int)sc[2].x,(int)sc[2].y, (int)sc[6].x,(int)sc[6].y);
    DrawLine(hdc, pen, (int)sc[3].x,(int)sc[3].y, (int)sc[7].x,(int)sc[7].y);
    DeleteObject(pen);

    // Position text at top corner (highest screen point among top-face corners)
    float minY = sc[4].y;
    float minX = sc[4].x;
    for (int i = 5; i < 8; i++) {
        if (sc[i].y < minY) { minY = sc[i].y; minX = sc[i].x; }
    }

    SelectObject(hdc, g_hFontESP);
    SetBkMode(hdc, TRANSPARENT);
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.0f %.0f %.0f",
                ent.origin.x, ent.origin.y, ent.origin.z);
    DrawTextOutlined(hdc, (int)minX, (int)minY - 14, buf, CLR_ESP_TEXT);
}

static void RenderESP(HDC hdc)
{
    ESPMode mode = static_cast<ESPMode>(g_ESPMode.load(std::memory_order_relaxed));
    if (mode == ESPMode::Off || !g_ClientBase) return;

    // Read view matrix
    ViewMatrix4x4 vm;
    if (!Mem<ViewMatrix4x4>(g_ClientBase + offsets::dwViewMatrix, vm))
        return;

    // Read local player team for friend/foe colouring
    uintptr_t localPawn = 0;
    uint8_t localTeam = 0;
    if (Mem<uintptr_t>(g_ClientBase + offsets::dwLocalPlayerPawn, localPawn) && localPawn)
        Mem<uint8_t>(localPawn + offsets::m_iTeamNum, localTeam);

    // Read entity list base
    uintptr_t entityList = 0;
    if (!Mem<uintptr_t>(g_ClientBase + offsets::dwEntityList, entityList) || !entityList)
        return;

    // Iterate controllers (indices 1..63 for players)
    for (int i = 1; i <= 63; i++)
    {
        // Entity list is a chunked array.
        // Chunk = entityList + 0x8 * (i >> 9) + 0x10 → deref → base of 512-entry block
        // Entry = chunkBase + 0x78 * (i & 0x1FF)
        uintptr_t chunkPtr = 0;
        if (!Mem<uintptr_t>(entityList + 0x8 * ((i >> 9) + 1), chunkPtr) || !chunkPtr)
            continue;

        uintptr_t controller = 0;
        if (!Mem<uintptr_t>(chunkPtr + 0x78 * (i & 0x1FF), controller) || !controller)
            continue;

        // Check if pawn is alive
        bool alive = false;
        Mem<bool>(controller + offsets::m_bPawnIsAlive, alive);
        if (!alive) continue;

        // Get pawn handle → resolve to pawn pointer
        uint32_t pawnHandle = 0;
        if (!Mem<uint32_t>(controller + offsets::m_hPlayerPawn, pawnHandle))
            continue;
        if (pawnHandle == 0xFFFFFFFF) continue;

        // Resolve handle: chunk index = handle >> 9, entry = handle & 0x1FF
        uintptr_t pawnChunkPtr = 0;
        if (!Mem<uintptr_t>(entityList + 0x8 * ((pawnHandle >> 9) + 1), pawnChunkPtr) || !pawnChunkPtr)
            continue;

        uintptr_t pawn = 0;
        if (!Mem<uintptr_t>(pawnChunkPtr + 0x78 * (pawnHandle & 0x1FF), pawn) || !pawn)
            continue;

        // Skip local player
        if (pawn == localPawn) continue;

        // Read entity data
        EntityData ent;
        if (!GetEntityData(pawn, ent)) continue;

        // Team colour (2=T, 3=CT typically)
        COLORREF clr = (ent.team == localTeam) ? CLR_ESP_TEAM : CLR_ESP_ENEMY;

        if (mode == ESPMode::Box2D)
            DrawESP2D(hdc, vm, ent, clr);
        else
            DrawESP3D(hdc, vm, ent, clr);
    }
}

// ============================================================================
// Overlay — paint
// ============================================================================

static void PaintOverlay(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);

    // ---- Clear entire window to the color-key (transparent) ----
    RECT rcClient;
    GetClientRect(hWnd, &rcClient);
    HBRUSH brTransparent = CreateSolidBrush(CLR_TRANSPARENT);
    FillRect(hdc, &rcClient, brTransparent);
    DeleteObject(brTransparent);

    // ---- Draw ESP (always, even if menu is hidden) ----
    RenderESP(hdc);

    // ---- Draw menu (only if visible) ----
    if (!g_MenuVisible.load(std::memory_order_relaxed))
    {
        EndPaint(hWnd, &ps);
        return;
    }

    RECT rc = { MENU_X, MENU_Y, MENU_X + MENU_W, MENU_Y + MENU_H };

    // background
    HBRUSH brBg = CreateSolidBrush(CLR_BG);
    FillRect(hdc, &rc, brBg);
    DeleteObject(brBg);

    // border
    HPEN penBorder = CreatePen(PS_SOLID, 2, CLR_BORDER);
    HGDIOBJ oldPen = SelectObject(hdc, penBorder);
    HGDIOBJ oldBr  = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(penBorder);

    // header bar
    RECT rcHeader = { rc.left + 1, rc.top + 1, rc.right - 1, rc.top + 30 };
    HBRUSH brHeader = CreateSolidBrush(CLR_HEADER);
    FillRect(hdc, &rcHeader, brHeader);
    DeleteObject(brHeader);

    // accent stripe
    RECT rcStripe = { rc.left + 1, rc.top + 30, rc.right - 1, rc.top + 32 };
    HBRUSH brAccent = CreateSolidBrush(CLR_ACCENT);
    FillRect(hdc, &rcStripe, brAccent);
    DeleteObject(brAccent);

    SetBkMode(hdc, TRANSPARENT);

    // title
    SelectObject(hdc, g_hFontTitle);
    SetTextColor(hdc, CLR_TITLE);
    RECT rcTitle = { rc.left + 12, rc.top + 6, rc.right - 10, rc.top + 28 };
    DrawTextA(hdc, "PAYLOAD", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
    SetTextColor(hdc, CLR_DIM);
    DrawTextA(hdc, "v2.0", -1, &rcTitle, DT_RIGHT | DT_SINGLELINE);

    SelectObject(hdc, g_hFontBody);

    // ---- Row 1: Jump ----
    int y = rc.top + 44;
    {
        SetTextColor(hdc, CLR_LABEL);
        RECT rl = { rc.left + 16, y, rc.left + 180, y + 18 };
        DrawTextA(hdc, "[INS] Jump", -1, &rl, DT_LEFT | DT_SINGLELINE);

        bool on = g_JumpEnabled.load(std::memory_order_relaxed);
        COLORREF tc = on ? CLR_ON : CLR_OFF;
        const char* ts = on ? "  ON " : " OFF ";
        RECT rp = { rc.left + 218, y - 2, rc.right - 16, y + 18 };
        HBRUSH bp = CreateSolidBrush(on ? RGB(30, 60, 40) : RGB(60, 30, 30));
        FillRect(hdc, &rp, bp); DeleteObject(bp);
        HPEN pp = CreatePen(PS_SOLID, 1, tc);
        oldPen = SelectObject(hdc, pp); oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rp.left, rp.top, rp.right, rp.bottom);
        SelectObject(hdc, oldBr); SelectObject(hdc, oldPen); DeleteObject(pp);
        SetTextColor(hdc, tc);
        DrawTextA(hdc, ts, -1, &rp, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // ---- Row 2: ESP ----
    y += 28;
    {
        SetTextColor(hdc, CLR_LABEL);
        RECT rl = { rc.left + 16, y, rc.left + 200, y + 18 };
        DrawTextA(hdc, "[HOME] ESP", -1, &rl, DT_LEFT | DT_SINGLELINE);

        int em = g_ESPMode.load(std::memory_order_relaxed);
        bool on = (em != 0);
        COLORREF tc = on ? CLR_ON : CLR_OFF;
        const char* ts = ESPModeStr[em];
        RECT rp = { rc.left + 218, y - 2, rc.right - 16, y + 18 };
        HBRUSH bp = CreateSolidBrush(on ? RGB(30, 60, 40) : RGB(60, 30, 30));
        FillRect(hdc, &rp, bp); DeleteObject(bp);
        HPEN pp = CreatePen(PS_SOLID, 1, tc);
        oldPen = SelectObject(hdc, pp); oldBr = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rp.left, rp.top, rp.right, rp.bottom);
        SelectObject(hdc, oldBr); SelectObject(hdc, oldPen); DeleteObject(pp);
        SetTextColor(hdc, tc);
        DrawTextA(hdc, ts, -1, &rp, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }

    // separator
    y += 28;
    HPEN penSep = CreatePen(PS_SOLID, 1, RGB(45, 45, 60));
    oldPen = SelectObject(hdc, penSep);
    MoveToEx(hdc, rc.left + 14, y, nullptr);
    LineTo(hdc, rc.right - 14, y);
    SelectObject(hdc, oldPen);
    DeleteObject(penSep);

    // footer
    y += 10;
    SetTextColor(hdc, CLR_DIM);
    RECT rcFoot = { rc.left + 16, y, rc.right - 10, y + 16 };
    DrawTextA(hdc, "DEL menu  END unload", -1, &rcFoot, DT_LEFT | DT_SINGLELINE);

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

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_REDRAW);
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcA(hWnd, msg, wp, lp);
    }
}

// ============================================================================
// Menu thread — fullscreen overlay window + input polling
// ============================================================================

static DWORD WINAPI MenuThreadProc(LPVOID /*p*/)
{
    ConInit();

    // Screen dimensions
    g_ScreenW = GetSystemMetrics(SM_CXSCREEN);
    g_ScreenH = GetSystemMetrics(SM_CYSCREEN);
    printf("[init] screen %d x %d\n", g_ScreenW, g_ScreenH);

    // Register class — use the color-key as the background brush
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

    // Create fullscreen overlay (always visible for ESP, menu drawn on top)
    g_OverlayWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        "PayloadOverlayClass", nullptr, WS_POPUP,
        0, 0, g_ScreenW, g_ScreenH,
        nullptr, nullptr, g_OwnModule, nullptr);

    // Color-key: magenta pixels become fully transparent
    SetLayeredWindowAttributes(g_OverlayWnd, CLR_TRANSPARENT, 0, LWA_COLORKEY);

    ShowWindow(g_OverlayWnd, SW_SHOWNOACTIVATE);
    SetTimer(g_OverlayWnd, TIMER_REDRAW, REDRAW_MS, nullptr);

    printf("[init] overlay window created (fullscreen, color-keyed)\n");

    // Message pump + hotkey polling
    MSG msg;
    bool prevDel  = false;
    bool prevIns  = false;
    bool prevHome = false;

    while (g_Running.load(std::memory_order_relaxed))
    {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { g_Running.store(false); goto cleanup; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // DELETE — toggle menu
        bool del = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (del && !prevDel) {
            bool vis = !g_MenuVisible.load(std::memory_order_relaxed);
            g_MenuVisible.store(vis, std::memory_order_relaxed);
            printf("[menu] %s\n", vis ? "opened" : "closed");
        }
        prevDel = del;

        // INSERT — toggle jump (menu open)
        bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (ins && !prevIns && g_MenuVisible.load(std::memory_order_relaxed)) {
            bool val = !g_JumpEnabled.load(std::memory_order_relaxed);
            g_JumpEnabled.store(val, std::memory_order_relaxed);
            printf("[feature] jump %s\n", val ? "ON" : "OFF");
        }
        prevIns = ins;

        // HOME — cycle ESP mode (menu open)
        bool home = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (home && !prevHome && g_MenuVisible.load(std::memory_order_relaxed)) {
            int next = (g_ESPMode.load(std::memory_order_relaxed) + 1) % (int)ESPMode::Count;
            g_ESPMode.store(next, std::memory_order_relaxed);
            printf("[feature] esp %s\n", ESPModeStr[next]);
        }
        prevHome = home;

        // END — unload
        if (GetAsyncKeyState(VK_END) & 1) {
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
                    ForceButton(g_ClientBase, offsets::jump,
                                (flags & FL_ONGROUND) != 0);
                }
            }
        }
        else
        {
            if (g_ClientBase)
                ForceButton(g_ClientBase, offsets::jump, false);
        }

        Sleep(1);
    }

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
        g_MenuThread    = CreateThread(nullptr, 0, MenuThreadProc,    nullptr, 0, nullptr);
        g_FeatureThread = CreateThread(nullptr, 0, FeatureThreadProc, nullptr, 0, nullptr);
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_Running.store(false, std::memory_order_relaxed);
        if (g_FeatureThread) { WaitForSingleObject(g_FeatureThread, 2000); CloseHandle(g_FeatureThread); }
        if (g_MenuThread)    { WaitForSingleObject(g_MenuThread,    2000); CloseHandle(g_MenuThread);    }
    }

    return TRUE;
}
