// ============================================================
// Overlay/overlay_viewer.cpp
// Standalone Win32 GUI overlay window — MSI Afterburner style.
// Reads from shared memory and renders a dark, always-on-top
// stats panel with GDI+ bar graphs. No injection needed.
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../Shared/SharedMemory.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")

using namespace Gdiplus;

// ----------------------------------------------------------------
// Layout constants
// ----------------------------------------------------------------
static const int WIN_W      = 230;
static const int WIN_H      = 200;
static const int MARGIN     = 10;
static const int ROW_H      = 22;
static const int BAR_H      = 5;
static const int LABEL_W    = 52;
static const int VAL_W      = 50;
static const int BAR_W      = WIN_W - MARGIN*2 - LABEL_W - VAL_W - 6;

// ----------------------------------------------------------------
// Colors (ARGB)
// ----------------------------------------------------------------
static const Color COL_BG      (220, 10,  12,  16 );
static const Color COL_BORDER  (180, 255, 120,  0  );
static const Color COL_TITLE   (255, 255, 120,  0  );
static const Color COL_LABEL   (255, 140, 140, 140 );
static const Color COL_WHITE   (255, 240, 240, 240 );
static const Color COL_FPS     (255, 255, 140,   0 );
static const Color COL_CPU     (255,   0, 210, 160 );
static const Color COL_RAM     (255,  74, 158, 255 );
static const Color COL_GPU     (255, 180,  80, 255 );
static const Color COL_WARN    (255, 255, 200,   0 );
static const Color COL_HOT     (255, 255,  70,  70 );
static const Color COL_BARBG   (40,  255, 255, 255 );

// ----------------------------------------------------------------
// Globals
// ----------------------------------------------------------------
static SharedMemHandle g_shm;
static HWND            g_hwnd      = nullptr;
static ULONG_PTR       g_gdipToken = 0;
static bool            g_dragging  = false;
static POINT           g_dragOff   = {};

// ----------------------------------------------------------------
// DrawStatRow — renders one label + bar + value row
// ----------------------------------------------------------------
static void DrawStatRow(Graphics& g, FontFamily& ff,
    int x, int y,
    const wchar_t* label,
    float value,          // 0-100 for bar
    const wchar_t* valStr,
    Color barColor)
{
    // Label
    Font fLabel(&ff, 8.5f, FontStyleRegular, UnitPoint);
    SolidBrush brLabel(COL_LABEL);
    g.DrawString(label, -1, &fLabel, PointF((float)x, (float)y+2), &brLabel);

    // Bar background
    int bx = x + LABEL_W;
    SolidBrush brBarBg(COL_BARBG);
    g.FillRectangle(&brBarBg, bx, y+8, BAR_W, BAR_H);

    // Bar fill
    float pct = max(0.f, min(100.f, value));
    int fillW = (int)(BAR_W * pct / 100.f);
    if (fillW > 0) {
        SolidBrush brBar(barColor);
        g.FillRectangle(&brBar, bx, y+8, fillW, BAR_H);
    }

    // Value text
    Font fVal(&ff, 9.5f, FontStyleBold, UnitPoint);
    Color vc = (value >= 90.f) ? COL_HOT : (value >= 70.f) ? COL_WARN : COL_WHITE;
    SolidBrush brVal(vc);
    RectF vRect((float)(bx + BAR_W + 4), (float)y,
                (float)VAL_W, (float)ROW_H);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentFar);
    g.DrawString(valStr, -1, &fVal, vRect, &sf, &brVal);
}

// ----------------------------------------------------------------
// PaintOverlay — full repaint on WM_PAINT / timer
// ----------------------------------------------------------------
static void PaintOverlay(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = ::BeginPaint(hwnd, &ps);

    // Double-buffer
    HDC memDC = ::CreateCompatibleDC(hdc);
    HBITMAP memBmp = ::CreateCompatibleBitmap(hdc, WIN_W, WIN_H);
    ::SelectObject(memDC, memBmp);

    Graphics g(memDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background
    SolidBrush brBg(COL_BG);
    g.FillRectangle(&brBg, 0, 0, WIN_W, WIN_H);

    // Border
    Pen pen(COL_BORDER, 1.f);
    g.DrawRectangle(&pen, 0, 0, WIN_W-1, WIN_H-1);

    FontFamily ff(L"Consolas");

    // Title bar
    SolidBrush brTitleBg(Color(60, 255, 120, 0));
    g.FillRectangle(&brTitleBg, 1, 1, WIN_W-2, 18);

    Font fTitle(&ff, 9.f, FontStyleBold, UnitPoint);
    SolidBrush brTitle(COL_TITLE);

    // Read shared stats
    RtssStats stats{};
    bool connected = false;
    if (g_shm.Valid()) {
        const RtssStats* s = g_shm.Data();
        if (s && s->version == RTSS_SHARED_VERSION) {
            memcpy(&stats, s, sizeof(stats));
            connected = true;
        }
    } else {
        g_shm.OpenMapping(); // retry
    }

    // Title
    wchar_t title[64];
    if (connected)
        swprintf_s(title, L"  RTSS Clone  [%hs]", stats.apiName[0] ? stats.apiName : "...");
    else
        swprintf_s(title, L"  RTSS Clone  [waiting...]");
    g.DrawString(title, -1, &fTitle, PointF(0.f, 2.f), &brTitle);

    int y = 24; // start below title bar
    int x = MARGIN;

    if (connected) {
        // FPS row (bigger text)
        wchar_t buf[32];
        Font fFps(&ff, 13.f, FontStyleBold, UnitPoint);
        SolidBrush brFps(COL_FPS);
        swprintf_s(buf, L"FPS: %.0f", stats.fps);
        g.DrawString(buf, -1, &fFps, PointF((float)x, (float)y), &brFps);

        Font fFt(&ff, 8.f, FontStyleRegular, UnitPoint);
        SolidBrush brFt(COL_LABEL);
        swprintf_s(buf, L"%.2f ms", stats.frameTimeMs);
        g.DrawString(buf, -1, &fFt,
            PointF((float)(WIN_W - MARGIN - 52), (float)(y + 4)), &brFt);

        y += 26;

        // Divider
        Pen divPen(Color(40, 255,255,255), 1.f);
        g.DrawLine(&divPen, x, y, WIN_W-x, y);
        y += 6;

        // CPU
        swprintf_s(buf, L"%.0f%%", stats.cpuUsagePercent);
        DrawStatRow(g, ff, x, y, L"CPU", stats.cpuUsagePercent, buf, COL_CPU);
        y += ROW_H;

        // RAM
        float ramPct = stats.ramTotalMB > 0.f
            ? (stats.ramUsedMB / stats.ramTotalMB * 100.f) : 0.f;
        swprintf_s(buf, L"%.1fG", stats.ramUsedMB / 1024.f);
        DrawStatRow(g, ff, x, y, L"RAM", ramPct, buf, COL_RAM);
        y += ROW_H;

        // Divider
        g.DrawLine(&divPen, x, y, WIN_W-x, y);
        y += 6;

        // GPU load
        if (stats.gpuUsagePercent >= 0.f) {
            swprintf_s(buf, L"%.0f%%", stats.gpuUsagePercent);
            DrawStatRow(g, ff, x, y, L"GPU", stats.gpuUsagePercent, buf, COL_GPU);
            y += ROW_H;

            // GPU mem (shared for iGPU)
            float memPct = stats.ramTotalMB > 0.f
                ? (stats.gpuMemUsedMB / stats.ramTotalMB * 100.f) : 0.f;
            swprintf_s(buf, L"%.0fM", stats.gpuMemUsedMB);
            DrawStatRow(g, ff, x, y, L"VRAM", memPct, buf, COL_GPU);
            y += ROW_H;

            // GPU temp
            if (stats.gpuTempC >= 0.f) {
                float tPct = stats.gpuTempC;
                swprintf_s(buf, L"%.0f\u00B0C", stats.gpuTempC);
                DrawStatRow(g, ff, x, y, L"Temp", tPct, buf, COL_HOT);
            } else {
                Font fSm(&ff, 8.f, FontStyleRegular, UnitPoint);
                SolidBrush brSm(COL_LABEL);
                g.DrawString(L"Temp  N/A (run OpenHardwareMonitor)",
                    -1, &fSm, PointF((float)x, (float)y+4), &brSm);
            }
        } else {
            Font fSm(&ff, 8.f, FontStyleRegular, UnitPoint);
            SolidBrush brSm(COL_LABEL);
            g.DrawString(L"GPU  not detected", -1, &fSm,
                PointF((float)x, (float)(y+4)), &brSm);
        }
    } else {
        // Not connected message
        Font fMsg(&ff, 8.5f, FontStyleRegular, UnitPoint);
        SolidBrush brMsg(COL_LABEL);
        g.DrawString(L"Waiting for HookDLL...\n\nRun Injector.exe and\nselect your game.",
            -1, &fMsg, RectF((float)x,(float)y,(float)(WIN_W-2*MARGIN),140.f),
            nullptr, &brMsg);
    }

    // Blit
    ::BitBlt(hdc, 0, 0, WIN_W, WIN_H, memDC, 0, 0, SRCCOPY);
    ::DeleteObject(memBmp);
    ::DeleteDC(memDC);
    ::EndPaint(hwnd, &ps);
}

// ----------------------------------------------------------------
// WndProc
// ----------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        PaintOverlay(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1; // handled in WM_PAINT

    case WM_TIMER:
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    // Drag the window by clicking anywhere on it
    case WM_LBUTTONDOWN:
        g_dragging = true;
        g_dragOff  = { LOWORD(lp), HIWORD(lp) };
        ::SetCapture(hwnd);
        return 0;

    case WM_MOUSEMOVE:
        if (g_dragging) {
            RECT r; ::GetWindowRect(hwnd, &r);
            ::SetWindowPos(hwnd, nullptr,
                r.left + (short)LOWORD(lp) - g_dragOff.x,
                r.top  + (short)HIWORD(lp) - g_dragOff.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        return 0;

    case WM_LBUTTONUP:
        g_dragging = false;
        ::ReleaseCapture();
        return 0;

    // Right-click to exit
    case WM_RBUTTONUP:
        ::PostQuitMessage(0);
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------
// WinMain
// ----------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // Init GDI+
    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdipToken, &gsi, nullptr);

    // Try to open shared memory (non-blocking — overlay still shows)
    g_shm.OpenMapping();

    // Register window class
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"RTSSCloneOverlay";
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);

    // Create layered, always-on-top, tool window (no taskbar entry)
    g_hwnd = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"RTSS Clone Overlay",
        WS_POPUP,
        20, 20, WIN_W, WIN_H,
        nullptr, nullptr, hInst, nullptr);

    // Semi-transparent window (255 = fully opaque; lower = more transparent)
    ::SetLayeredWindowAttributes(g_hwnd, 0, 230, LWA_ALPHA);

    ::ShowWindow(g_hwnd, SW_SHOW);
    ::UpdateWindow(g_hwnd);

    // Refresh timer — 10 Hz is plenty for a stats overlay
    ::SetTimer(g_hwnd, 1, 100, nullptr);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdipToken);
    return 0;
}
