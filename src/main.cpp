#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "advisor.h"
#include "hardware.h"

enum {
    ID_HWTEXT = 100, ID_CORES, ID_RAM, ID_VRAM, ID_APPLY, ID_RESET, ID_REDETECT,
    ID_LIST, ID_COMBO, ID_REC,
    ID_LBL_CORES, ID_LBL_RAM, ID_LBL_VRAM, ID_LBL_TARGET, ID_GRP_HW, ID_GRP_WHATIF
};

static HINSTANCE g_inst;
static HWND g_hwnd, g_hwText, g_list, g_combo, g_rec;
static HWND g_edCores, g_edRam, g_edVram;
static HwInfo g_hw;
static Spec g_spec;

static std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

static Spec specFromDetected() {
    Spec s;
    s.cores = g_hw.physicalCores;
    s.avx2 = g_hw.avx2;
    s.ramGB = g_hw.ramGB;
    int g = g_hw.bestGpu();
    if (g >= 0) {
        s.vramGB = g_hw.gpus[g].vramGB;
        s.gpuName = g_hw.gpus[g].name;
    }
    return s;
}

static std::string hardwareText() {
    std::string t = "CPU:  " + g_hw.cpuName + "\r\n";
    t += fmt("      %d cores / %d threads   AVX:%s  AVX2:%s  AVX-512:%s\r\n", g_hw.physicalCores,
             g_hw.logicalCores, g_hw.avx ? "yes" : "no", g_hw.avx2 ? "yes" : "no",
             g_hw.avx512 ? "yes" : "no");
    t += fmt("RAM:  %.1f GB\r\n", g_hw.ramGB);
    if (g_hw.gpus.empty()) t += "GPU:  none detected\r\n";
    for (const auto& g : g_hw.gpus)
        t += fmt("GPU:  %s   VRAM %.1f GB (shared %.1f GB)\r\n", g.name.c_str(), g.vramGB, g.sharedGB);
    return t;
}

static void setEditNum(HWND h, double v) {
    SetWindowTextA(h, fmt("%.0f", v).c_str());
}

static void fillInputs() {
    setEditNum(g_edCores, g_spec.cores);
    setEditNum(g_edRam, g_spec.ramGB);
    setEditNum(g_edVram, g_spec.vramGB);
}

static void refreshList() {
    ListView_DeleteAllItems(g_list);
    const auto& cat = modelCatalog();
    for (size_t i = 0; i < cat.size(); ++i) {
        Verdict v = evaluate(cat[i], g_spec);
        LVITEMA it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (char*)cat[i].name;
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        auto set = [&](int col, const std::string& s) {
            LVITEMA sub = {};
            sub.iSubItem = col;
            sub.pszText = (char*)s.c_str();
            SendMessageA(g_list, LVM_SETITEMTEXTA, i, (LPARAM)&sub);
        };
        set(1, fmt("%.1f GB", v.needGB));
        set(2, levelText(v.level));
        set(3, v.mode);
        set(4, v.tps > 0 ? fmt("%.1f", v.tps) : std::string("-"));
    }
}

static void refreshRecommendation() {
    int sel = (int)SendMessage(g_combo, CB_GETCURSEL, 0, 0);
    const auto& cat = modelCatalog();
    if (sel < 0 || sel >= (int)cat.size()) return;
    SetWindowTextA(g_rec, recommend(cat[sel], g_spec).c_str());
}

static void refreshAll() {
    refreshList();
    refreshRecommendation();
}

static double readNum(HWND h, double fallback) {
    char b[64];
    GetWindowTextA(h, b, sizeof b);
    char* end = nullptr;
    double v = strtod(b, &end);
    if (end == b || v < 0) return fallback;
    return v;
}

static void applyWhatIf() {
    Spec d = specFromDetected();
    g_spec.cores = (int)std::max(1.0, readNum(g_edCores, d.cores));
    g_spec.ramGB = readNum(g_edRam, d.ramGB);
    g_spec.vramGB = readNum(g_edVram, d.vramGB);
    g_spec.vramOverridden = (g_spec.vramGB != d.vramGB);
    refreshAll();
}

static void detectAndShow() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    g_hw = detectHardware();
    g_spec = specFromDetected();
    SetWindowTextA(g_hwText, hardwareText().c_str());
    fillInputs();
    refreshAll();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

static HWND mk(const char* cls, const char* text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_hwnd,
                           (HMENU)(INT_PTR)id, g_inst, nullptr);
}

static void layout(int w, int h) {
    const int m = 10;
    int y = m;
    MoveWindow(GetDlgItem(g_hwnd, ID_GRP_HW), m, y, w - 2 * m, 100, TRUE);
    MoveWindow(g_hwText, m + 8, y + 18, w - 2 * m - 16 - 110, 78, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, ID_REDETECT), w - m - 104, y + 22, 96, 26, TRUE);
    y += 108;

    MoveWindow(GetDlgItem(g_hwnd, ID_GRP_WHATIF), m, y, w - 2 * m, 52, TRUE);
    int x = m + 10, cy = y + 20;
    MoveWindow(GetDlgItem(g_hwnd, ID_LBL_CORES), x, cy + 3, 90, 18, TRUE);
    MoveWindow(g_edCores, x + 92, cy, 50, 22, TRUE);
    x += 160;
    MoveWindow(GetDlgItem(g_hwnd, ID_LBL_RAM), x, cy + 3, 60, 18, TRUE);
    MoveWindow(g_edRam, x + 62, cy, 55, 22, TRUE);
    x += 135;
    MoveWindow(GetDlgItem(g_hwnd, ID_LBL_VRAM), x, cy + 3, 70, 18, TRUE);
    MoveWindow(g_edVram, x + 72, cy, 55, 22, TRUE);
    x += 150;
    MoveWindow(GetDlgItem(g_hwnd, ID_APPLY), x, cy - 1, 70, 24, TRUE);
    MoveWindow(GetDlgItem(g_hwnd, ID_RESET), x + 78, cy - 1, 130, 24, TRUE);
    y += 60;

    int recH = 170;
    int listH = h - y - recH - 40 - m;
    if (listH < 80) listH = 80;
    MoveWindow(g_list, m, y, w - 2 * m, listH, TRUE);
    y += listH + 8;

    MoveWindow(GetDlgItem(g_hwnd, ID_LBL_TARGET), m, y + 3, 200, 18, TRUE);
    MoveWindow(g_combo, m + 205, y, 300, 300, TRUE);
    y += 28;
    MoveWindow(g_rec, m, y, w - 2 * m, h - y - m, TRUE);
}

static void createControls() {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    mk("BUTTON", " Detected hardware ", BS_GROUPBOX, ID_GRP_HW);
    g_hwText = mk("STATIC", "", 0, ID_HWTEXT);
    mk("BUTTON", "Re-detect", BS_PUSHBUTTON | WS_TABSTOP, ID_REDETECT);

    mk("BUTTON", " What-if hardware (edit to test upgrades) ", BS_GROUPBOX, ID_GRP_WHATIF);
    mk("STATIC", "CPU cores:", 0, ID_LBL_CORES);
    g_edCores = mk("EDIT", "", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, ID_CORES, WS_EX_CLIENTEDGE);
    mk("STATIC", "RAM (GB):", 0, ID_LBL_RAM);
    g_edRam = mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_RAM, WS_EX_CLIENTEDGE);
    mk("STATIC", "VRAM (GB):", 0, ID_LBL_VRAM);
    g_edVram = mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_VRAM, WS_EX_CLIENTEDGE);
    mk("BUTTON", "Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_APPLY);
    mk("BUTTON", "Reset to detected", BS_PUSHBUTTON | WS_TABSTOP, ID_RESET);

    g_list = mk(WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, ID_LIST,
                WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const char* cols[] = {"AI engine / model", "Memory needed", "Result", "Mode", "Est. tokens/s"};
    const int widths[] = {230, 110, 100, 90, 100};
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNA c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (char*)cols[i];
        c.cx = widths[i];
        SendMessageA(g_list, LVM_INSERTCOLUMNA, i, (LPARAM)&c);
    }

    mk("STATIC", "Recommend hardware for (optional):", 0, ID_LBL_TARGET);
    g_combo = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_COMBO);
    for (const auto& m : modelCatalog()) SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)m.name);
    SendMessage(g_combo, CB_SETCURSEL, 10, 0);  // a "superior" model by default

    g_rec = mk("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_REC,
               WS_EX_CLIENTEDGE);

    EnumChildWindows(
        g_hwnd, [](HWND c, LPARAM f) -> BOOL { SendMessage(c, WM_SETFONT, f, TRUE); return TRUE; },
        (LPARAM)font);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            createControls();
            detectAndShow();
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) layout(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mi = (MINMAXINFO*)lp;
            mi->ptMinTrackSize.x = 720;
            mi->ptMinTrackSize.y = 620;
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_APPLY: applyWhatIf(); break;
                case ID_RESET:
                    g_spec = specFromDetected();
                    fillInputs();
                    refreshAll();
                    break;
                case ID_REDETECT: detectAndShow(); break;
                case ID_COMBO:
                    if (HIWORD(wp) == CBN_SELCHANGE) refreshRecommendation();
                    break;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    g_inst = inst;
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "HardwareAIWnd";
    RegisterClassA(&wc);

    HWND w = CreateWindowA(wc.lpszClassName, "Hardware-AI - what AI can my PC run?",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 700, nullptr,
                           nullptr, inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessage(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
