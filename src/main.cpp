#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "advisor.h"
#include "hardware.h"

enum {
    ID_HWTEXT = 100, ID_REDETECT, ID_EXPORT,
    ID_CORES, ID_RAM, ID_RAMBW, ID_VRAM, ID_GPUPRESET, ID_APPLY, ID_RESET,
    ID_QUANT, ID_CTX, ID_CATF, ID_HIDE,
    ID_LIST, ID_SUMMARY, ID_REC,
    ID_GRP_HW, ID_GRP_WHATIF, ID_GRP_OPT,
    ID_L1, ID_L2, ID_L3, ID_L4, ID_L5, ID_L6, ID_L7, ID_L8
};

static HINSTANCE g_inst;
static HWND g_hwnd, g_hwText, g_list, g_rec, g_summary, g_hide;
static HWND g_edCores, g_edRam, g_edRamBw, g_edVram;
static HWND g_cbPreset, g_cbQuant, g_cbCtx, g_cbCat;
static HwInfo g_hw;
static Spec g_spec;
static std::vector<std::string> g_cats;
static std::vector<int> g_view;        // catalog indices currently shown
static std::vector<Verdict> g_verd;    // verdict per catalog index
static int g_sel = -1;                 // selected catalog index
static int g_sortCol = -1;
static bool g_sortAsc = true;
static bool g_updating = false;

static std::string fmt(const char* f, ...) {
    char buf[1024];
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
    s.ramBw = g_hw.ramBwGBs;
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
    t += fmt("RAM:  %.1f GB", g_hw.ramGB);
    if (g_hw.ramBwGBs > 0)
        t += fmt("   %d modules @ %d MT/s  (~%.0f GB/s)", g_hw.ramModules, g_hw.ramSpeedMTs,
                 g_hw.ramBwGBs);
    t += "\r\n";
    if (g_hw.gpus.empty()) t += "GPU:  none detected\r\n";
    for (const auto& g : g_hw.gpus)
        t += fmt("GPU:  %s   VRAM %.1f GB (shared %.1f GB)\r\n", g.name.c_str(), g.vramGB, g.sharedGB);
    return t;
}

static std::string paramsText(const ModelInfo& m) {
    if (m.paramsB <= 0) return "-";
    if (m.paramsB < 1) return fmt("%.2g B", m.paramsB);
    return fmt("%.4g B", m.paramsB);
}

static void setEditNum(HWND h, double v) {
    SetWindowTextA(h, v > 0 ? fmt("%.0f", v).c_str() : "");
}

static void fillInputs() {
    setEditNum(g_edCores, g_spec.cores);
    setEditNum(g_edRam, g_spec.ramGB);
    setEditNum(g_edRamBw, g_spec.ramBw);
    SetWindowTextA(g_edVram, fmt("%.0f", g_spec.vramGB).c_str());
    SendMessage(g_cbPreset, CB_SETCURSEL, 0, 0);
}

static void syncOptions() {
    int q = (int)SendMessage(g_cbQuant, CB_GETCURSEL, 0, 0);
    int c = (int)SendMessage(g_cbCtx, CB_GETCURSEL, 0, 0);
    g_spec.quant = std::max(0, q);
    g_spec.ctx = contextSizes()[std::max(0, c)];
}

static COLORREF levelColor(Level l) {
    switch (l) {
        case LV_SMOOTH: return RGB(198, 239, 206);
        case LV_USABLE: return RGB(255, 242, 204);
        case LV_SLOW:   return RGB(252, 228, 214);
        default:        return RGB(242, 200, 200);
    }
}

static void sortView() {
    if (g_sortCol < 0) return;
    const auto& cat = modelCatalog();
    std::stable_sort(g_view.begin(), g_view.end(), [&](int a, int b) {
        double x = 0, y = 0;
        switch (g_sortCol) {
            case 0: {
                int c = _stricmp(cat[a].name, cat[b].name);
                return g_sortAsc ? c < 0 : c > 0;
            }
            case 1: x = a; y = b; break;
            case 2: x = cat[a].paramsB; y = cat[b].paramsB; break;
            case 3: x = g_verd[a].needGB; y = g_verd[b].needGB; break;
            case 4: x = g_verd[a].level; y = g_verd[b].level; break;
            case 5: {
                int c = g_verd[a].mode.compare(g_verd[b].mode);
                return g_sortAsc ? c < 0 : c > 0;
            }
            default: x = g_verd[a].tps; y = g_verd[b].tps; break;
        }
        return g_sortAsc ? x < y : x > y;
    });
}

static void refreshRecommendation() {
    const auto& cat = modelCatalog();
    if (g_sel < 0 || g_sel >= (int)cat.size())
        SetWindowTextA(g_rec, "Click a model in the list to see the minimum hardware needed to run it smoothly.");
    else
        SetWindowTextA(g_rec, recommend(cat[g_sel], g_spec).c_str());
}

static void refreshList() {
    const auto& cat = modelCatalog();
    g_verd.assign(cat.size(), Verdict());
    int cnt[4] = {0, 0, 0, 0};
    int best = -1;
    for (size_t i = 0; i < cat.size(); ++i) {
        g_verd[i] = evaluate(cat[i], g_spec);
        cnt[g_verd[i].level]++;
        if (cat[i].kind == K_LLM && g_verd[i].level == LV_SMOOTH &&
            (best < 0 || cat[i].paramsB > cat[best].paramsB))
            best = (int)i;
    }

    int cf = (int)SendMessage(g_cbCat, CB_GETCURSEL, 0, 0);
    bool hide = SendMessage(g_hide, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_view.clear();
    for (size_t i = 0; i < cat.size(); ++i) {
        if (cf > 0 && g_cats[cf - 1] != cat[i].category) continue;
        if (hide && g_verd[i].level == LV_NO) continue;
        g_view.push_back((int)i);
    }
    sortView();

    g_updating = true;
    SendMessage(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (size_t r = 0; r < g_view.size(); ++r) {
        int i = g_view[r];
        const Verdict& v = g_verd[i];
        LVITEMA it = {};
        it.mask = LVIF_TEXT;
        it.iItem = (int)r;
        it.pszText = (char*)cat[i].name;
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        auto set = [&](int col, const std::string& s) {
            LVITEMA sub = {};
            sub.iSubItem = col;
            sub.pszText = (char*)s.c_str();
            SendMessageA(g_list, LVM_SETITEMTEXTA, r, (LPARAM)&sub);
        };
        set(1, cat[i].category);
        set(2, paramsText(cat[i]));
        set(3, fmt("%.1f GB", v.needGB));
        set(4, levelText(v.level));
        set(5, v.mode);
        set(6, v.tps > 0 ? fmt("%.1f", v.tps) : std::string("-"));
        if (i == g_sel) {
            ListView_SetItemState(g_list, (int)r, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, (int)r, FALSE);
        }
    }
    SendMessage(g_list, WM_SETREDRAW, TRUE, 0);
    g_updating = false;

    std::string sum = fmt("Showing %d of %d.   Smooth: %d   Usable: %d   Slow: %d   Cannot run: %d",
                          (int)g_view.size(), (int)cat.size(), cnt[LV_SMOOTH], cnt[LV_USABLE],
                          cnt[LV_SLOW], cnt[LV_NO]);
    if (best >= 0)
        sum += fmt("   |   Best smooth LLM: %s (%.1f tok/s)", cat[best].name, g_verd[best].tps);
    SetWindowTextA(g_summary, sum.c_str());
}

static void refreshAll() {
    syncOptions();
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
    g_spec.ramBw = readNum(g_edRamBw, 0);
    g_spec.vramGB = readNum(g_edVram, d.vramGB);

    int p = (int)SendMessage(g_cbPreset, CB_GETCURSEL, 0, 0);
    if (p == 1) {
        g_spec.gpuName.clear();
        g_spec.vramOverridden = g_spec.vramGB > 0;
    } else if (p >= 2) {
        const GpuPreset& g = gpuPresets()[p - 2];
        g_spec.gpuName = g.name;
        g_spec.vramOverridden = g_spec.vramGB != g.vramGB;
    } else {
        g_spec.gpuName = d.gpuName;
        g_spec.vramOverridden = g_spec.vramGB != d.vramGB;
    }
    refreshAll();
}

static void onPresetChanged() {
    int p = (int)SendMessage(g_cbPreset, CB_GETCURSEL, 0, 0);
    if (p == 0) SetWindowTextA(g_edVram, fmt("%.0f", specFromDetected().vramGB).c_str());
    else if (p == 1) SetWindowTextA(g_edVram, "0");
    else SetWindowTextA(g_edVram, fmt("%.0f", gpuPresets()[p - 2].vramGB).c_str());
    applyWhatIf();
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

static void exportReport() {
    char path[MAX_PATH] = "Hardware-AI-report.txt";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Text report (*.txt)\0*.txt\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&ofn)) return;

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        MessageBoxA(g_hwnd, "Cannot write the file.", "Hardware-AI", MB_ICONERROR);
        return;
    }
    const auto& cat = modelCatalog();
    f << "Hardware-AI report\r\n==================\r\n\r\n" << hardwareText() << "\r\n";
    f << fmt("Used for calculation: %d cores, %.0f GB RAM, %.0f GB VRAM, %s, context %d\r\n\r\n",
             g_spec.cores, g_spec.ramGB, g_spec.vramGB, quants()[g_spec.quant].name, g_spec.ctx);
    f << fmt("%-34s %-20s %9s %10s  %-10s %-8s %8s\r\n", "Model", "Category", "Params", "Memory",
             "Result", "Mode", "tok/s");
    for (size_t i = 0; i < cat.size(); ++i) {
        const Verdict& v = g_verd[i];
        f << fmt("%-34s %-20s %9s %7.1f GB  %-10s %-8s %8s\r\n", cat[i].name, cat[i].category,
                 paramsText(cat[i]).c_str(), v.needGB, levelText(v.level), v.mode.c_str(),
                 v.tps > 0 ? fmt("%.1f", v.tps).c_str() : "-");
    }
    if (g_sel >= 0) f << "\r\n---\r\n" << recommend(cat[g_sel], g_spec);
}

static HWND mk(const char* cls, const char* text, DWORD style, int id, DWORD ex = 0) {
    return CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_hwnd,
                           (HMENU)(INT_PTR)id, g_inst, nullptr);
}

static void place(int id, int x, int y, int w, int h) {
    MoveWindow(GetDlgItem(g_hwnd, id), x, y, w, h, TRUE);
}

static void layout(int w, int h) {
    const int m = 10;
    int y = m;
    place(ID_GRP_HW, m, y, w - 2 * m, 96);
    place(ID_HWTEXT, m + 8, y + 18, w - 2 * m - 130, 74);
    place(ID_REDETECT, w - m - 112, y + 20, 104, 26);
    place(ID_EXPORT, w - m - 112, y + 54, 104, 26);
    y += 102;

    place(ID_GRP_WHATIF, m, y, w - 2 * m, 82);
    int x = m + 10, cy = y + 20;
    place(ID_L1, x, cy + 3, 68, 18);        place(ID_CORES, x + 70, cy, 45, 22);
    x += 135;
    place(ID_L2, x, cy + 3, 56, 18);        place(ID_RAM, x + 58, cy, 55, 22);
    x += 130;
    place(ID_L3, x, cy + 3, 68, 18);        place(ID_RAMBW, x + 70, cy, 55, 22);
    x += 150;
    place(ID_L4, x, cy + 3, 64, 18);        place(ID_VRAM, x + 66, cy, 55, 22);
    x = m + 10;
    cy += 28;
    place(ID_L5, x, cy + 3, 68, 18);        place(ID_GPUPRESET, x + 70, cy, 260, 300);
    place(ID_APPLY, x + 350, cy - 1, 70, 24);
    place(ID_RESET, x + 428, cy - 1, 130, 24);
    y += 90;

    place(ID_GRP_OPT, m, y, w - 2 * m, 52);
    x = m + 10;
    cy = y + 20;
    place(ID_L6, x, cy + 3, 40, 18);        place(ID_QUANT, x + 42, cy, 120, 200);
    x += 180;
    place(ID_L7, x, cy + 3, 52, 18);        place(ID_CTX, x + 54, cy, 70, 200);
    x += 140;
    place(ID_L8, x, cy + 3, 58, 18);        place(ID_CATF, x + 60, cy, 170, 300);
    x += 250;
    place(ID_HIDE, x, cy, 170, 22);
    y += 60;

    int recH = 190;
    int listH = std::max(100, h - y - recH - 26 - 2 * m);
    MoveWindow(g_list, m, y, w - 2 * m, listH, TRUE);
    y += listH + 4;
    MoveWindow(g_summary, m, y, w - 2 * m, 18, TRUE);
    y += 22;
    MoveWindow(g_rec, m, y, w - 2 * m, std::max(60, h - y - m), TRUE);
}

static void createControls() {
    mk("BUTTON", " Detected hardware ", BS_GROUPBOX, ID_GRP_HW);
    g_hwText = mk("STATIC", "", 0, ID_HWTEXT);
    mk("BUTTON", "Re-detect", BS_PUSHBUTTON | WS_TABSTOP, ID_REDETECT);
    mk("BUTTON", "Export report...", BS_PUSHBUTTON | WS_TABSTOP, ID_EXPORT);

    mk("BUTTON", " What-if hardware (edit to test upgrades, blank RAM GB/s = estimate) ",
       BS_GROUPBOX, ID_GRP_WHATIF);
    mk("STATIC", "CPU cores:", 0, ID_L1);
    g_edCores = mk("EDIT", "", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, ID_CORES, WS_EX_CLIENTEDGE);
    mk("STATIC", "RAM (GB):", 0, ID_L2);
    g_edRam = mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_RAM, WS_EX_CLIENTEDGE);
    mk("STATIC", "RAM GB/s:", 0, ID_L3);
    g_edRamBw = mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_RAMBW, WS_EX_CLIENTEDGE);
    mk("STATIC", "VRAM (GB):", 0, ID_L4);
    g_edVram = mk("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, ID_VRAM, WS_EX_CLIENTEDGE);
    mk("STATIC", "GPU preset:", 0, ID_L5);
    g_cbPreset = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_GPUPRESET);
    SendMessageA(g_cbPreset, CB_ADDSTRING, 0, (LPARAM) "Detected GPU (as found)");
    SendMessageA(g_cbPreset, CB_ADDSTRING, 0, (LPARAM) "No GPU (CPU only)");
    for (const auto& g : gpuPresets())
        SendMessageA(g_cbPreset, CB_ADDSTRING, 0,
                     (LPARAM)fmt("%s - %.0f GB", g.name, g.vramGB).c_str());
    SendMessage(g_cbPreset, CB_SETCURSEL, 0, 0);
    mk("BUTTON", "Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_APPLY);
    mk("BUTTON", "Reset to detected", BS_PUSHBUTTON | WS_TABSTOP, ID_RESET);

    mk("BUTTON", " Options ", BS_GROUPBOX, ID_GRP_OPT);
    mk("STATIC", "Quant:", 0, ID_L6);
    g_cbQuant = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_QUANT);
    for (const auto& q : quants()) SendMessageA(g_cbQuant, CB_ADDSTRING, 0, (LPARAM)q.name);
    SendMessage(g_cbQuant, CB_SETCURSEL, 0, 0);
    mk("STATIC", "Context:", 0, ID_L7);
    g_cbCtx = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_CTX);
    for (int c : contextSizes())
        SendMessageA(g_cbCtx, CB_ADDSTRING, 0, (LPARAM)fmt("%dK", c / 1024).c_str());
    SendMessage(g_cbCtx, CB_SETCURSEL, 1, 0);
    mk("STATIC", "Category:", 0, ID_L8);
    g_cbCat = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_CATF);
    SendMessageA(g_cbCat, CB_ADDSTRING, 0, (LPARAM) "All categories");
    for (const auto& m : modelCatalog())
        if (std::find(g_cats.begin(), g_cats.end(), m.category) == g_cats.end())
            g_cats.push_back(m.category);
    for (const auto& c : g_cats) SendMessageA(g_cbCat, CB_ADDSTRING, 0, (LPARAM)c.c_str());
    SendMessage(g_cbCat, CB_SETCURSEL, 0, 0);
    g_hide = mk("BUTTON", "Hide \"Cannot run\"", BS_AUTOCHECKBOX | WS_TABSTOP, ID_HIDE);

    g_list = mk(WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const char* cols[] = {"AI engine / model", "Category", "Params", "Memory needed", "Result",
                          "Mode", "Est. tokens/s"};
    const int widths[] = {210, 130, 70, 100, 85, 70, 90};
    for (int i = 0; i < 7; ++i) {
        LVCOLUMNA c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (char*)cols[i];
        c.cx = widths[i];
        SendMessageA(g_list, LVM_INSERTCOLUMNA, i, (LPARAM)&c);
    }

    g_summary = mk("STATIC", "", 0, ID_SUMMARY);
    g_rec = mk("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_REC,
               WS_EX_CLIENTEDGE);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(
        g_hwnd, [](HWND c, LPARAM f) -> BOOL { SendMessage(c, WM_SETFONT, f, TRUE); return TRUE; },
        (LPARAM)font);
}

static LRESULT onNotify(LPARAM lp) {
    NMHDR* h = (NMHDR*)lp;
    if (h->idFrom != ID_LIST) return 0;
    if (h->code == LVN_ITEMCHANGED) {
        NMLISTVIEW* nm = (NMLISTVIEW*)lp;
        if (!g_updating && (nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) &&
            nm->iItem >= 0 && nm->iItem < (int)g_view.size()) {
            g_sel = g_view[nm->iItem];
            refreshRecommendation();
        }
    } else if (h->code == LVN_COLUMNCLICK) {
        int col = ((NMLISTVIEW*)lp)->iSubItem;
        if (col == g_sortCol) g_sortAsc = !g_sortAsc;
        else { g_sortCol = col; g_sortAsc = (col != 4 && col != 6); }
        refreshList();
    } else if (h->code == NM_CUSTOMDRAW) {
        NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lp;
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            int i = (int)cd->nmcd.dwItemSpec;
            if (i >= 0 && i < (int)g_view.size()) {
                cd->clrTextBk = levelColor(g_verd[g_view[i]].level);
                cd->clrText = RGB(0, 0, 0);
            }
            return CDRF_NEWFONT;
        }
    }
    return 0;
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
            mi->ptMinTrackSize.x = 800;
            mi->ptMinTrackSize.y = 720;
            return 0;
        }
        case WM_NOTIFY:
            return onNotify(lp);
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_APPLY: applyWhatIf(); break;
                case ID_RESET:
                    g_spec = specFromDetected();
                    fillInputs();
                    refreshAll();
                    break;
                case ID_REDETECT: detectAndShow(); break;
                case ID_EXPORT: exportReport(); break;
                case ID_HIDE: refreshAll(); break;
                case ID_GPUPRESET:
                    if (HIWORD(wp) == CBN_SELCHANGE) onPresetChanged();
                    break;
                case ID_QUANT:
                case ID_CTX:
                case ID_CATF:
                    if (HIWORD(wp) == CBN_SELCHANGE) refreshAll();
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
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 860, 820, nullptr,
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
