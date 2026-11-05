#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <urlmon.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "advisor.h"
#include "hardware.h"
#include "i18n.h"

enum {
    ID_HWTEXT = 100, ID_REDETECT, ID_EXPORT,
    ID_CORES, ID_RAM, ID_RAMBW, ID_VRAM, ID_GPUPRESET, ID_APPLY, ID_RESET,
    ID_QUANT, ID_CTX, ID_CATF, ID_HIDE, ID_DARK,
    ID_LIST, ID_SUMMARY, ID_REC,
    ID_GRP_HW, ID_GRP_WHATIF, ID_GRP_OPT,
    ID_L1, ID_L2, ID_L3, ID_L4, ID_L5, ID_L6, ID_L7, ID_L8,
    ID_BENCH, ID_SAVEPROF, ID_LOADPROF, ID_COPYCMD, ID_ALLGPU, ID_FAVONLY, ID_CMP, ID_CMPPRESET,
    ID_CMPRAM, ID_L9, ID_LANG, ID_UPDATE
};

static HINSTANCE g_inst;
static HWND g_hwnd, g_hwText, g_list, g_rec, g_summary, g_hide, g_dark;
static bool g_isDark = false;
static HBRUSH g_bgBrush = nullptr, g_editBrush = nullptr;
static const COLORREF DARK_BG = RGB(32, 32, 32), DARK_EDIT = RGB(45, 45, 45),
                      DARK_TEXT = RGB(235, 235, 235);
static HWND g_edCores, g_edRam, g_edRamBw, g_edVram;
static HWND g_cbPreset, g_cbQuant, g_cbCtx, g_cbCat;
static HWND g_allGpu, g_favOnly, g_cmp, g_cbCmp, g_edCmpRam;
static std::vector<Verdict> g_verdB;   // verdicts for the compare setup
static std::set<std::string> g_fav;    // favorite model names
static double g_measuredBw = 0;        // RAM bandwidth from the benchmark, GB/s
static double g_diskFree = 0;
static int g_customAdded = 0;
static HwInfo g_hw;
static Spec g_spec;
static std::vector<std::string> g_cats;
static HWND g_lang = nullptr;
static std::map<HWND, std::string> g_enText;  // original English control texts
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
    if (g_measuredBw > 0) {
        s.ramBw = g_measuredBw;
        s.ramMeasured = true;
    }
    s.diskFreeGB = g_diskFree;
    int g = g_hw.bestGpu();
    if (g >= 0) {
        s.vramGB = g_hw.gpus[g].vramGB;
        s.gpuName = g_hw.gpus[g].name;
        if (g_allGpu && SendMessage(g_allGpu, BM_GETCHECK, 0, 0) == BST_CHECKED) {
            double sum = 0;
            for (const auto& gi : g_hw.gpus)
                if (gi.vramGB >= 1.0) sum += gi.vramGB;  // skip integrated GPUs
            s.vramGB = std::max(s.vramGB, sum);
        }
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
    if (g_measuredBw > 0) t += fmt("      measured read bandwidth: %.1f GB/s\r\n", g_measuredBw);
    if (g_hw.gpus.empty()) t += "GPU:  none detected\r\n";
    int dedicated = 0;
    for (const auto& g : g_hw.gpus) {
        t += fmt("GPU:  %s   VRAM %.1f GB (shared %.1f GB)\r\n", g.name.c_str(), g.vramGB, g.sharedGB);
        if (g.vramGB >= 1.0) ++dedicated;
    }
    if (dedicated > 1) t += fmt("      %d GPUs with VRAM: combined %.1f GB\r\n", dedicated, specFromDetected().vramGB);
    if (g_diskFree > 0) t += fmt("Disk: %.0f GB free for downloads\r\n", g_diskFree);
    return t;
}

static void showHw() { SetWindowTextA(g_hwText, tr(hardwareText()).c_str()); }

static std::string paramsText(const ModelInfo& m) {
    if (m.paramsB <= 0) return "-";
    if (m.paramsB < 1) return fmt("%.2g B", m.paramsB);
    return fmt("%.4g B", m.paramsB);
}

static int g_scalePct = 100;
static HFONT g_font = nullptr;
static int S(int v) { return MulDiv(v, g_scalePct, 100); }

static std::string exeDir() {
    char p[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string d = p;
    size_t k = d.find_last_of("\\/");
    return k == std::string::npos ? std::string() : d.substr(0, k);
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

static COLORREF levelColor(Level l, bool dark) {
    if (dark) {
        switch (l) {
            case LV_SMOOTH: return RGB(30, 80, 40);
            case LV_USABLE: return RGB(90, 80, 25);
            case LV_SLOW:   return RGB(100, 55, 30);
            default:        return RGB(100, 35, 35);
        }
    }
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
            case 2: {
                int c = capabilities(cat[a]).compare(capabilities(cat[b]));
                return g_sortAsc ? c < 0 : c > 0;
            }
            case 3: x = cat[a].paramsB; y = cat[b].paramsB; break;
            case 4: x = g_verd[a].needGB; y = g_verd[b].needGB; break;
            case 5: x = g_verd[a].level; y = g_verd[b].level; break;
            case 6: {
                int c = g_verd[a].mode.compare(g_verd[b].mode);
                return g_sortAsc ? c < 0 : c > 0;
            }
            case 8: x = g_verd[a].diskGB; y = g_verd[b].diskGB; break;
            case 9:
                if (g_verdB.size() != cat.size()) return false;
                x = g_verdB[a].level * 1000 + g_verdB[a].tps;
                y = g_verdB[b].level * 1000 + g_verdB[b].tps;
                break;
            default: x = g_verd[a].tps; y = g_verd[b].tps; break;
        }
        return g_sortAsc ? x < y : x > y;
    });
}

static void refreshRecommendation() {
    const auto& cat = modelCatalog();
    if (g_sel < 0 || g_sel >= (int)cat.size())
        SetWindowTextA(g_rec, tr("Click a model in the list to see the minimum hardware needed to run it smoothly.").c_str());
    else
    {
        std::string t = recommend(cat[g_sel], g_spec);
        if (cat[g_sel].kind == K_LLM && evaluate(cat[g_sel], g_spec).mode != "-")
            t += "\r\nSuggested llama.cpp command (button \"Copy run command\" copies it):\r\n" +
                 runCommand(cat[g_sel], g_spec);
        SetWindowTextA(g_rec, tr(t).c_str());
    }
}

static Spec specB() {
    Spec b = g_spec;
    b.ramGB = std::max(1.0, [&] {
        char t[64];
        GetWindowTextA(g_edCmpRam, t, sizeof t);
        return atof(t);
    }());
    b.ramBw = 0;
    b.ramMeasured = false;
    int p = (int)SendMessage(g_cbCmp, CB_GETCURSEL, 0, 0);
    b.vramOverridden = false;
    if (p <= 0) {
        b.vramGB = 0;
        b.gpuName.clear();
    } else {
        b.vramGB = gpuPresets()[p - 1].vramGB;
        b.gpuName = gpuPresets()[p - 1].name;
    }
    return b;
}

static bool isFav(const char* name) { return g_fav.count(name) > 0; }

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

    const bool cmp = SendMessage(g_cmp, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_verdB.assign(cat.size(), Verdict());
    if (cmp) {
        Spec sb = specB();
        for (size_t i = 0; i < cat.size(); ++i) g_verdB[i] = evaluate(cat[i], sb);
    }
    ListView_SetColumnWidth(g_list, 9, cmp ? S(150) : 0);

    int cf = (int)SendMessage(g_cbCat, CB_GETCURSEL, 0, 0);
    bool hide = SendMessage(g_hide, BM_GETCHECK, 0, 0) == BST_CHECKED;
    bool favOnly = SendMessage(g_favOnly, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_view.clear();
    for (size_t i = 0; i < cat.size(); ++i) {
        if (cf > 0 && g_cats[cf - 1] != cat[i].category) continue;
        if (hide && g_verd[i].level == LV_NO) continue;
        if (favOnly && !isFav(cat[i].name)) continue;
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
        std::string nm = (isFav(cat[i].name) ? "* " : "") + std::string(cat[i].name);
        it.pszText = (char*)nm.c_str();
        SendMessageA(g_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
        auto set = [&](int col, const std::string& s) {
            LVITEMA sub = {};
            sub.iSubItem = col;
            sub.pszText = (char*)s.c_str();
            SendMessageA(g_list, LVM_SETITEMTEXTA, r, (LPARAM)&sub);
        };
        set(1, cat[i].category);
        set(2, trTags(capabilities(cat[i])));
        set(3, paramsText(cat[i]));
        set(4, fmt("%.1f GB", v.needGB));
        set(5, tr(levelText(v.level)));
        set(6, v.mode);
        set(7, v.tps > 0 ? fmt("%.1f", v.tps) : std::string("-"));
        set(8, fmt("%.1f GB", v.diskGB));
        if (cmp) {
            const Verdict& vb = g_verdB[i];
            set(9, tr(levelText(vb.level)) + (vb.tps > 0 ? fmt("  %.1f tok/s", vb.tps) : ""));
        }
        if (i == g_sel) {
            ListView_SetItemState(g_list, (int)r, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_list, (int)r, FALSE);
        }
    }
    SendMessage(g_list, WM_SETREDRAW, TRUE, 0);
    g_updating = false;

    std::string sum = fmt(tr("Showing %d of %d.   Smooth: %d   Usable: %d   Slow: %d   Cannot run: %d").c_str(),
                          (int)g_view.size(), (int)cat.size(), cnt[LV_SMOOTH], cnt[LV_USABLE],
                          cnt[LV_SLOW], cnt[LV_NO]);
    if (best >= 0)
        sum += fmt(tr("   |   Best smooth LLM: %s (%.1f tok/s)").c_str(), cat[best].name, g_verd[best].tps);
    if (g_customAdded > 0) sum += fmt(tr("   |   +%d custom models").c_str(), g_customAdded);
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
    g_spec.ramMeasured = g_measuredBw > 0 && std::lround(g_spec.ramBw) == std::lround(g_measuredBw);
    if (g_spec.ramMeasured) g_spec.ramBw = g_measuredBw;
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

static void copyToClipboard(const std::string& text) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
    if (h) {
        std::memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}

static const char* REG_KEY = "Software\\Hardware-AI";

static void saveFavorites() {
    std::string v;
    for (const auto& n : g_fav) v += n + "|";
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k,
                        nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(k, "Favorites", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)v.size() + 1);
        RegCloseKey(k);
    }
}

static void loadFavorites() {
    HKEY k;
    char buf[16384] = {0};
    DWORD sz = sizeof buf - 1, type = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return;
    if (RegQueryValueExA(k, "Favorites", nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS &&
        type == REG_SZ) {
        std::string all = buf, cur;
        for (char c : all) {
            if (c == '|') {
                if (!cur.empty()) g_fav.insert(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    RegCloseKey(k);
}

static void runBenchmark() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    double bw = benchmarkRamBandwidth(std::max(2, g_hw.physicalCores));
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (bw <= 0) {
        MessageBoxA(g_hwnd, tr("The benchmark could not run (not enough free memory?).").c_str(), "Hardware-AI",
                    MB_ICONWARNING);
        return;
    }
    g_measuredBw = bw;
    g_spec.ramBw = bw;
    g_spec.ramMeasured = true;
    setEditNum(g_edRamBw, bw);
    showHw();
    refreshAll();
}

static void copyRunCommand() {
    const auto& cat = modelCatalog();
    if (g_sel < 0 || g_sel >= (int)cat.size()) {
        MessageBoxA(g_hwnd, tr("Select a model in the list first.").c_str(), "Hardware-AI", MB_ICONINFORMATION);
        return;
    }
    std::string cmd = runCommand(cat[g_sel], g_spec);
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n')) cmd.pop_back();
    copyToClipboard(cmd);
    MessageBoxA(g_hwnd, (tr("Copied to clipboard:\n\n") + cmd).c_str(), "Hardware-AI", MB_ICONINFORMATION);
}

static void saveProfile() {
    char path[MAX_PATH] = "my-pc.hwai";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Hardware-AI profile (*.hwai)\0*.hwai\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "hwai";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&ofn)) return;
    std::ofstream f(path);
    if (!f) {
        MessageBoxA(g_hwnd, tr("Cannot write the file.").c_str(), "Hardware-AI", MB_ICONERROR);
        return;
    }
    f << "cores=" << g_spec.cores << "\nram=" << g_spec.ramGB << "\nrambw=" << g_spec.ramBw
      << "\nvram=" << g_spec.vramGB << "\ngpu=" << g_spec.gpuName
      << "\noverridden=" << (g_spec.vramOverridden ? 1 : 0) << "\navx2=" << (g_spec.avx2 ? 1 : 0)
      << "\nquant=" << g_spec.quant << "\nctx=" << g_spec.ctx << "\n";
}

static void loadProfile() {
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = "Hardware-AI profile (*.hwai)\0*.hwai\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return;
    std::ifstream f(path);
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t e = line.find('=');
        if (e != std::string::npos) kv[line.substr(0, e)] = line.substr(e + 1);
    }
    if (!kv.count("cores") || !kv.count("ram")) {
        MessageBoxA(g_hwnd, tr("This is not a valid profile file.").c_str(), "Hardware-AI", MB_ICONWARNING);
        return;
    }
    auto num = [&](const char* k, double d) { return kv.count(k) ? atof(kv[k].c_str()) : d; };
    g_spec.cores = (int)std::max(1.0, num("cores", g_spec.cores));
    g_spec.ramGB = std::max(0.0, num("ram", g_spec.ramGB));
    g_spec.ramBw = std::max(0.0, num("rambw", 0));
    g_spec.ramMeasured = false;
    g_spec.vramGB = std::max(0.0, num("vram", 0));
    g_spec.gpuName = kv.count("gpu") ? kv["gpu"] : "";
    g_spec.vramOverridden = num("overridden", 0) != 0;
    g_spec.avx2 = num("avx2", 1) != 0;
    int q = (int)num("quant", 0), c = (int)num("ctx", 4096);
    q = std::max(0, std::min((int)quants().size() - 1, q));

    fillInputs();
    int preset = g_spec.vramGB <= 0 ? 1 : 0;
    for (size_t i = 0; i < gpuPresets().size(); ++i)
        if (g_spec.gpuName == gpuPresets()[i].name) preset = (int)i + 2;
    SendMessage(g_cbPreset, CB_SETCURSEL, preset, 0);
    SendMessage(g_cbQuant, CB_SETCURSEL, q, 0);
    for (size_t i = 0; i < contextSizes().size(); ++i)
        if (contextSizes()[i] == c) SendMessage(g_cbCtx, CB_SETCURSEL, i, 0);
    refreshAll();
}

static void detectAndShow() {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    g_hw = detectHardware();
    g_diskFree = freeDiskGB(exeDir());
    g_spec = specFromDetected();
    showHw();
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
        MessageBoxA(g_hwnd, tr("Cannot write the file.").c_str(), "Hardware-AI", MB_ICONERROR);
        return;
    }
    const auto& cat = modelCatalog();
    f << tr("Hardware-AI report") << "\r\n==================\r\n\r\n" << tr(hardwareText()) << "\r\n";
    f << fmt(tr("Used for calculation: %d cores, %.0f GB RAM, %.0f GB VRAM, %s, context %d\r\n\r\n").c_str(),
             g_spec.cores, g_spec.ramGB, g_spec.vramGB, quants()[g_spec.quant].name, g_spec.ctx);
    f << fmt("%-34s %-20s %-24s %9s %10s  %-10s %-8s %8s\r\n", tr("Model").c_str(), tr("Category").c_str(), tr("Can do").c_str(), tr("Params").c_str(), tr("Memory").c_str(),
             tr("Result").c_str(), tr("Mode").c_str(), "tok/s");
    for (size_t i = 0; i < cat.size(); ++i) {
        const Verdict& v = g_verd[i];
        f << fmt("%-34s %-20s %-24s %9s %7.1f GB  %-10s %-8s %8s\r\n", cat[i].name,
                 cat[i].category, trTags(capabilities(cat[i])).c_str(), paramsText(cat[i]).c_str(), v.needGB, tr(levelText(v.level)).c_str(), v.mode.c_str(),
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

// Everything is laid out for an 8pt font and multiplied by the font scale.

static void initFont() {
    const int pt = 10;  // UI font size in points
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    g_scalePct = MulDiv(100, pt * dpi, 8 * 96);
    g_font = CreateFontA(-MulDiv(pt, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_SWISS, "Segoe UI");
}

static void layout(int w, int h) {
    const int m = S(10);
    const int ctlH = S(24), lblH = S(20);
    int y = m;
    place(ID_GRP_HW, m, y, w - 2 * m, S(128));
    place(ID_HWTEXT, m + S(8), y + S(20), w - 2 * m - S(290), S(104));
    const int bx2 = w - m - S(122), bx1 = bx2 - S(122), bw = S(114), bh = S(28);
    place(ID_REDETECT, bx1, y + S(22), bw, bh);   place(ID_SAVEPROF, bx2, y + S(22), bw, bh);
    place(ID_BENCH, bx1, y + S(56), bw, bh);      place(ID_LOADPROF, bx2, y + S(56), bw, bh);
    place(ID_EXPORT, bx1, y + S(90), bw, bh);     place(ID_COPYCMD, bx2, y + S(90), bw, bh);
    y += S(134);

    place(ID_GRP_WHATIF, m, y, w - 2 * m, S(92));
    int x = m + S(10), cy = y + S(22);
    place(ID_L1, x, cy + S(2), S(72), lblH);        place(ID_CORES, x + S(74), cy, S(48), ctlH);
    x += S(145);
    place(ID_L2, x, cy + S(2), S(62), lblH);        place(ID_RAM, x + S(64), cy, S(58), ctlH);
    x += S(140);
    place(ID_L3, x, cy + S(2), S(72), lblH);        place(ID_RAMBW, x + S(74), cy, S(58), ctlH);
    x += S(160);
    place(ID_L4, x, cy + S(2), S(72), lblH);        place(ID_VRAM, x + S(74), cy, S(58), ctlH);
    x = m + S(10);
    cy += S(32);
    place(ID_L5, x, cy + S(2), S(76), lblH);        place(ID_GPUPRESET, x + S(78), cy, S(270), S(300));
    place(ID_APPLY, x + S(362), cy - S(1), S(70), S(26));
    place(ID_RESET, x + S(438), cy - S(1), S(140), S(26));
    place(ID_ALLGPU, x + S(588), cy, S(150), ctlH);
    y += S(98);

    place(ID_GRP_OPT, m, y, w - 2 * m, S(124));
    x = m + S(10);
    cy = y + S(22);
    place(ID_L6, x, cy + S(2), S(48), lblH);        place(ID_QUANT, x + S(50), cy, S(130), S(200));
    x += S(190);
    place(ID_L7, x, cy + S(2), S(60), lblH);        place(ID_CTX, x + S(62), cy, S(76), S(200));
    x += S(150);
    place(ID_L8, x, cy + S(2), S(68), lblH);        place(ID_CATF, x + S(70), cy, S(180), S(300));
    x += S(262);
    place(ID_HIDE, x, cy, S(140), ctlH);
    place(ID_DARK, x + S(146), cy, S(100), ctlH);
    x = m + S(10);
    cy += S(32);
    place(ID_FAVONLY, x, cy, S(150), ctlH);
    place(ID_CMP, x + S(160), cy, S(130), ctlH);
    place(ID_CMPPRESET, x + S(294), cy, S(240), S(300));
    place(ID_L9, x + S(546), cy + S(2), S(66), lblH);
    place(ID_CMPRAM, x + S(616), cy, S(58), ctlH);
    cy += S(32);
    place(ID_LANG, x, cy, S(130), ctlH);
    place(ID_UPDATE, x + S(160), cy - S(1), S(220), S(26));
    y += S(130);

    const int recH = S(170);
    int listH = std::max(S(100), h - y - recH - S(30) - 2 * m);
    MoveWindow(g_list, m, y, w - 2 * m, listH, TRUE);
    y += listH + S(4);
    MoveWindow(g_summary, m, y, w - 2 * m, S(20), TRUE);
    y += S(24);
    MoveWindow(g_rec, m, y, w - 2 * m, std::max(S(60), h - y - m), TRUE);
}

static const char* const COLS[] = {"AI engine / model", "Category", "Can do", "Params", "Memory needed",
                                   "Result", "Mode", "Est. tokens/s", "Download", "Compare"};

static void fillCategories() {
    std::string keep;
    int cur = (int)SendMessage(g_cbCat, CB_GETCURSEL, 0, 0);
    if (cur > 0 && cur - 1 < (int)g_cats.size()) keep = g_cats[cur - 1];
    g_cats.clear();
    for (const auto& m : modelCatalog())
        if (std::find(g_cats.begin(), g_cats.end(), m.category) == g_cats.end())
            g_cats.push_back(m.category);
    SendMessage(g_cbCat, CB_RESETCONTENT, 0, 0);
    SendMessageA(g_cbCat, CB_ADDSTRING, 0, (LPARAM)tr("All categories").c_str());
    int sel = 0;
    for (size_t i = 0; i < g_cats.size(); ++i) {
        SendMessageA(g_cbCat, CB_ADDSTRING, 0, (LPARAM)g_cats[i].c_str());
        if (g_cats[i] == keep) sel = (int)i + 1;
    }
    SendMessage(g_cbCat, CB_SETCURSEL, sel, 0);
}

static void createControls() {
    mk("BUTTON", " Detected hardware ", BS_GROUPBOX, ID_GRP_HW);
    g_hwText = mk("STATIC", "", 0, ID_HWTEXT);
    mk("BUTTON", "Re-detect", BS_PUSHBUTTON | WS_TABSTOP, ID_REDETECT);
    mk("BUTTON", "Export report...", BS_PUSHBUTTON | WS_TABSTOP, ID_EXPORT);
    mk("BUTTON", "Benchmark RAM", BS_PUSHBUTTON | WS_TABSTOP, ID_BENCH);
    mk("BUTTON", "Save profile...", BS_PUSHBUTTON | WS_TABSTOP, ID_SAVEPROF);
    mk("BUTTON", "Load profile...", BS_PUSHBUTTON | WS_TABSTOP, ID_LOADPROF);
    mk("BUTTON", "Copy run command", BS_PUSHBUTTON | WS_TABSTOP, ID_COPYCMD);

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
    g_allGpu = mk("BUTTON", "Sum all GPUs", BS_AUTOCHECKBOX | WS_TABSTOP, ID_ALLGPU);
    SendMessage(g_allGpu, BM_SETCHECK, BST_CHECKED, 0);

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
    fillCategories();
    g_hide = mk("BUTTON", "Hide \"Cannot run\"", BS_AUTOCHECKBOX | WS_TABSTOP, ID_HIDE);
    g_dark = mk("BUTTON", "Dark mode", BS_AUTOCHECKBOX | WS_TABSTOP, ID_DARK);
    g_favOnly = mk("BUTTON", "Favorites only", BS_AUTOCHECKBOX | WS_TABSTOP, ID_FAVONLY);
    g_lang = mk("BUTTON", "Portugu\xEAs", BS_AUTOCHECKBOX | WS_TABSTOP, ID_LANG);
    mk("BUTTON", "Update models online", BS_PUSHBUTTON | WS_TABSTOP, ID_UPDATE);
    g_cmp = mk("BUTTON", "Compare with:", BS_AUTOCHECKBOX | WS_TABSTOP, ID_CMP);
    g_cbCmp = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_CMPPRESET);
    SendMessageA(g_cbCmp, CB_ADDSTRING, 0, (LPARAM) "No GPU (CPU only)");
    int cmpDefault = 0, idx = 1;
    for (const auto& g : gpuPresets()) {
        SendMessageA(g_cbCmp, CB_ADDSTRING, 0, (LPARAM)fmt("%s - %.0f GB", g.name, g.vramGB).c_str());
        if (std::string(g.name) == "RTX 4090") cmpDefault = idx;
        ++idx;
    }
    SendMessage(g_cbCmp, CB_SETCURSEL, cmpDefault, 0);
    mk("STATIC", "RAM (GB):", 0, ID_L9);
    g_edCmpRam = mk("EDIT", "64", ES_AUTOHSCROLL | WS_TABSTOP, ID_CMPRAM, WS_EX_CLIENTEDGE);

    g_list = mk(WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    const char* const* cols = COLS;
    const int widths[] = {S(190), S(110), S(140), S(62), S(100), S(80), S(68), S(92), S(84), 0};
    for (int i = 0; i < 10; ++i) {
        LVCOLUMNA c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (char*)cols[i];
        c.cx = widths[i];
        SendMessageA(g_list, LVM_INSERTCOLUMNA, i, (LPARAM)&c);
    }

    g_summary = mk("STATIC", "", 0, ID_SUMMARY);
    g_rec = mk("EDIT", "", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, ID_REC,
               WS_EX_CLIENTEDGE);

    EnumChildWindows(
        g_hwnd,
        [](HWND c, LPARAM) -> BOOL {
            char cls[32], t[256];
            GetClassNameA(c, cls, sizeof cls);
            int id = GetDlgCtrlID(c);
            if ((!_stricmp(cls, "Button") || !_stricmp(cls, "Static")) && id != ID_HWTEXT &&
                id != ID_SUMMARY && id != ID_LANG) {
                GetWindowTextA(c, t, sizeof t);
                if (t[0]) g_enText[c] = t;
            }
            return TRUE;
        },
        0);

    HFONT font = g_font;
    EnumChildWindows(
        g_hwnd, [](HWND c, LPARAM f) -> BOOL { SendMessage(c, WM_SETFONT, f, TRUE); return TRUE; },
        (LPARAM)font);
}

static void retitleCombo(HWND cb, int idx, const std::string& text) {
    int sel = (int)SendMessage(cb, CB_GETCURSEL, 0, 0);
    SendMessage(cb, CB_DELETESTRING, idx, 0);
    SendMessageA(cb, CB_INSERTSTRING, idx, (LPARAM)text.c_str());
    SendMessage(cb, CB_SETCURSEL, sel, 0);
}

static void applyLanguage() {
    for (const auto& kv : g_enText) SetWindowTextA(kv.first, tr(kv.second).c_str());
    for (int i = 0; i < 10; ++i) {
        std::string t = tr(COLS[i]);
        LVCOLUMNA c = {};
        c.mask = LVCF_TEXT;
        c.pszText = (char*)t.c_str();
        SendMessageA(g_list, LVM_SETCOLUMNA, i, (LPARAM)&c);
    }
    retitleCombo(g_cbPreset, 0, tr("Detected GPU (as found)"));
    retitleCombo(g_cbPreset, 1, tr("No GPU (CPU only)"));
    retitleCombo(g_cbCmp, 0, tr("No GPU (CPU only)"));
    fillCategories();
    showHw();
    refreshAll();
}

static bool loadLanguageSetting() {
    DWORD v = 0, sz = sizeof v, type = 0;
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
        LONG r = RegQueryValueExA(k, "Language", nullptr, &type, (BYTE*)&v, &sz);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS && type == REG_DWORD) return v == 1;
    }
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
}

static void saveLanguageSetting(bool pt) {
    HKEY k;
    DWORD v = pt ? 1 : 0;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExA(k, "Language", 0, REG_DWORD, (const BYTE*)&v, sizeof v);
        RegCloseKey(k);
    }
}

static const char* ONLINE_URL = "https://raw.githubusercontent.com/githurby/Hardware-AI/main/models-online.csv";

// Cached copy of the online list, kept in %APPDATA% so it works without write access to the exe folder.
static std::string onlineCachePath(bool createDir) {
    const char* ad = getenv("APPDATA");
    if (!ad) return std::string();
    std::string d = std::string(ad) + "\\Hardware-AI";
    if (createDir) CreateDirectoryA(d.c_str(), nullptr);
    return d + "\\models-online.csv";
}

static void updateModelsOnline() {
    std::string dst = onlineCachePath(true);
    if (dst.empty()) return;
    std::string tmp = dst + ".tmp";
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    // The query string defeats proxy/IE caching of the raw file.
    std::string url = std::string(ONLINE_URL) + "?t=" + std::to_string(GetTickCount());
    HRESULT hr = URLDownloadToFileA(nullptr, url.c_str(), tmp.c_str(), 0, nullptr);
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    if (FAILED(hr)) {
        DeleteFileA(tmp.c_str());
        MessageBoxA(g_hwnd,
                    tr("Downloading the latest model list failed. Check your internet connection.").c_str(),
                    "Hardware-AI", MB_ICONWARNING);
        return;
    }
    std::ifstream f(tmp);
    std::string line;
    int rows = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '<') {  // an HTML error page
            rows = -1;
            break;
        }
        if (std::count(line.begin(), line.end(), ',') >= 6) ++rows;
    }
    f.close();
    if (rows <= 0) {
        DeleteFileA(tmp.c_str());
        MessageBoxA(g_hwnd, tr("The downloaded file is not a valid model list.").c_str(), "Hardware-AI",
                    MB_ICONWARNING);
        return;
    }
    MoveFileExA(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);

    std::string keep = (g_sel >= 0 && g_sel < (int)modelCatalog().size()) ? modelCatalog()[g_sel].name : "";
    int added = loadCustomModels(dst);
    g_customAdded += added;
    g_sel = -1;
    for (size_t i = 0; i < modelCatalog().size(); ++i)
        if (keep == modelCatalog()[i].name) g_sel = (int)i;
    fillCategories();
    refreshAll();
    if (added > 0)
        MessageBoxA(g_hwnd, fmt(tr("Added %d new models from the online list.").c_str(), added).c_str(),
                    "Hardware-AI", MB_ICONINFORMATION);
    else
        MessageBoxA(g_hwnd, tr("Model list is already up to date.").c_str(), "Hardware-AI", MB_ICONINFORMATION);
}

static void setTheme(HWND h, const wchar_t* name) {
    typedef HRESULT(WINAPI * Fn)(HWND, LPCWSTR, LPCWSTR);
    static Fn fn = (Fn)(void*)GetProcAddress(LoadLibraryA("uxtheme.dll"), "SetWindowTheme");
    if (fn) fn(h, name, nullptr);
}

static void saveDarkSetting() {
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Hardware-AI", 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD v = g_isDark;
        RegSetValueExA(k, "DarkMode", 0, REG_DWORD, (const BYTE*)&v, sizeof v);
        RegCloseKey(k);
    }
}

static bool loadDarkSetting() {
    HKEY k;
    DWORD v = 0, sz = sizeof v;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Hardware-AI", 0, KEY_QUERY_VALUE, &k) ==
        ERROR_SUCCESS) {
        RegQueryValueExA(k, "DarkMode", nullptr, nullptr, (BYTE*)&v, &sz);
        RegCloseKey(k);
    }
    return v != 0;
}

static void applyTheme(bool dark) {
    g_isDark = dark;
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_editBrush) DeleteObject(g_editBrush);
    g_bgBrush = CreateSolidBrush(dark ? DARK_BG : GetSysColor(COLOR_BTNFACE));
    g_editBrush = CreateSolidBrush(dark ? DARK_EDIT : GetSysColor(COLOR_WINDOW));
    SetClassLongPtr(g_hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)g_bgBrush);

    BOOL on = dark;
    DwmSetWindowAttribute(g_hwnd, 20, &on, sizeof on);  // dark title bar (Windows 10 20H1+)

    EnumChildWindows(
        g_hwnd,
        [](HWND c, LPARAM) -> BOOL {
            char cls[32];
            GetClassNameA(c, cls, sizeof cls);
            LONG st = GetWindowLong(c, GWL_STYLE) & BS_TYPEMASK;
            bool plain = _stricmp(cls, "Button") == 0 && (st == BS_GROUPBOX || st == BS_AUTOCHECKBOX);
            // Classic style makes group boxes and checkboxes honor the text color.
            if (plain) setTheme(c, g_isDark ? L"" : nullptr);
            else setTheme(c, g_isDark ? L"DarkMode_Explorer" : nullptr);
            return TRUE;
        },
        0);
    ListView_SetBkColor(g_list, dark ? DARK_EDIT : GetSysColor(COLOR_WINDOW));
    ListView_SetTextBkColor(g_list, dark ? DARK_EDIT : GetSysColor(COLOR_WINDOW));
    ListView_SetTextColor(g_list, dark ? DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT));
    SendMessage(g_dark, BM_SETCHECK, dark ? BST_CHECKED : BST_UNCHECKED, 0);
    RedrawWindow(g_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
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
        else { g_sortCol = col; g_sortAsc = (col != 5 && col != 7 && col != 9); }
        refreshList();
    } else if (h->code == NM_DBLCLK) {
        int r = ((NMITEMACTIVATE*)lp)->iItem;
        if (r >= 0 && r < (int)g_view.size()) {
            const char* name = modelCatalog()[g_view[r]].name;
            g_sel = g_view[r];
            if (!g_fav.erase(name)) g_fav.insert(name);
            saveFavorites();
            refreshList();
        }
    } else if (h->code == NM_CUSTOMDRAW) {
        NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lp;
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            int i = (int)cd->nmcd.dwItemSpec;
            if (i >= 0 && i < (int)g_view.size()) {
                cd->clrTextBk = levelColor(g_verd[g_view[i]].level, g_isDark);
                cd->clrText = g_isDark ? DARK_TEXT : RGB(0, 0, 0);
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
            loadFavorites();
            {
                // models.csv next to the exe, or up to three folders above it (Code::Blocks bin/Release)
                std::string d = exeDir();
                for (int up = 0; up < 4 && !d.empty(); ++up) {
                    g_customAdded += loadCustomModels(d + "\\models.csv");
                    g_customAdded += loadCustomModels(d + "\\models-online.csv");
                    size_t k = d.find_last_of("\\/");
                    d = k == std::string::npos ? std::string() : d.substr(0, k);
                }
            }
            {
                std::string cache = onlineCachePath(false);
                if (!cache.empty()) g_customAdded += loadCustomModels(cache);
            }
            setPortuguese(loadLanguageSetting());
            initFont();
            createControls();
            detectAndShow();
            if (isPortuguese()) {
                SendMessage(g_lang, BM_SETCHECK, BST_CHECKED, 0);
                applyLanguage();
            }
            applyTheme(loadDarkSetting());
            return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) layout(LOWORD(lp), HIWORD(lp));
            return 0;
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mi = (MINMAXINFO*)lp;
            RECT wa;
            SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
            mi->ptMinTrackSize.x = std::min<LONG>(S(900) + 20, wa.right - wa.left);
            mi->ptMinTrackSize.y = std::min<LONG>(S(672) + 40, wa.bottom - wa.top);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            if (!g_isDark) break;
            SetTextColor((HDC)wp, DARK_TEXT);
            SetBkColor((HDC)wp, DARK_BG);
            return (LRESULT)g_bgBrush;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (!g_isDark) break;
            SetTextColor((HDC)wp, DARK_TEXT);
            SetBkColor((HDC)wp, DARK_EDIT);
            return (LRESULT)g_editBrush;
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
                case ID_BENCH: runBenchmark(); break;
                case ID_LANG:
                    setPortuguese(SendMessage(g_lang, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    saveLanguageSetting(isPortuguese());
                    applyLanguage();
                    break;
                case ID_UPDATE: updateModelsOnline(); break;
                case ID_SAVEPROF: saveProfile(); break;
                case ID_LOADPROF: loadProfile(); break;
                case ID_COPYCMD: copyRunCommand(); break;
                case ID_ALLGPU:
                    g_spec = specFromDetected();
                    fillInputs();
                    showHw();
                    refreshAll();
                    break;
                case ID_FAVONLY:
                case ID_CMP: refreshAll(); break;
                case ID_CMPPRESET:
                    if (HIWORD(wp) == CBN_SELCHANGE) refreshAll();
                    break;
                case ID_CMPRAM:
                    if (HIWORD(wp) == EN_CHANGE) refreshAll();
                    break;
                case ID_EXPORT: exportReport(); break;
                case ID_HIDE: refreshAll(); break;
                case ID_DARK:
                    applyTheme(SendMessage(g_dark, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    saveDarkSetting();
                    break;
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

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "HardwareAIWnd";
    wc.hIconSm = (HICON)LoadImage(inst, MAKEINTRESOURCE(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExA(&wc);

    RECT wa;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    {
        HDC dc = GetDC(nullptr);
        int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        g_scalePct = MulDiv(100, 10 * dpi, 8 * 96);  // same value initFont() computes
    }
    int ww = std::min<int>(S(940) + 20, wa.right - wa.left);
    int wh = std::min<int>(S(812) + 40, wa.bottom - wa.top);
    HWND w = CreateWindowA(wc.lpszClassName, "Hardware-AI - what AI can my PC run?",
                           WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - ww) / 2,
                           wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, nullptr,
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
