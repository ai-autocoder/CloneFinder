// Build (MinGW-w64 / winlibs):
//   g++ -O2 -std=c++17 -static -mwindows clone-finder.cpp -o clone-finder.exe ^
//       -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -pthread

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <sstream>
#include <cstring>
#include <cstdio>

// ============================================================
// Core comparison logic
// ============================================================

struct ComparisonResult {
    std::string filePath;
    int differentLines = 0;
    int linesAdded = 0;
    int linesRemoved = 0;
    double similarity = 0.0;
};

static std::vector<std::string> splitIntoNormalizedLines(const std::string &content) {
    std::vector<std::string> lines;
    lines.reserve(256);
    std::string temp;
    auto flush = [&]() {
        std::string processed;
        processed.reserve(temp.size());
        for (char x : temp) {
            unsigned char uc = static_cast<unsigned char>(x);
            if (!std::isspace(uc)) processed.push_back(static_cast<char>(std::tolower(uc)));
        }
        lines.push_back(std::move(processed));
    };
    for (char c : content) {
        if (c == '\n') {
            if (!temp.empty()) flush();
            temp.clear();
        } else {
            temp.push_back(c);
        }
    }
    if (!temp.empty()) flush();
    return lines;
}

static std::vector<std::string> readAllLines(const std::string &filePath) {
    std::ifstream f(filePath, std::ios::binary);
    if (!f.is_open()) return {};
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return splitIntoNormalizedLines(raw);
}

// O(min(n,m)) memory LCS — two-row DP.
static void computeDiffCounts(const std::vector<std::string> &A, const std::vector<std::string> &B,
                              int &diffCount, int &added, int &removed) {
    int lenA = static_cast<int>(A.size());
    int lenB = static_cast<int>(B.size());

    const std::vector<std::string> *shortV = &A;
    const std::vector<std::string> *longV  = &B;
    int n = lenA, m = lenB;
    if (m < n) { std::swap(shortV, longV); std::swap(n, m); }

    std::vector<int> prev(n + 1, 0), curr(n + 1, 0);
    for (int i = 1; i <= m; ++i) {
        const std::string &bi = (*longV)[i - 1];
        for (int j = 1; j <= n; ++j) {
            if ((*shortV)[j - 1] == bi) curr[j] = prev[j - 1] + 1;
            else                         curr[j] = std::max(prev[j], curr[j - 1]);
        }
        std::swap(prev, curr);
    }
    int lcsLen = prev[n];
    removed = lenA - lcsLen;
    added   = lenB - lcsLen;
    diffCount = removed + added;
}

static ComparisonResult compareAgainstTarget(const std::string &otherFile,
                                             const std::vector<std::string> &targetLines) {
    std::vector<std::string> otherLines = readAllLines(otherFile);
    ComparisonResult r;
    r.filePath = otherFile;
    computeDiffCounts(targetLines, otherLines, r.differentLines, r.linesAdded, r.linesRemoved);
    size_t targetLineCount = targetLines.size();
    if (targetLineCount == 0) {
        r.similarity = 0.0;
    } else {
        double ratio = (double)r.differentLines / (double)targetLineCount;
        r.similarity = (ratio >= 1.0) ? 0.0 : (1.0 - ratio) * 100.0;
    }
    return r;
}

// Expand a single folder path that may contain one '*' wildcard segment.
// e.g. "C:/Projects/*/src" ->
//      ["C:/Projects/v1.0/src", "C:/Projects/v2.0/src", ...]
// If no '*' is present the path is returned as-is (in a 1-element vector).
static std::vector<std::string> expandWildcardFolder(const std::string &pattern) {
    namespace fs = std::filesystem;
    size_t star = pattern.find('*');
    if (star == std::string::npos) return {pattern};

    // Split at the '*' segment boundary
    size_t segStart = pattern.rfind('/', star);
    if (segStart == std::string::npos) segStart = pattern.rfind('\\', star);
    size_t segEnd   = pattern.find_first_of("/\\", star);

    std::string base   = (segStart == std::string::npos) ? "" : pattern.substr(0, segStart);
    std::string suffix = (segEnd   == std::string::npos) ? "" : pattern.substr(segEnd);

    std::vector<std::string> results;
    std::error_code ec;
    if (base.empty() || !fs::exists(base, ec)) return results;
    for (auto &entry : fs::directory_iterator(base, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_directory()) continue;
        fs::path candidate = entry.path();
        if (!suffix.empty()) candidate /= fs::path(suffix.substr(1)); // strip leading separator
        if (fs::is_directory(candidate, ec))
            results.push_back(candidate.string());
    }
    return results;
}

static std::vector<std::string> getAllFiles(const std::string &rootPath,
                                            const std::string &extension,
                                            bool recursive) {
    std::vector<std::string> results;
    std::error_code ec;
    if (!std::filesystem::exists(rootPath, ec)) return results;
    try {
        auto opts = std::filesystem::directory_options::skip_permission_denied;
        if (recursive) {
            for (auto &p : std::filesystem::recursive_directory_iterator(rootPath, opts)) {
                std::error_code e;
                if (!p.is_directory(e) && p.path().extension() == extension)
                    results.push_back(p.path().string());
            }
        } else {
            for (auto &p : std::filesystem::directory_iterator(rootPath, opts)) {
                std::error_code e;
                if (!p.is_directory(e) && p.path().extension() == extension)
                    results.push_back(p.path().string());
            }
        }
    } catch (...) { /* swallow traversal errors */ }
    return results;
}

// ============================================================
// GUI
// ============================================================

enum : int {
    IDC_TARGET_EDIT   = 1001,
    IDC_BROWSE_FILE   = 1002,
    IDC_FOLDERS_LIST  = 1003,
    IDC_ADD_FOLDER    = 1004,
    IDC_REMOVE_FOLDER = 1005,
    IDC_EXT_EDIT      = 1006,
    IDC_EDIT_FOLDER   = 1013,
    IDC_RECURSIVE     = 1007,
    IDC_START         = 1008,
    IDC_CANCEL        = 1009,
    IDC_PROGRESS      = 1010,
    IDC_STATUS        = 1011,
    IDC_RESULTS       = 1012,
    IDC_LBL_TARGET    = 1100,
    IDC_LBL_FOLDERS   = 1101,
    IDC_LBL_EXT       = 1102,
};

enum : UINT {
    WM_APP_PROGRESS = WM_APP + 1, // wParam = done, lParam = total
    WM_APP_DONE     = WM_APP + 2, // lParam = std::vector<ComparisonResult>*
    WM_APP_STATUS   = WM_APP + 3, // lParam = char*  (heap, delete[])
};

struct AppState {
    HWND hMain = nullptr;
    HWND hTarget = nullptr, hFolders = nullptr, hExt = nullptr, hRecursive = nullptr;
    HWND hStart = nullptr, hCancel = nullptr, hProgress = nullptr, hStatus = nullptr;
    HWND hResults = nullptr;
    HFONT hFont = nullptr;

    int  sortCol = 1;   // 0=File 1=Similarity 2=DiffLines 3=Added 4=Removed
    bool sortAsc = false;

    std::vector<ComparisonResult> allResults;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> running{false};
    std::thread worker;
};
static AppState g;

static void postStatus(const std::string &s) {
    char *buf = new char[s.size() + 1];
    std::memcpy(buf, s.c_str(), s.size() + 1);
    PostMessageA(g.hMain, WM_APP_STATUS, 0, reinterpret_cast<LPARAM>(buf));
}

// ------------------------------------------------------------
// Worker
// ------------------------------------------------------------

static void worker_run(std::string targetFile,
                       std::vector<std::string> folders,
                       std::string ext,
                       bool recursive) {
    postStatus("Scanning folders...");
    std::vector<std::string> allFiles;
    for (auto &folder : folders) {
        if (g.cancelRequested) break;
        for (auto &resolved : expandWildcardFolder(folder)) {
            if (g.cancelRequested) break;
            auto files = getAllFiles(resolved, ext, recursive);
            allFiles.insert(allFiles.end(), files.begin(), files.end());
        }
    }

    if (g.cancelRequested || allFiles.empty()) {
        postStatus(g.cancelRequested ? "Cancelled." : "No matching files found.");
        PostMessageA(g.hMain, WM_APP_DONE, 0,
                     reinterpret_cast<LPARAM>(new std::vector<ComparisonResult>()));
        return;
    }

    postStatus("Reading target file...");
    std::vector<std::string> targetLines = readAllLines(targetFile);

    size_t total = allFiles.size();
    std::atomic<size_t> nextIdx{0};
    std::atomic<size_t> doneCount{0};
    std::vector<ComparisonResult> results(total);

    unsigned numThreads = std::max(1u, std::thread::hardware_concurrency());
    if (total < numThreads) numThreads = static_cast<unsigned>(std::max<size_t>(1, total));

    PostMessageA(g.hMain, WM_APP_PROGRESS, 0, static_cast<LPARAM>(total));

    std::vector<std::thread> pool;
    pool.reserve(numThreads);
    for (unsigned t = 0; t < numThreads; ++t) {
        pool.emplace_back([&]() {
            while (!g.cancelRequested) {
                size_t i = nextIdx.fetch_add(1);
                if (i >= total) break;
                results[i] = compareAgainstTarget(allFiles[i], targetLines);
                size_t d = doneCount.fetch_add(1) + 1;
                if (d % 8 == 0 || d == total) {
                    PostMessageA(g.hMain, WM_APP_PROGRESS,
                                 static_cast<WPARAM>(d), static_cast<LPARAM>(total));
                }
            }
        });
    }
    for (auto &th : pool) th.join();

    if (g.cancelRequested) {
        postStatus("Cancelled.");
    } else {
        std::ostringstream oss;
        oss << "Done. Compared " << total << " files.";
        postStatus(oss.str());
    }

    auto *payload = new std::vector<ComparisonResult>(std::move(results));
    PostMessageA(g.hMain, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(payload));
}

// ------------------------------------------------------------
// Dialog helpers
// ------------------------------------------------------------

static std::string wideToAnsi(PCWSTR w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

static std::string pickFolder(HWND owner) {
    std::string result;
    IFileOpenDialog *dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = wideToAnsi(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

static std::string pickFile(HWND owner) {
    char buf[4096] = {0};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf);
    ofn.lpstrFilter = "All Files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return buf;
    return {};
}

// ------------------------------------------------------------
// Folder add/edit modal dialog (in-memory DLGTEMPLATE)
// ------------------------------------------------------------

static constexpr WORD IDC_DLG_EDIT_PATH  = 2001;
static constexpr WORD IDC_DLG_BROWSE_BTN = 2002;

static void dlgAppendWord(std::vector<BYTE> &v, WORD w) {
    v.push_back(static_cast<BYTE>(w & 0xFF));
    v.push_back(static_cast<BYTE>((w >> 8) & 0xFF));
}
static void dlgAppendDword(std::vector<BYTE> &v, DWORD d) {
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<BYTE>((d >> (i * 8)) & 0xFF));
}
static void dlgAppendShort(std::vector<BYTE> &v, short s) {
    dlgAppendWord(v, static_cast<WORD>(s));
}
static void dlgAppendUtf16(std::vector<BYTE> &v, const char *s) {
    while (*s) {
        dlgAppendWord(v, static_cast<WORD>(static_cast<unsigned char>(*s)));
        ++s;
    }
    dlgAppendWord(v, 0);
}
static void dlgAlignDword(std::vector<BYTE> &v) {
    while (v.size() % 4) v.push_back(0);
}

static void dlgAppendControl(std::vector<BYTE> &v,
                             DWORD style, DWORD exStyle,
                             short x, short y, short cx, short cy,
                             WORD id, WORD classAtom, const char *text) {
    dlgAlignDword(v);
    dlgAppendDword(v, style);
    dlgAppendDword(v, exStyle);
    dlgAppendShort(v, x);
    dlgAppendShort(v, y);
    dlgAppendShort(v, cx);
    dlgAppendShort(v, cy);
    dlgAppendWord(v, id);
    dlgAppendWord(v, 0xFFFF);
    dlgAppendWord(v, classAtom);
    dlgAppendUtf16(v, text);
    dlgAppendWord(v, 0);
}

static INT_PTR CALLBACK promptDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrA(hDlg, GWLP_USERDATA, static_cast<LONG_PTR>(lParam));
        std::string *p = reinterpret_cast<std::string*>(lParam);
        if (p && !p->empty()) SetDlgItemTextA(hDlg, IDC_DLG_EDIT_PATH, p->c_str());
        HWND hEdit = GetDlgItem(hDlg, IDC_DLG_EDIT_PATH);
        SendMessageA(hEdit, EM_SETSEL, 0, -1);
        SetFocus(hEdit);
        return FALSE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDOK) {
            char buf[4096] = {};
            GetDlgItemTextA(hDlg, IDC_DLG_EDIT_PATH, buf, sizeof(buf));
            std::string s = buf;
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
            if (s.empty()) {
                MessageBoxA(hDlg, "Please enter a folder path or wildcard pattern.",
                            "Empty input", MB_ICONWARNING);
                return TRUE;
            }
            std::string *out = reinterpret_cast<std::string*>(
                GetWindowLongPtrA(hDlg, GWLP_USERDATA));
            if (out) *out = s;
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        if (id == IDC_DLG_BROWSE_BTN) {
            std::string s = pickFolder(hDlg);
            if (!s.empty()) SetDlgItemTextA(hDlg, IDC_DLG_EDIT_PATH, s.c_str());
            return TRUE;
        }
        return FALSE;
    }
    }
    return FALSE;
}

static bool promptForFolder(HWND owner, std::string &inout) {
    std::vector<BYTE> tpl;
    tpl.reserve(512);

    DWORD style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlgAppendDword(tpl, style);
    dlgAppendDword(tpl, 0);
    dlgAppendWord(tpl, 5);
    dlgAppendShort(tpl, 0);
    dlgAppendShort(tpl, 0);
    dlgAppendShort(tpl, 280);
    dlgAppendShort(tpl, 60);
    dlgAppendWord(tpl, 0);
    dlgAppendWord(tpl, 0);
    dlgAppendUtf16(tpl, inout.empty() ? "Add folder" : "Edit folder");
    dlgAppendWord(tpl, 8);
    dlgAppendUtf16(tpl, "MS Shell Dlg");

    dlgAppendControl(tpl,
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
        7, 7, 200, 8, 0xFFFF, 0x0082,
        "Path or wildcard pattern:");

    dlgAppendControl(tpl,
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 0,
        7, 18, 215, 14, IDC_DLG_EDIT_PATH, 0x0081, "");

    dlgAppendControl(tpl,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
        226, 18, 47, 14, IDC_DLG_BROWSE_BTN, 0x0080, "Browse...");

    dlgAppendControl(tpl,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0,
        166, 40, 50, 14, IDOK, 0x0080, "OK");

    dlgAppendControl(tpl,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
        222, 40, 50, 14, IDCANCEL, 0x0080, "Cancel");

    INT_PTR r = DialogBoxIndirectParamA(
        GetModuleHandleA(nullptr),
        reinterpret_cast<LPCDLGTEMPLATEA>(tpl.data()),
        owner,
        promptDlgProc,
        reinterpret_cast<LPARAM>(&inout));
    return r == IDOK;
}

static void updateFolderButtons() {
    BOOL hasSel = (SendMessage(g.hFolders, LB_GETCURSEL, 0, 0) != LB_ERR);
    EnableWindow(GetDlgItem(g.hMain, IDC_EDIT_FOLDER),   hasSel);
    EnableWindow(GetDlgItem(g.hMain, IDC_REMOVE_FOLDER), hasSel);
}

// ------------------------------------------------------------
// ListView
// ------------------------------------------------------------

static void setupListView(HWND lv) {
    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    struct { const char *name; int width; } cols[] = {
        {"File",          520},
        {"Similarity %",   90},
        {"Diff Lines",     90},
        {"Added",          80},
        {"Removed",        80},
    };
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNA c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<LPSTR>(cols[i].name);
        c.cx = cols[i].width;
        c.iSubItem = i;
        ListView_InsertColumn(lv, i, &c);
    }
}

static void updateSortArrow(HWND lv, int col, bool asc) {
    HWND hdr = ListView_GetHeader(lv);
    int count = Header_GetItemCount(hdr);
    for (int i = 0; i < count; ++i) {
        HDITEM hi = {};
        hi.mask = HDI_FORMAT;
        Header_GetItem(hdr, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == col) hi.fmt |= asc ? HDF_SORTUP : HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hi);
    }
}

static void populateResults(HWND lv, const std::vector<ComparisonResult> &results, int col, bool asc) {
    ListView_DeleteAllItems(lv);
    std::vector<const ComparisonResult*> sorted;
    sorted.reserve(results.size());
    for (auto &r : results) sorted.push_back(&r);
    std::sort(sorted.begin(), sorted.end(), [col, asc](const ComparisonResult *a, const ComparisonResult *b) {
        const ComparisonResult *x = asc ? a : b;
        const ComparisonResult *y = asc ? b : a;
        switch (col) {
            case 1:  return x->similarity     < y->similarity;
            case 3:  return x->linesAdded     < y->linesAdded;
            case 4:  return x->linesRemoved   < y->linesRemoved;
            case 0:  return x->filePath       < y->filePath;
            default: return x->differentLines < y->differentLines;
        }
    });
    size_t limit = std::min<size_t>(500, sorted.size());
    SendMessageA(lv, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < limit; ++i) {
        const ComparisonResult &r = *sorted[i];
        LVITEMA it = {};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<LPSTR>(r.filePath.c_str());
        int idx = ListView_InsertItem(lv, &it);
        char num[32];
        snprintf(num, sizeof(num), "%.1f%%", r.similarity); ListView_SetItemText(lv, idx, 1, num);
        wsprintfA(num, "%d", r.differentLines);             ListView_SetItemText(lv, idx, 2, num);
        wsprintfA(num, "%d", r.linesAdded);                 ListView_SetItemText(lv, idx, 3, num);
        wsprintfA(num, "%d", r.linesRemoved);               ListView_SetItemText(lv, idx, 4, num);
    }
    SendMessageA(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

// ------------------------------------------------------------
// Layout
// ------------------------------------------------------------

static void layout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    const int M = 8;
    const int BTN_W = 90, BTN_H = 24, LBL_H = 18, EDIT_H = 22;

    int y = M;

    // Target file row
    SetWindowPos(GetDlgItem(hwnd, IDC_LBL_TARGET), nullptr, M, y, 120, LBL_H, SWP_NOZORDER);
    y += LBL_H + 2;
    SetWindowPos(g.hTarget, nullptr, M, y, W - 3*M - BTN_W, EDIT_H, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_BROWSE_FILE), nullptr, W - M - BTN_W, y, BTN_W, BTN_H, SWP_NOZORDER);
    y += EDIT_H + 8;

    // Folders row
    SetWindowPos(GetDlgItem(hwnd, IDC_LBL_FOLDERS), nullptr, M, y, 120, LBL_H, SWP_NOZORDER);
    y += LBL_H + 2;
    const int listH = 100;
    SetWindowPos(g.hFolders, nullptr, M, y, W - 3*M - BTN_W, listH, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_ADD_FOLDER), nullptr, W - M - BTN_W, y, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_EDIT_FOLDER), nullptr, W - M - BTN_W, y + BTN_H + 4, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(GetDlgItem(hwnd, IDC_REMOVE_FOLDER), nullptr, W - M - BTN_W, y + 2*(BTN_H + 4), BTN_W, BTN_H, SWP_NOZORDER);
    y += listH + 8;

    // Extension / Recursive row
    SetWindowPos(GetDlgItem(hwnd, IDC_LBL_EXT), nullptr, M, y + 4, 70, LBL_H, SWP_NOZORDER);
    SetWindowPos(g.hExt, nullptr, M + 72, y, 80, EDIT_H, SWP_NOZORDER);
    SetWindowPos(g.hRecursive, nullptr, M + 165, y + 2, 100, EDIT_H, SWP_NOZORDER);
    y += EDIT_H + 8;

    // Start / Cancel / Progress
    SetWindowPos(g.hStart, nullptr, M, y, BTN_W, BTN_H, SWP_NOZORDER);
    SetWindowPos(g.hCancel, nullptr, M + BTN_W + 4, y, BTN_W, BTN_H, SWP_NOZORDER);
    int progX = M + 2*(BTN_W + 4);
    SetWindowPos(g.hProgress, nullptr, progX, y + 2, W - M - progX, BTN_H - 4, SWP_NOZORDER);
    y += BTN_H + 6;

    // Results + status
    const int statusH = LBL_H + 4;
    int lvH = H - y - statusH - M;
    if (lvH < 60) lvH = 60;
    SetWindowPos(g.hResults, nullptr, M, y, W - 2*M, lvH, SWP_NOZORDER);
    y += lvH + 4;
    SetWindowPos(g.hStatus, nullptr, M, y, W - 2*M, statusH, SWP_NOZORDER);
}

// ------------------------------------------------------------
// Config persistence (INI next to the .exe)
// ------------------------------------------------------------

static std::string configPath() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string p = exe;
    auto dot = p.rfind('.');
    if (dot != std::string::npos) p = p.substr(0, dot);
    return p + ".ini";
}

static void saveConfig() {
    const std::string ini = configPath();
    const char *sec = "Settings";

    char buf[4096] = {};
    GetWindowTextA(g.hTarget, buf, sizeof(buf));
    WritePrivateProfileStringA(sec, "TargetFile", buf, ini.c_str());

    GetWindowTextA(g.hExt, buf, sizeof(buf));
    WritePrivateProfileStringA(sec, "Extension", buf, ini.c_str());

    WritePrivateProfileStringA(sec, "Recursive",
        SendMessage(g.hRecursive, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0",
        ini.c_str());

    char sortNum[4];
    wsprintfA(sortNum, "%d", g.sortCol);
    WritePrivateProfileStringA(sec, "SortColumn",    sortNum, ini.c_str());
    WritePrivateProfileStringA(sec, "SortAscending", g.sortAsc ? "1" : "0", ini.c_str());

    int count = (int)SendMessage(g.hFolders, LB_GETCOUNT, 0, 0);
    std::string folders;
    for (int i = 0; i < count; ++i) {
        int len = (int)SendMessage(g.hFolders, LB_GETTEXTLEN, i, 0);
        std::vector<char> fb(len + 1, 0);
        SendMessageA(g.hFolders, LB_GETTEXT, i, (LPARAM)fb.data());
        if (i > 0) folders += '|';
        folders += fb.data();
    }
    WritePrivateProfileStringA(sec, "Folders", folders.c_str(), ini.c_str());
}

static void loadConfig() {
    const std::string ini = configPath();
    const char *sec = "Settings";
    char buf[4096] = {};

    GetPrivateProfileStringA(sec, "TargetFile", "", buf, sizeof(buf), ini.c_str());
    SetWindowTextA(g.hTarget, buf);

    GetPrivateProfileStringA(sec, "Extension", ".htm", buf, sizeof(buf), ini.c_str());
    SetWindowTextA(g.hExt, buf);

    int rec = GetPrivateProfileIntA(sec, "Recursive", 1, ini.c_str());
    SendMessage(g.hRecursive, BM_SETCHECK, rec ? BST_CHECKED : BST_UNCHECKED, 0);

    g.sortCol = GetPrivateProfileIntA(sec, "SortColumn",    1, ini.c_str());
    g.sortAsc = GetPrivateProfileIntA(sec, "SortAscending", 0, ini.c_str()) != 0;

    GetPrivateProfileStringA(sec, "Folders", "", buf, sizeof(buf), ini.c_str());
    SendMessage(g.hFolders, LB_RESETCONTENT, 0, 0);
    std::string token;
    for (char *p = buf; ; ++p) {
        if (*p == '|' || *p == '\0') {
            if (!token.empty()) {
                SendMessageA(g.hFolders, LB_ADDSTRING, 0, (LPARAM)token.c_str());
                token.clear();
            }
            if (*p == '\0') break;
        } else {
            token += *p;
        }
    }
    updateFolderButtons();
}

// ------------------------------------------------------------
// Window procedure
// ------------------------------------------------------------

static HWND makeLabel(HWND parent, const char *text, int id) {
    return CreateWindowA("STATIC", text, WS_CHILD | WS_VISIBLE,
                         0, 0, 10, 10, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         nullptr, nullptr);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSA ncm = {}; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g.hFont = CreateFontIndirectA(&ncm.lfMessageFont);

        makeLabel(hwnd, "Target file:", IDC_LBL_TARGET);
        g.hTarget = CreateWindowA("EDIT", "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_TARGET_EDIT, nullptr, nullptr);
        CreateWindowA("BUTTON", "Browse...",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_BROWSE_FILE, nullptr, nullptr);

        makeLabel(hwnd, "Folders:", IDC_LBL_FOLDERS);
        g.hFolders = CreateWindowA("LISTBOX", "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|LBS_NOTIFY|WS_VSCROLL|WS_TABSTOP,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_FOLDERS_LIST, nullptr, nullptr);
        CreateWindowA("BUTTON", "Add...",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_ADD_FOLDER, nullptr, nullptr);
        CreateWindowA("BUTTON", "Edit...",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_DISABLED,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_EDIT_FOLDER, nullptr, nullptr);
        CreateWindowA("BUTTON", "Remove",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_DISABLED,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_REMOVE_FOLDER, nullptr, nullptr);

        makeLabel(hwnd, "Extension:", IDC_LBL_EXT);
        g.hExt = CreateWindowA("EDIT", ".htm",
            WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_EXT_EDIT, nullptr, nullptr);
        g.hRecursive = CreateWindowA("BUTTON", "Recursive",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_RECURSIVE, nullptr, nullptr);
        SendMessage(g.hRecursive, BM_SETCHECK, BST_CHECKED, 0);

        g.hStart = CreateWindowA("BUTTON", "Start",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_START, nullptr, nullptr);
        g.hCancel = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_CANCEL, nullptr, nullptr);
        EnableWindow(g.hCancel, FALSE);
        g.hProgress = CreateWindowA(PROGRESS_CLASSA, "",
            WS_CHILD|WS_VISIBLE,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_PROGRESS, nullptr, nullptr);

        g.hResults = CreateWindowA(WC_LISTVIEWA, "",
            WS_CHILD|WS_VISIBLE|WS_BORDER|LVS_REPORT|LVS_SHOWSELALWAYS|WS_TABSTOP,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_RESULTS, nullptr, nullptr);
        setupListView(g.hResults);

        g.hStatus = CreateWindowA("STATIC", "Ready.",
            WS_CHILD|WS_VISIBLE|SS_LEFTNOWORDWRAP,
            0,0,10,10, hwnd, (HMENU)(INT_PTR)IDC_STATUS, nullptr, nullptr);

        // Apply font to every child
        EnumChildWindows(hwnd, [](HWND c, LPARAM lp) -> BOOL {
            SendMessage(c, WM_SETFONT, (WPARAM)(HFONT)lp, TRUE);
            return TRUE;
        }, (LPARAM)g.hFont);

        loadConfig();
        return 0;
    }
    case WM_SIZE:
        layout(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 720;
        mmi->ptMinTrackSize.y = 520;
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_BROWSE_FILE) {
            std::string s = pickFile(hwnd);
            if (!s.empty()) SetWindowTextA(g.hTarget, s.c_str());
        }
        else if (id == IDC_ADD_FOLDER) {
            std::string s;
            if (promptForFolder(hwnd, s)) {
                int idx = (int)SendMessageA(g.hFolders, LB_ADDSTRING, 0, (LPARAM)s.c_str());
                if (idx != LB_ERR) SendMessage(g.hFolders, LB_SETCURSEL, idx, 0);
                updateFolderButtons();
            }
        }
        else if (id == IDC_EDIT_FOLDER) {
            int sel = (int)SendMessage(g.hFolders, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                int len = (int)SendMessage(g.hFolders, LB_GETTEXTLEN, sel, 0);
                std::vector<char> fb(len + 1, 0);
                SendMessageA(g.hFolders, LB_GETTEXT, sel, (LPARAM)fb.data());
                std::string s(fb.data());
                if (promptForFolder(hwnd, s)) {
                    SendMessage(g.hFolders, LB_DELETESTRING, sel, 0);
                    SendMessageA(g.hFolders, LB_INSERTSTRING, sel, (LPARAM)s.c_str());
                    SendMessage(g.hFolders, LB_SETCURSEL, sel, 0);
                    updateFolderButtons();
                }
            }
        }
        else if (id == IDC_REMOVE_FOLDER) {
            int sel = (int)SendMessage(g.hFolders, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR) {
                SendMessage(g.hFolders, LB_DELETESTRING, sel, 0);
                int count = (int)SendMessage(g.hFolders, LB_GETCOUNT, 0, 0);
                if (count > 0) {
                    int next = sel < count ? sel : count - 1;
                    SendMessage(g.hFolders, LB_SETCURSEL, next, 0);
                }
                updateFolderButtons();
            }
        }
        else if (id == IDC_FOLDERS_LIST) {
            if (code == LBN_DBLCLK) {
                SendMessageA(hwnd, WM_COMMAND,
                             MAKEWPARAM(IDC_EDIT_FOLDER, BN_CLICKED), 0);
            } else if (code == LBN_SELCHANGE) {
                updateFolderButtons();
            }
        }
        else if (id == IDC_START) {
            if (g.running) return 0;

            char buf[4096];
            GetWindowTextA(g.hTarget, buf, sizeof(buf));
            std::string target = buf;
            if (target.empty() || !std::filesystem::exists(target)) {
                MessageBoxA(hwnd, "Please choose an existing target file.", "Error", MB_ICONERROR);
                return 0;
            }

            int count = (int)SendMessage(g.hFolders, LB_GETCOUNT, 0, 0);
            if (count <= 0) {
                MessageBoxA(hwnd, "Please add at least one folder.", "Error", MB_ICONERROR);
                return 0;
            }
            std::vector<std::string> folders;
            folders.reserve(count);
            for (int i = 0; i < count; ++i) {
                int len = (int)SendMessage(g.hFolders, LB_GETTEXTLEN, i, 0);
                std::vector<char> fb(len + 1, 0);
                SendMessageA(g.hFolders, LB_GETTEXT, i, (LPARAM)fb.data());
                folders.emplace_back(fb.data());
            }

            GetWindowTextA(g.hExt, buf, sizeof(buf));
            std::string ext = buf;
            if (!ext.empty() && ext[0] != '.') ext = "." + ext;

            bool rec = SendMessage(g.hRecursive, BM_GETCHECK, 0, 0) == BST_CHECKED;

            g.allResults.clear();
            ListView_DeleteAllItems(g.hResults);
            SendMessage(g.hProgress, PBM_SETRANGE32, 0, (LPARAM)100);
            SendMessage(g.hProgress, PBM_SETPOS, 0, 0);

            g.cancelRequested = false;
            g.running = true;
            EnableWindow(g.hStart, FALSE);
            EnableWindow(g.hCancel, TRUE);
            SetWindowTextA(g.hStatus, "Starting...");

            if (g.worker.joinable()) g.worker.join();
            g.worker = std::thread(worker_run, target, std::move(folders), ext, rec);
        }
        else if (id == IDC_CANCEL) {
            g.cancelRequested = true;
            SetWindowTextA(g.hStatus, "Cancelling...");
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr->idFrom != IDC_RESULTS) break;
        switch (hdr->code) {
        case LVN_COLUMNCLICK: {
            auto *nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
            if (nmlv->iSubItem == g.sortCol)
                g.sortAsc = !g.sortAsc;
            else { g.sortCol = nmlv->iSubItem; g.sortAsc = true; }
            populateResults(g.hResults, g.allResults, g.sortCol, g.sortAsc);
            updateSortArrow(g.hResults, g.sortCol, g.sortAsc);
            break;
        }
        case NM_DBLCLK: {
            int sel = ListView_GetNextItem(g.hResults, -1, LVNI_SELECTED);
            if (sel < 0) break;
            char buf[MAX_PATH] = {};
            ListView_GetItemText(g.hResults, sel, 0, buf, MAX_PATH);
            ShellExecuteA(nullptr, "open", buf, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case NM_RCLICK: {
            int sel = ListView_GetNextItem(g.hResults, -1, LVNI_SELECTED);
            if (sel < 0) break;
            char buf[MAX_PATH] = {};
            ListView_GetItemText(g.hResults, sel, 0, buf, MAX_PATH);
            size_t len = strlen(buf) + 1;
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
            if (!hMem) break;
            memcpy(GlobalLock(hMem), buf, len);
            GlobalUnlock(hMem);
            if (OpenClipboard(g.hMain)) {
                EmptyClipboard();
                SetClipboardData(CF_TEXT, hMem);
                CloseClipboard();
                std::string msg = "Copied to clipboard: ";
                msg += buf;
                SetWindowTextA(g.hStatus, msg.c_str());
            } else {
                GlobalFree(hMem);
            }
            break;
        }
        }
        return 0;
    }
    case WM_APP_PROGRESS: {
        int done  = (int)wParam;
        int total = (int)lParam;
        if (total < 1) total = 1;
        SendMessage(g.hProgress, PBM_SETRANGE32, 0, (LPARAM)total);
        SendMessage(g.hProgress, PBM_SETPOS, (WPARAM)done, 0);
        if (done > 0) {
            std::ostringstream oss;
            oss << "Comparing... " << done << " / " << total;
            std::string s = oss.str();
            SetWindowTextA(g.hStatus, s.c_str());
        }
        return 0;
    }
    case WM_APP_STATUS: {
        char *s = reinterpret_cast<char*>(lParam);
        if (s) { SetWindowTextA(g.hStatus, s); delete[] s; }
        return 0;
    }
    case WM_APP_DONE: {
        auto *payload = reinterpret_cast<std::vector<ComparisonResult>*>(lParam);
        g.allResults = std::move(*payload);
        delete payload;
        if (g.worker.joinable()) g.worker.join();
        g.running = false;
        EnableWindow(g.hStart, TRUE);
        EnableWindow(g.hCancel, FALSE);
        populateResults(g.hResults, g.allResults, g.sortCol, g.sortAsc);
        updateSortArrow(g.hResults, g.sortCol, g.sortAsc);
        // Fill the bar
        LRESULT high = SendMessage(g.hProgress, PBM_GETRANGE, FALSE, 0);
        SendMessage(g.hProgress, PBM_SETPOS, (WPARAM)(int)high, 0);
        return 0;
    }
    case WM_CLOSE:
        if (g.running) {
            if (MessageBoxA(hwnd, "A scan is running. Quit anyway?",
                            "Confirm", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            g.cancelRequested = true;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g.cancelRequested = true;
        if (g.worker.joinable()) g.worker.join();
        saveConfig();
        if (g.hFont) { DeleteObject(g.hFont); g.hFont = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ------------------------------------------------------------
// Entry point
// ------------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CloneFinderMain";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "CloneFinder",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720,
                              nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    g.hMain = hwnd;
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    CoUninitialize();
    return (int)msg.wParam;
}
