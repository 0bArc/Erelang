#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdlib>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <windowsx.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")
#endif
#include <thread>
#include <mutex>
#include <atomic>
#include <commctrl.h>

// Simple dark-themed GUI build bootstrap for erelang/obc.
// Shows scrolling log and status. Double-click background to rebuild.

namespace fs = std::filesystem;

static std::wstring g_log;
static std::mutex g_logMutex;
static std::atomic<bool> g_building{false};
static HWND g_edit = nullptr;
static HWND g_btnBuild = nullptr;
static HWND g_btnRebuild = nullptr;
static HWND g_btnClean = nullptr;
static HWND g_btnAbout = nullptr;
static HWND g_chkObs = nullptr;
static HWND g_chkObc = nullptr;
static HWND g_status = nullptr;
static HWND g_side = nullptr; // side panel
static float g_scale = 1.0f;
static HFONT g_font = nullptr;        // UI font
static HFONT g_fontMono = nullptr;    // Log font
static HFONT g_fontIcon = nullptr;    // icon/glyph font (if needed)
static HWND g_aboutWnd = nullptr;

// Colors
// Theme (focused on subtle contrast so it looks cleaner at smaller sizes)
static COLORREF C_BG_TOP = RGB(27,30,36);
static COLORREF C_BG_BOTTOM = RGB(20,22,26);
static COLORREF C_PANEL = RGB(36,40,46);
static COLORREF C_PANEL_BORDER = RGB(55,60,68);
static COLORREF C_TEXT = RGB(232,236,241);
static COLORREF C_TEXT_MUTED = RGB(160,168,178);
static COLORREF C_ACCENT_BORDER = RGB(65,120,230);
static COLORREF C_BTN_BG = RGB(44,48,54);
static COLORREF C_BTN_BG_HOT = RGB(58,64,72);
static COLORREF C_BTN_BG_DOWN = RGB(34,38,44);
static COLORREF C_EDIT_BG = RGB(22,24,28);
static COLORREF C_OK = RGB(0,195,90);
static COLORREF C_WARN = RGB(220,185,0);
static COLORREF C_ERR = RGB(210,60,60);
static const wchar_t* APP_TITLE = L"erelang Builder";

static void append_log(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_edit) return;
    std::wstring ws(s.begin(), s.end());
    SendMessageW(g_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)ws.c_str());
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
}

static int run_cmd_capture(const std::string& cmd) {
#ifdef _WIN32
    std::string full = "cmd /c \"" + cmd + "\"";
#else
    std::string full = cmd;
#endif
    append_log("$ " + full + "\n");
    FILE* pipe = _popen(full.c_str(), "r");
    if (!pipe) { append_log("Failed to spawn command\n"); return 1; }
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) append_log(buf);
    int code = _pclose(pipe);
    append_log("(exit " + std::to_string(code) + ")\n\n");
    return code;
}

static void ensure_configured(const fs::path& root, const fs::path& buildDir, bool force) {
    if (force || !fs::exists(buildDir / "CMakeCache.txt")) {
        fs::create_directories(buildDir);
        run_cmd_capture("cmake -S \"" + root.string() + "\" -B \"" + buildDir.string() + "\"");
    }
}

static void set_status(const std::wstring& s) { if (g_status) SetWindowTextW(g_status, s.c_str()); if (g_side) InvalidateRect(g_side,nullptr,FALSE); }

enum class BuildMode { Build, Rebuild, Clean };

static void do_build(BuildMode mode) {
    if (g_building.exchange(true)) return;
    switch(mode) {
        case BuildMode::Build: set_status(L"Building..."); break;
        case BuildMode::Rebuild: set_status(L"Rebuilding..."); break;
        case BuildMode::Clean: set_status(L"Cleaning..."); break;
    }
    bool buildObs = (SendMessageW(g_chkObs, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool buildObc = (SendMessageW(g_chkObc, BM_GETCHECK, 0, 0) == BST_CHECKED);
    std::wstring targetsW;
    if (mode != BuildMode::Clean) {
        if (buildObs) targetsW += L" --target erelang";
        if (buildObc) targetsW += L" --target obc";
        if (targetsW.empty()) targetsW = L" --target erelang --target obc";
    }
    std::string targets(targetsW.begin(), targetsW.end());
    std::thread([mode, targets]{
        fs::path exe = fs::absolute(fs::path(L"build.exe"));
#ifdef _WIN32
        wchar_t mod[MAX_PATH]; GetModuleFileNameW(nullptr, mod, MAX_PATH); exe = fs::path(mod);
#endif
        fs::path buildDir = exe.parent_path();
        fs::path root = buildDir.parent_path();
        if (mode == BuildMode::Build) append_log("=== Build started ===\n");
        else if (mode == BuildMode::Rebuild) append_log("=== Rebuild started (clean + build) ===\n");
        else append_log("=== Clean started ===\n");
        ensure_configured(root, buildDir, mode == BuildMode::Rebuild);
        int rc = 0;
        if (mode == BuildMode::Clean || mode == BuildMode::Rebuild) {
            rc = run_cmd_capture("cmake --build \"" + buildDir.string() + "\" --target clean");
            if (mode == BuildMode::Clean) {
                append_log(rc==0?"Clean succeeded.\n":"Clean failed.\n");
                g_building = false; set_status(rc==0?L"Idle (cleaned)":L"Idle (clean failed)"); return;
            }
            if (rc!=0) {
                append_log("Clean failed; aborting rebuild.\n");
                g_building = false; set_status(L"Idle (clean failed)"); return; }
        }
        rc = run_cmd_capture("cmake --build \"" + buildDir.string() + "\"" + targets + " -j 4");
        append_log(rc == 0 ? "Build succeeded.\n" : "Build failed.\n");
        g_building = false;
        set_status(rc==0?L"Idle (success)":L"Idle (failed)");
    }).detach();
}

#ifdef _WIN32
static void enable_dark(HWND hwnd) {
    BOOL useDark = TRUE; DwmSetWindowAttribute(hwnd, 20, &useDark, sizeof(useDark));
    // Try immersive dark (some Windows builds)
    DwmSetWindowAttribute(hwnd, 19, &useDark, sizeof(useDark));
}

static void apply_font(HWND w, HFONT f) { if (f) SendMessageW(w, WM_SETFONT, (WPARAM)f, TRUE); }

static int dpi(HWND hwnd) { HDC dc = GetDC(hwnd); int d = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(hwnd, dc); return d; }

struct BtnState { HWND hwnd; bool hot=false; bool down=false; int id=0; };
static BtnState g_btnStates[4];

static LRESULT CALLBACK SideProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps); RECT r; GetClientRect(hwnd,&r);
        HBRUSH br=CreateSolidBrush(C_PANEL); FillRect(hdc,&r,br); DeleteObject(br);
        // left border
        HPEN p=CreatePen(PS_SOLID,1,C_PANEL_BORDER); HGDIOBJ op=SelectObject(hdc,p); MoveToEx(hdc,0,0,nullptr); LineTo(hdc,0,r.bottom); SelectObject(hdc,op); DeleteObject(p);
        SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,C_TEXT);
        if (g_font) SelectObject(hdc,g_font);
        int pad=(int)(10*g_scale);
        RECT hdr{pad,pad,r.right-pad,pad+18}; DrawTextW(hdc,L"Targets", -1,&hdr,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX|DT_VCENTER);
        // status indicator
        COLORREF dot=C_OK;
        if (g_building.load()) dot=C_WARN; else {
            if (g_status) { wchar_t buf[64]; GetWindowTextW(g_status,buf,64); if (wcsstr(buf,L"failed")||wcsstr(buf,L"error")) dot=C_ERR; }
        }
        int yBase = hdr.bottom + (int)(14*g_scale);
        RECT circle{pad,yBase,pad+14,yBase+14};
        HBRUSH bDot=CreateSolidBrush(dot); HBRUSH ob=(HBRUSH)SelectObject(hdc,bDot); HPEN pen=CreatePen(PS_SOLID,1,dot); HPEN open=(HPEN)SelectObject(hdc,pen);
        Ellipse(hdc,circle.left,circle.top,circle.right,circle.bottom);
        SelectObject(hdc,ob); DeleteObject(bDot); SelectObject(hdc,open); DeleteObject(pen);
        RECT sl{circle.right + (int)(6*g_scale), circle.top-1, r.right - pad, circle.bottom+1}; DrawTextW(hdc,L"Status", -1,&sl,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        EndPaint(hwnd,&ps); return 0; }
    }
    return DefWindowProcW(hwnd,msg,wParam,lParam);
}

static void update_button_hot(HWND hwnd, POINT pt) {
    for (auto &b : g_btnStates) if (b.hwnd==hwnd) {
        RECT r; GetWindowRect(hwnd, &r); ScreenToClient(GetParent(hwnd), (LPPOINT)&r.left); ScreenToClient(GetParent(hwnd), (LPPOINT)&r.right);
        bool inside = PtInRect(&r, pt)!=0; if (inside!=b.hot) { b.hot = inside; InvalidateRect(hwnd, nullptr, TRUE);} return; }
}

static void gradient_fill_vertical(HDC hdc, RECT r, COLORREF top, COLORREF bottom) {
    TRIVERTEX vert[2];
    GRADIENT_RECT gRect{0,1};
    vert[0].x = r.left; vert[0].y = r.top;
    vert[0].Red = GetRValue(top) << 8; vert[0].Green = GetGValue(top) << 8; vert[0].Blue = GetBValue(top) << 8; vert[0].Alpha = 0xff00;
    vert[1].x = r.right; vert[1].y = r.bottom;
    vert[1].Red = GetRValue(bottom) << 8; vert[1].Green = GetGValue(bottom) << 8; vert[1].Blue = GetBValue(bottom) << 8; vert[1].Alpha = 0xff00;
    GradientFill(hdc, vert, 2, &gRect, 1, GRADIENT_FILL_RECT_V);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        enable_dark(hwnd);
        int d = dpi(hwnd); g_scale = d / 96.0f;
        // Fonts (try modern faces, fallback gracefully)
    LOGFONTW lf{}; lf.lfHeight = (LONG)(- (int)(13 * g_scale)); lf.lfQuality = CLEARTYPE_NATURAL_QUALITY; wcscpy_s(lf.lfFaceName, L"Segoe UI"); g_font = CreateFontIndirectW(&lf);
    LOGFONTW lfIcon{}; lfIcon.lfHeight = (LONG)(- (int)(15 * g_scale)); wcscpy_s(lfIcon.lfFaceName,L"Segoe UI Symbol"); g_fontIcon = CreateFontIndirectW(&lfIcon);
        LOGFONTW lfMono{}; lfMono.lfHeight = (LONG)(- (int)(12 * g_scale)); lfMono.lfQuality = CLEARTYPE_QUALITY; wcscpy_s(lfMono.lfFaceName, L"Cascadia Mono"); g_fontMono = CreateFontIndirectW(&lfMono);
        if (!g_fontMono) { wcscpy_s(lfMono.lfFaceName, L"Consolas"); g_fontMono = CreateFontIndirectW(&lfMono);}        
        int pad = (int)(10 * g_scale);
        int btnW = (int)(80 * g_scale); int btnH = (int)(30 * g_scale);
        DWORD btnStyle = WS_CHILD|WS_VISIBLE|BS_OWNERDRAW;
    g_btnBuild   = CreateWindowW(L"BUTTON", L"▶ Build",   btnStyle, pad, pad, btnW, btnH, hwnd, (HMENU)1, nullptr, nullptr);
    g_btnRebuild = CreateWindowW(L"BUTTON", L"⟳ Rebuild", btnStyle, pad + (int)(85*g_scale), pad, btnW, btnH, hwnd, (HMENU)6, nullptr, nullptr);
    g_btnClean   = CreateWindowW(L"BUTTON", L"✖ Clean",   btnStyle, pad + (int)(170*g_scale), pad, btnW, btnH, hwnd, (HMENU)7, nullptr, nullptr);
    g_btnAbout   = CreateWindowW(L"BUTTON", L"ℹ About",   btnStyle, pad + (int)(255*g_scale), pad, (int)(80*g_scale), btnH, hwnd, (HMENU)8, nullptr, nullptr);
        g_chkObs = CreateWindowW(L"BUTTON", L"erelang", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, pad + (int)(330*g_scale), pad + (int)(6*g_scale), (int)(50*g_scale), (int)(22*g_scale), hwnd, (HMENU)3, nullptr, nullptr);
        g_chkObc = CreateWindowW(L"BUTTON", L"obc", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX, pad + (int)(385*g_scale), pad + (int)(6*g_scale), (int)(50*g_scale), (int)(22*g_scale), hwnd, (HMENU)4, nullptr, nullptr);
        SendMessageW(g_chkObs, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_chkObc, BM_SETCHECK, BST_CHECKED, 0);
    g_status = CreateWindowW(L"STATIC", L"Idle", WS_CHILD|WS_VISIBLE|SS_LEFT, 0,0,0,0, hwnd, (HMENU)5, nullptr, nullptr);
        g_edit = CreateWindowW(L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY, pad, pad + btnH + (int)(12*g_scale), 100, 100, hwnd, (HMENU)2, nullptr, nullptr);
        apply_font(g_btnBuild, g_font); apply_font(g_btnRebuild, g_font); apply_font(g_btnClean, g_font); apply_font(g_btnAbout, g_font);
        apply_font(g_chkObs, g_font); apply_font(g_chkObc, g_font); apply_font(g_status, g_font); apply_font(g_edit, g_fontMono);
        g_btnStates[0] = { g_btnBuild, false, false, 1};
        g_btnStates[1] = { g_btnRebuild, false, false, 6};
        g_btnStates[2] = { g_btnClean, false, false, 7};
        g_btnStates[3] = { g_btnAbout, false, false, 8};
    // Side panel class
    WNDCLASSW sc{}; sc.lpszClassName=L"ERELANG_SIDE"; sc.hInstance=GetModuleHandleW(nullptr); sc.lpfnWndProc=SideProc; sc.hCursor=LoadCursor(nullptr,IDC_ARROW); RegisterClassW(&sc);
    g_side = CreateWindowW(sc.lpszClassName,L"",WS_CHILD|WS_VISIBLE,0,0,0,0,hwnd,nullptr,sc.hInstance,nullptr);
    break; }
    case WM_SIZE: {
        if (g_edit) {
            RECT rc; GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left; int h = rc.bottom - rc.top;
            int pad = (int)(10 * g_scale); int topH = (int)(30 * g_scale) + (int)(20 * g_scale);
            int sideW = (int)(180 * g_scale);
            int gap = (int)(10 * g_scale);
            int logW = w - pad*2 - sideW - gap; if (logW < 120) logW = 120;
            int logY = pad + (int)(30 * g_scale) + (int)(12 * g_scale);
            MoveWindow(g_edit, pad, logY, logW, h - topH, TRUE);
            MoveWindow(g_side, pad + logW + gap, logY, sideW, h - topH, TRUE);
            // place checkboxes + status label inside the side area logically (absolute for now)
            int innerPad = pad + logW + gap + (int)(14 * g_scale);
            MoveWindow(g_chkObs, innerPad, logY + (int)(10 * g_scale), (int)(60*g_scale), (int)(22*g_scale), TRUE);
            MoveWindow(g_chkObc, innerPad, logY + (int)(40 * g_scale), (int)(60*g_scale), (int)(22*g_scale), TRUE);
            MoveWindow(g_status, innerPad + (int)(4 * g_scale), logY + (int)(80 * g_scale), (int)(110 * g_scale), (int)(20 * g_scale), TRUE);
            InvalidateRect(g_side,nullptr,FALSE);
        }
        break; }
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = (LONG)(560 * g_scale);
        mmi->ptMinTrackSize.y = (LONG)(360 * g_scale);
        return 0; }
    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        for (auto &b : g_btnStates) if (b.hwnd) update_button_hot(b.hwnd, pt);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0}; TrackMouseEvent(&tme);
        return 0; }
    case WM_MOUSELEAVE: {
        for (auto &b : g_btnStates) if (b.hwnd && b.hot) { b.hot=false; InvalidateRect(b.hwnd, nullptr, TRUE);} return 0; }
    case WM_COMMAND:
        switch(LOWORD(wParam)) {
            case 1: do_build(BuildMode::Build); break;
            case 6: do_build(BuildMode::Rebuild); break;
            case 7: do_build(BuildMode::Clean); break;
            case 8: {
                if (!g_aboutWnd) {
                    WNDCLASSW awc{}; awc.lpszClassName = L"ERELANG_ABOUT"; awc.hInstance = GetModuleHandleW(nullptr); awc.lpfnWndProc = [](HWND w, UINT m, WPARAM wp, LPARAM lp)->LRESULT {
                        switch(m){
                            case WM_LBUTTONDOWN: DestroyWindow(w); return 0;
                            case WM_PAINT: {
                                PAINTSTRUCT ps; HDC hdc=BeginPaint(w,&ps); RECT r; GetClientRect(w,&r);
                                gradient_fill_vertical(hdc, r, C_BG_TOP, C_BG_BOTTOM);
                                HBRUSH br = CreateSolidBrush(C_ACCENT_BORDER); FrameRect(hdc,&r,br); DeleteObject(br);
                                SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, C_TEXT);
                                std::wstring txt = L"erelang Builder\nVersion 0.2\nClick to close";
                                DrawTextW(hdc, txt.c_str(), (int)txt.size(), &r, DT_CENTER|DT_VCENTER|DT_NOPREFIX|DT_WORDBREAK);
                                EndPaint(w,&ps); return 0; }
                        }
                        return DefWindowProcW(w,m,wp,lp);
                    }; RegisterClassW(&awc);
                    g_aboutWnd = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW, awc.lpszClassName, L"About", WS_POPUP, 0,0,280,140, hwnd, nullptr, awc.hInstance, nullptr);
                    apply_font(g_aboutWnd, g_font);
                }
                RECT pr; GetWindowRect(hwnd, &pr); int cx = pr.left + ( (pr.right-pr.left) - 280)/2; int cy = pr.top + 90;
                SetWindowPos(g_aboutWnd, HWND_TOPMOST, cx, cy, 280, 140, SWP_SHOWWINDOW|SWP_NOACTIVATE);
                break; }
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam; SetTextColor(hdc, C_TEXT); SetBkMode(hdc, TRANSPARENT); return (LRESULT)CreateSolidBrush(C_BG_BOTTOM); }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam; SetTextColor(hdc, RGB(210,215,220)); SetBkColor(hdc, C_EDIT_BG); return (LRESULT)CreateSolidBrush(C_EDIT_BG); }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps); RECT r; GetClientRect(hwnd,&r);
        HDC mem = CreateCompatibleDC(hdc); HBITMAP bmp = CreateCompatibleBitmap(hdc, r.right, r.bottom); HGDIOBJ oldBmp = SelectObject(mem, bmp);
        gradient_fill_vertical(mem, r, C_BG_TOP, C_BG_BOTTOM);
        RECT topBar = r; topBar.bottom = (int)( (30 * g_scale) + (20 * g_scale) );
        HBRUSH brPanel = CreateSolidBrush(C_PANEL); FillRect(mem, &topBar, brPanel); DeleteObject(brPanel);
        RECT border = topBar; border.bottom -= 1; // bottom line
        HPEN pen = CreatePen(PS_SOLID,1,C_PANEL_BORDER); HGDIOBJ oldPen = SelectObject(mem, pen); MoveToEx(mem, topBar.left, topBar.bottom-1, nullptr); LineTo(mem, topBar.right, topBar.bottom-1); SelectObject(mem, oldPen); DeleteObject(pen);
        BitBlt(hdc,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
        SelectObject(mem, oldBmp); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd,&ps); return 0; }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT ds = (LPDRAWITEMSTRUCT)lParam; if (!ds) break;
        wchar_t text[64]; GetWindowTextW(ds->hwndItem, text, 64); RECT r = ds->rcItem; HDC hdc = ds->hDC;
        BtnState* bs=nullptr; for(auto &b: g_btnStates) if (b.hwnd==ds->hwndItem) { bs=&b; break; }
        bool down = (ds->itemState & ODS_SELECTED)!=0; if (bs) bs->down = down;
        bool hot = bs?bs->hot:false;
        COLORREF fill = down?C_BTN_BG_DOWN:(hot?C_BTN_BG_HOT:C_BTN_BG);
        HBRUSH br = CreateSolidBrush(fill);
        HBRUSH brAccent = CreateSolidBrush(C_ACCENT_BORDER);
        HPEN pen = CreatePen(PS_SOLID,1, hot?C_ACCENT_BORDER:RGB(50,55,62));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBr = SelectObject(hdc, br);
        RoundRect(hdc, r.left, r.top, r.right, r.bottom, 8,8);
        SelectObject(hdc, oldBr); DeleteObject(br);
        if (down||hot) {
            RECT glow = r; InflateRect(&glow,-2,-2);
            FrameRect(hdc,&glow,brAccent);
        }
        DeleteObject(brAccent);
        SelectObject(hdc, oldPen); DeleteObject(pen);
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, C_TEXT);
        DrawTextW(hdc, text, (int)wcslen(text), &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        return TRUE; }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // Lightweight backend mode for future WinUI 3 front-end
    for (int i=1;i<argc;i++) {
        std::string a = argv[i];
        if (a == "--json") {
            // Very small proof-of-concept: just emit a build status skeleton and exit.
            // Future implementation: parse further subcommands (build / clean / rebuild + targets) and stream JSON lines.
            printf("{\n  \"version\": \"0.1\",\n  \"status\": \"ok\",\n  \"message\": \"JSON mode placeholder\"\n}\n");
            return 0;
        }
    }
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpszClassName = L"ERELANG_BUILD_WIN";
    wc.hInstance = hInst;
    wc.lpfnWndProc = WndProc;
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE+1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 820, 620, nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
#else
    printf("This GUI builder is Windows-only.\n");
    return 0;
#endif
}
