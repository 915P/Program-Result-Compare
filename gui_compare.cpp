// ============================================================================
//  gui_compare.cpp  ---  程序输出自动对比测试工具（图形界面版）
//
//  作用：
//    选择一个“标准答案”DEMO 程序、一个“自己写的”程序，以及一个测试数据集
//    （数据集中每一行 = 一组测试数据）。程序会逐行把数据喂给两个程序，
//    收集它们的输出，并调用 txt_compare.exe 进行比对。
//    每完成一组就立刻把结果显示到界面上；一旦发现不同就自动暂停，
//    等你点击“继续”后再执行下一组。
//
//  编译（两种编译器都可以）：
//    MinGW/GCC : 见 build-gcc.bat
//    MSVC      : 见 build-msvc.bat
//
//  依赖：txt_compare.exe（第三方工具，不随本项目分发）。
//    编译时放进 vendor\ 会内嵌；否则运行时找 exe 同目录下的同名文件。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// ---------------------------------------------------------------- 控件 ID
#define IDR_TXTCOMPARE 101          // 内嵌的 txt_compare.exe（RCDATA）

enum {
    IDC_EDIT_DEMO = 1001,
    IDC_BTN_DEMO,
    IDC_EDIT_MINE,
    IDC_BTN_MINE,
    IDC_EDIT_DATA,
    IDC_BTN_DATA,
    IDC_COMBO_TRIM,
    IDC_EDIT_SKIP,
    IDC_CHK_BLANK,
    IDC_CHK_LF,
    IDC_EDIT_TIMEOUT,
    IDC_BTN_START,
    IDC_BTN_PAUSE,
    IDC_BTN_STOP,
    IDC_BTN_CLEAR,
    IDC_BTN_OPENOUT,
    IDC_LOG,
    IDC_PROGRESS,
    IDC_STATUS
};

// ------------------------------------------------------------ 自定义消息
#define WM_APP_LOG      (WM_APP + 1)   // LPARAM = std::wstring*
#define WM_APP_PROGRESS (WM_APP + 2)   // WPARAM = 当前组号, LPARAM = 总组数
#define WM_APP_PAUSED   (WM_APP + 3)   // WPARAM = 当前组号, LPARAM = 总组数
#define WM_APP_DONE     (WM_APP + 4)   // LPARAM = DoneInfo*

struct DoneInfo {
    int total = 0;
    int passed = 0;
    int failed = 0;
    bool stopped = false;
    std::wstring outDir;
};

struct PauseInfo {
    int cur = 0;
    int total = 0;
    std::wstring reason;
};

struct Opts {
    std::wstring trim = L"none";
    int  lineskip = 0;
    bool ignoreBlank = false;
    bool ignoreLF = false;
    DWORD timeoutMs = 3000;
};

// 把 trim 的取值翻译成界面上的中文
static const wchar_t* TrimText(const std::wstring& t) {
    if (t == L"left")  return L"忽略行首空格";
    if (t == L"right") return L"忽略行尾空格";
    if (t == L"all")   return L"忽略首尾空格";
    return L"不忽略";
}

// ---------------------------------------------------------------- 全局变量
static HINSTANCE g_hInst = nullptr;
static HWND g_hMain = nullptr;
static HWND g_hEditDemo = nullptr, g_hEditMine = nullptr, g_hEditData = nullptr;
static HWND g_hBtnDemo = nullptr, g_hBtnMine = nullptr, g_hBtnData = nullptr;
static HWND g_hComboTrim = nullptr, g_hEditSkip = nullptr, g_hChkBlank = nullptr, g_hChkLF = nullptr;
static HWND g_hEditTimeout = nullptr;
static HWND g_hBtnStart = nullptr, g_hBtnPause = nullptr, g_hBtnStop = nullptr;
static HWND g_hBtnClear = nullptr, g_hBtnOpenOut = nullptr;
static HWND g_hLog = nullptr, g_hProgress = nullptr, g_hStatus = nullptr;
static HWND g_hLabelDemo = nullptr, g_hLabelMine = nullptr, g_hLabelData = nullptr;
static HWND g_hLabelOpt = nullptr, g_hLabelSkip = nullptr, g_hLabelSkipSuffix = nullptr;
static HWND g_hLabelTimeout = nullptr, g_hLabelTimeoutSuffix = nullptr;

static HFONT g_fontUI = nullptr, g_fontMono = nullptr;
static int   g_dpi = 96;

static std::wstring g_iniPath;
static std::wstring g_outDir;          // 本轮输出保存目录
static std::wstring g_toolDir;         // 临时释放目录（退出时删除）
static std::wstring g_toolPath;        // 临时释放出来的 txt_compare.exe
static bool         g_toolExternal = false;  // true = 用的是 exe 旁边的文件，不要删

static std::thread g_worker;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_pauseReq{false};
static std::atomic<bool> g_resumeReq{false};
static bool g_uiPaused = false;

static HANDLE g_hResume = nullptr;
static std::mutex g_childMutex;
static HANDLE g_child = nullptr;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------- 小工具
static std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    return (std::filesystem::path(a) / b).wstring();
}

static std::wstring ParentDir(const std::wstring& p) {
    std::filesystem::path pp(p);
    std::filesystem::path parent = pp.parent_path();
    if (parent.empty()) return std::filesystem::current_path().wstring();
    return parent.wstring();
}

static std::wstring QuoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

static std::wstring LastErrText() {
    DWORD e = GetLastError();
    LPWSTR msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ')) s.pop_back();
    return s + L"（错误码 " + std::to_wstring(e) + L"）";
}

static bool ReadAllBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return false; }
    out.resize((size_t)sz.QuadPart);
    size_t total = 0;
    while (total < out.size()) {
        DWORD got = 0;
        DWORD want = (DWORD)(out.size() - total);
        if (!ReadFile(h, out.data() + total, want, &got, nullptr) || got == 0) break;
        total += got;
    }
    out.resize(total);
    CloseHandle(h);
    return true;
}

static bool WriteAllBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == data.size();
}

// 把字节流转换为宽字符串：优先按 UTF-8 解释，失败则按系统 ANSI（简体中文为 GBK）
static std::wstring DecodeBytes(const std::string& s) {
    if (s.empty()) return L"";
    size_t off = 0;
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        off = 3;
    }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data() + off,
                                (int)(s.size() - off), nullptr, 0);
    if (n > 0) {
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data() + off, (int)(s.size() - off), &w[0], n);
        return w;
    }
    n = MultiByteToWideChar(CP_ACP, 0, s.data() + off, (int)(s.size() - off), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data() + off, (int)(s.size() - off), &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

static std::wstring Truncate(const std::wstring& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen) + L"...";
}

// 把 \n 统一成 \r\n，便于在 EDIT 控件中显示
static std::wstring NormalizeNewlines(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + s.size() / 16);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\r') {
            out.push_back(L'\r');
            if (i + 1 < s.size() && s[i + 1] == L'\n') ++i;
            out.push_back(L'\n');
        } else if (s[i] == L'\n') {
            out += L"\r\n";
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::wstring Indent(const std::wstring& s) {
    std::wstring out = L"    ";
    for (size_t i = 0; i < s.size(); ++i) {
        out.push_back(s[i]);
        if (s[i] == L'\n') out += L"    ";
    }
    // 去掉末尾多出来的缩进
    while (out.size() >= 4 && out.compare(out.size() - 4, 4, L"    ") == 0) out.erase(out.size() - 4);
    return out;
}

static std::wstring Pad2(size_t n) {
    std::wstring s = std::to_wstring(n);
    if (s.size() < 2) s = L"0" + s;
    return s;
}

// ---------------------------------------------------------------- 进程执行
static bool RunProcess(const std::wstring& cmdLine, const std::wstring& workDir,
                       HANDLE hIn, HANDLE hOut, DWORD timeoutMs,
                       DWORD& exitCode, bool& timedOut) {
    std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hIn;
    si.hStdOutput = hOut;
    si.hStdError = hOut;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_childMutex);
        g_child = pi.hProcess;
    }

    DWORD wr = WaitForSingleObject(pi.hProcess, timeoutMs);
    timedOut = (wr == WAIT_TIMEOUT);
    if (timedOut) TerminateProcess(pi.hProcess, 1);

    exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    {
        std::lock_guard<std::mutex> lk(g_childMutex);
        g_child = nullptr;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void TerminateChild() {
    std::lock_guard<std::mutex> lk(g_childMutex);
    if (g_child) TerminateProcess(g_child, 1);
}

// 用指定输入运行一个程序，输出写入文件
static bool RunProgram(const std::wstring& exe, const std::string& input,
                       const std::wstring& inPath, const std::wstring& outPath,
                       DWORD timeoutMs, std::string& outBytes, DWORD& exitCode,
                       bool& timedOut, std::wstring& err) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    std::string data = input;
    data += "\r\n";

    HANDLE hIn = CreateFileW(inPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hIn == INVALID_HANDLE_VALUE) {
        err = L"无法创建输入文件：" + inPath + L" " + LastErrText();
        return false;
    }
    DWORD written = 0;
    WriteFile(hIn, data.data(), (DWORD)data.size(), &written, nullptr);
    SetFilePointer(hIn, 0, nullptr, FILE_BEGIN);

    HANDLE hOut = CreateFileW(outPath.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        CloseHandle(hIn);
        err = L"无法创建输出文件：" + outPath + L" " + LastErrText();
        return false;
    }

    bool started = RunProcess(QuoteArg(exe), ParentDir(exe), hIn, hOut, timeoutMs, exitCode, timedOut);

    CloseHandle(hIn);
    CloseHandle(hOut);

    if (!started) {
        err = L"无法启动程序：" + exe + L" " + LastErrText();
        return false;
    }

    ReadAllBytes(outPath, outBytes);
    return true;
}

// 调用 txt_compare.exe 比较两个文件
static bool RunToolCompare(const std::wstring& tool, const std::wstring& file1,
                           const std::wstring& file2, const Opts& opt,
                           const std::wstring& workDir, std::wstring& diffText, bool& same) {
    std::wstring cmd = QuoteArg(tool) + L" --file1 " + QuoteArg(file1) + L" --file2 " +
                       QuoteArg(file2) + L" --display detailed";
    if (opt.trim != L"none" && !opt.trim.empty()) cmd += L" --trim " + opt.trim;
    if (opt.lineskip > 0) cmd += L" --lineskip " + std::to_wstring(opt.lineskip);
    if (opt.ignoreBlank) cmd += L" --ignore_blank";
    if (opt.ignoreLF) cmd += L" --ignore_linefeed";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    std::wstring inPath = JoinPath(workDir, L"nul_in.txt");
    std::wstring outPath = JoinPath(workDir, L"compare.txt");

    HANDLE hIn = CreateFileW(inPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hIn == INVALID_HANDLE_VALUE) return false;
    SetFilePointer(hIn, 0, nullptr, FILE_BEGIN);

    HANDLE hOut = CreateFileW(outPath.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        CloseHandle(hIn);
        return false;
    }

    DWORD code = 0;
    bool timedOut = false;
    bool started = RunProcess(cmd, ParentDir(tool), hIn, hOut, 20000, code, timedOut);

    CloseHandle(hIn);
    CloseHandle(hOut);

    if (!started) return false;

    std::string bytes;
    ReadAllBytes(outPath, bytes);
    diffText = DecodeBytes(bytes);

    // 去掉 txt_compare 每次都会打印的固定“阅读提示”，让日志更紧凑
    size_t tip = diffText.find(L"阅读提示");
    if (tip != std::wstring::npos) {
        diffText.erase(tip);
        while (!diffText.empty() &&
               (diffText.back() == L'\r' || diffText.back() == L'\n' ||
                diffText.back() == L' ' || diffText.back() == L'\t')) {
            diffText.pop_back();
        }
        diffText += L"\r\n";
    }

    same = (code == 0);
    return true;
}

// ---------------------------------------------------------------- 测试数据
static bool LoadDataset(const std::wstring& path, std::vector<std::string>& lines,
                        std::wstring& err) {
    std::string raw;
    if (!ReadAllBytes(path, raw)) {
        err = L"无法读取文件（文件不存在或没有权限）";
        return false;
    }

    // UTF-16 BOM 转成 UTF-8
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        std::wstring w(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / 2);
        raw = WideToUtf8(w);
    } else if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF) {
        std::wstring w;
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {
            wchar_t c = (wchar_t)(((unsigned char)raw[i] << 8) | (unsigned char)raw[i + 1]);
            w.push_back(c);
        }
        raw = WideToUtf8(w);
    }

    size_t pos = 0;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF) {
        pos = 3;
    }

    std::string cur;
    auto flush = [&]() {
        std::string t = cur;
        cur.clear();
        if (!t.empty() && t.back() == '\r') t.pop_back();

        size_t b = t.find_first_not_of(" \t");
        if (b == std::string::npos) return;             // 空行
        size_t e = t.find_last_not_of(" \t");
        std::string core = t.substr(b, e - b + 1);
        if (!core.empty() && core[0] == '#') return;     // 注释行
        lines.push_back(t);
    };

    for (size_t i = pos; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\n') {
            flush();
        } else if (c != '\0') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) flush();

    return true;
}

// ---------------------------------------------------------------- 后台线程
static void PostLog(const std::wstring& text) {
    if (!g_hMain) return;
    auto* p = new std::wstring(NormalizeNewlines(text));
    if (!PostMessageW(g_hMain, WM_APP_LOG, 0, (LPARAM)p)) delete p;
}

static void WaitWhilePaused() {
    while (!g_stop && g_pauseReq && !g_resumeReq) {
        WaitForSingleObject(g_hResume, 100);
    }
    if (g_resumeReq) {
        g_resumeReq = false;
        g_pauseReq = false;
    }
}

static void WorkerThread(std::wstring demo, std::wstring mine, std::wstring data,
                         std::wstring tool, Opts opt) {
    auto postDone = [](DoneInfo* di) {
        if (!PostMessageW(g_hMain, WM_APP_DONE, 0, (LPARAM)di)) delete di;
    };

    std::vector<std::string> lines;
    std::wstring err;
    if (!LoadDataset(data, lines, err)) {
        PostLog(L"[错误] 读取测试数据失败：" + err + L"\r\n");
        postDone(new DoneInfo{0, 0, 0, true, L""});
        return;
    }
    if (lines.empty()) {
        PostLog(L"[错误] 测试数据为空（每行一组数据，空行和 # 开头的行会被忽略）。\r\n");
        postDone(new DoneInfo{0, 0, 0, true, L""});
        return;
    }

    wchar_t tempBuf[MAX_PATH]{};
    DWORD tempLen = GetTempPathW(MAX_PATH, tempBuf);
    std::wstring root = JoinPath(std::wstring(tempBuf, tempLen), L"gui_compare_out");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    g_outDir = root;

    PostLog(L"共 " + std::to_wstring(lines.size()) + L" 组测试数据。\r\n");
    PostLog(L"输出文件保存在：" + root + L"\r\n");
    PostLog(L"------------------------------------------------------------\r\n");

    int passed = 0, failed = 0;
    bool stopped = false;
    ULONGLONG tStart = GetTickCount64();

    // 暂停并等待用户点击“继续”/“停止”
    auto pauseFor = [&](size_t idx, const std::wstring& reason) {
        g_pauseReq = true;
        g_resumeReq = false;
        auto* pi = new PauseInfo{(int)(idx + 1), (int)lines.size(), reason};
        if (!PostMessageW(g_hMain, WM_APP_PAUSED, 0, (LPARAM)pi)) delete pi;
        WaitWhilePaused();
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        if (g_stop) { stopped = true; break; }
        WaitWhilePaused();
        if (g_stop) { stopped = true; break; }

        PostMessageW(g_hMain, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)lines.size());

        std::wstring caseName = L"case_" + Pad2(i + 1);
        std::wstring caseDir = JoinPath(root, caseName);
        std::filesystem::create_directories(caseDir, ec);

        std::wstring inputShow = Truncate(DecodeBytes(lines[i]), 70);
        std::wstring head = L"[" + Pad2(i + 1) + L"/" + std::to_wstring(lines.size()) +
                            L"] 输入: " + inputShow;

        std::string demoOut, mineOut;
        DWORD demoCode = 0, mineCode = 0;
        bool demoTmo = false, mineTmo = false;
        std::wstring eDemo, eMine;

        ULONGLONG t0 = GetTickCount64();
        bool okDemo = RunProgram(demo, lines[i], JoinPath(caseDir, L"in_demo.txt"),
                                 JoinPath(caseDir, L"out_demo.txt"), opt.timeoutMs,
                                 demoOut, demoCode, demoTmo, eDemo);
        bool okMine = RunProgram(mine, lines[i], JoinPath(caseDir, L"in_mine.txt"),
                                 JoinPath(caseDir, L"out_mine.txt"), opt.timeoutMs,
                                 mineOut, mineCode, mineTmo, eMine);
        ULONGLONG elapsed = GetTickCount64() - t0;

        bool caseFailed = false;
        std::wstring pauseReason;

        if (!okDemo || !okMine) {
            ++failed;
            caseFailed = true;
            pauseReason = L"程序无法启动";
            PostLog(head + L"\r\n    [错误] " + (okDemo ? eMine : eDemo) + L"\r\n");
        } else if (demoTmo || mineTmo) {
            ++failed;
            caseFailed = true;
            pauseReason = L"程序运行超时";
            std::wstring who = demoTmo ? L"DEMO 程序" : L"我的程序";
            PostLog(head + L"\r\n    [超时] " + who + L" 超过 " +
                    std::to_wstring(opt.timeoutMs) + L" ms 没有结束，已强制终止。\r\n");
        } else {
            std::wstring diff;
            bool same = false;
            bool cmpOk = RunToolCompare(tool, JoinPath(caseDir, L"out_demo.txt"),
                                        JoinPath(caseDir, L"out_mine.txt"), opt,
                                        caseDir, diff, same);
            if (!cmpOk) {
                ++failed;
                caseFailed = true;
                pauseReason = L"txt_compare 运行失败";
                PostLog(head + L"\r\n    [错误] 无法运行 txt_compare，请检查路径。\r\n");
            } else if (same) {
                ++passed;
                PostLog(head + L"  -> 通过（" + std::to_wstring(elapsed) + L" ms）\r\n");
            } else {
                ++failed;
                caseFailed = true;
                pauseReason = L"输出不同";
                PostLog(head + L"  -> 不同（" + std::to_wstring(elapsed) + L" ms）\r\n");
                PostLog(Indent(diff) + L"\r\n");
            }
        }

        if (caseFailed) {
            pauseFor(i, pauseReason);
            if (g_stop) { stopped = true; break; }
        }

        PostLog(L"------------------------------------------------------------\r\n");
    }

    ULONGLONG total = GetTickCount64() - tStart;

    std::wstring summary = L"\r\n=== 测试结束 ===\r\n";
    summary += L"共 " + std::to_wstring(lines.size()) + L" 组：通过 " +
               std::to_wstring(passed) + L" 组，不同/出错 " + std::to_wstring(failed) + L" 组";
    if (stopped) summary += L"（已中止）";
    summary += L"\r\n总用时 " + std::to_wstring(total) + L" ms\r\n";

    PostLog(summary);

    auto* di = new DoneInfo;
    di->total = (int)lines.size();
    di->passed = passed;
    di->failed = failed;
    di->stopped = stopped;
    di->outDir = root;
    postDone(di);
}

// ---------------------------------------------------------------- 界面辅助
static void AppendLog(const std::wstring& text) {
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void SetStatus(const std::wstring& text) {
    SetWindowTextW(g_hStatus, text.c_str());
}

static void UpdateButtons(bool running, bool paused) {
    EnableWindow(g_hBtnStart, !running);
    EnableWindow(g_hBtnPause, running);
    EnableWindow(g_hBtnStop, running);
    SetWindowTextW(g_hBtnPause, paused ? L"继续" : L"暂停");
    g_uiPaused = paused;
}

static bool PickFile(HWND owner, const wchar_t* filter, const wchar_t* title,
                     std::wstring& out) {
    wchar_t buf[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = buf;
    return true;
}

static std::wstring FindFileInDir(const std::wstring& dir, const std::wstring& pattern) {
    if (dir.empty()) return L"";
    WIN32_FIND_DATAW fd{};
    std::wstring spec = JoinPath(dir, pattern);
    HANDLE h = FindFirstFileW(spec.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring res = JoinPath(dir, fd.cFileName);
    FindClose(h);
    return res;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return ParentDir(buf);
}

// ------------------------------------------------------------ 内嵌 txt_compare
// 把编译时打进 exe 的 txt_compare.exe 释放到临时目录，用完即删。
// 目录：%TEMP%\gui_compare_tmp\<本进程 PID>\txt_compare.exe
static bool ProcessAlive(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

// 清理以前被强制结束（或崩溃）时残留的临时目录
static void SweepStaleToolDirs() {
    wchar_t tmp[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0) return;
    std::wstring base = JoinPath(std::wstring(tmp, n), L"gui_compare_tmp");

    std::error_code ec;
    if (!std::filesystem::exists(base, ec)) return;

    for (std::filesystem::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::wstring name = it->path().filename().wstring();
        wchar_t* stop = nullptr;
        unsigned long pid = wcstoul(name.c_str(), &stop, 10);
        if (pid == 0 || !stop || *stop != L'\0') continue;
        if (!ProcessAlive((DWORD)pid)) {
            std::error_code ec2;
            std::filesystem::remove_all(it->path(), ec2);
        }
    }
}

static std::wstring EnsureEmbeddedTool() {
    if (!g_toolPath.empty() && std::filesystem::exists(g_toolPath)) return g_toolPath;

    HRSRC hr = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_TXTCOMPARE), RT_RCDATA);
    if (!hr) {
        // 没有内嵌资源（编译时没有 vendor\txt_compare.exe）：
        // 先找 gui_compare.exe 同目录，再找上次用户手动选过的路径。
        std::wstring beside = JoinPath(ExeDir(), L"txt_compare.exe");
        std::error_code ec;
        if (std::filesystem::exists(beside, ec)) {
            g_toolPath = beside;
            g_toolExternal = true;
            return g_toolPath;
        }

        wchar_t saved[MAX_PATH * 2]{};
        GetPrivateProfileStringW(L"Settings", L"Tool", L"", saved, MAX_PATH * 2, g_iniPath.c_str());
        if (saved[0] != L'\0') {
            std::error_code ec2;
            if (std::filesystem::exists(saved, ec2)) {
                g_toolPath = saved;
                g_toolExternal = true;
            }
        }
        return g_toolPath;
    }
    DWORD size = SizeofResource(g_hInst, hr);
    HGLOBAL hg = LoadResource(g_hInst, hr);
    if (!hg || size == 0) return L"";
    const void* data = LockResource(hg);
    if (!data) return L"";

    wchar_t tmp[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0) return L"";

    g_toolDir = JoinPath(JoinPath(std::wstring(tmp, n), L"gui_compare_tmp"),
                         std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(g_toolDir, ec);

    std::wstring path = JoinPath(g_toolDir, L"txt_compare.exe");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        g_toolDir.clear();
        return L"";
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, nullptr);
    CloseHandle(h);

    if (!ok || written != size) {
        DeleteFileW(path.c_str());
        g_toolDir.clear();
        return L"";
    }

    g_toolPath = path;
    g_toolExternal = false;
    return g_toolPath;
}

// 退出时删除释放出来的 txt_compare，不在用户电脑上留下文件
static void CleanupEmbeddedTool() {
    // 外部文件是用户自己的，不能删
    if (!g_toolPath.empty() && !g_toolExternal) {
        DeleteFileW(g_toolPath.c_str());
    }
    if (!g_toolDir.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(g_toolDir, ec);
        // 根目录空了就一并删掉，不留空文件夹
        std::filesystem::remove(ParentDir(g_toolDir), ec);
    }
    g_toolPath.clear();
    g_toolDir.clear();
    g_toolExternal = false;
}

// 编译时没有内嵌、exe 旁边也没有、上次选的也失效了：
// 请用户自己指定一次 txt_compare.exe，并记到 ini 里，下次直接用。
static std::wstring AskUserForTool(HWND owner) {
    MessageBoxW(owner,
                L"没有找到 txt_compare.exe。\n\n"
                L"这个版本在编译时没有内置它，需要你自己指定一次。\n"
                L"指定后会记在 gui_compare.ini 里，下次不用再选。\n\n"
                L"（也可以把 txt_compare.exe 直接放到 gui_compare.exe 旁边。）",
                L"需要 txt_compare", MB_OK | MB_ICONINFORMATION);

    std::wstring picked;
    if (!PickFile(owner, L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0",
                  L"请选择 txt_compare.exe", picked)) {
        return L"";
    }

    g_toolPath = picked;
    g_toolExternal = true;
    WritePrivateProfileStringW(L"Settings", L"Tool", picked.c_str(), g_iniPath.c_str());
    return picked;
}

// ---------------------------------------------------------------- 配置读写
static void LoadSettings() {
    auto rd = [](const wchar_t* key, const wchar_t* def) {
        wchar_t buf[2048]{};
        GetPrivateProfileStringW(L"Settings", key, def, buf, 2048, g_iniPath.c_str());
        return std::wstring(buf);
    };
    SetWindowTextW(g_hEditDemo, rd(L"Demo", L"").c_str());
    SetWindowTextW(g_hEditMine, rd(L"Mine", L"").c_str());
    SetWindowTextW(g_hEditData, rd(L"Data", L"").c_str());

    std::wstring trim = rd(L"Trim", L"none");
    int sel = 0;
    if (trim == L"left") sel = 1;
    else if (trim == L"right") sel = 2;
    else if (trim == L"all") sel = 3;
    SendMessageW(g_hComboTrim, CB_SETCURSEL, sel, 0);

    SetWindowTextW(g_hEditSkip, rd(L"LineSkip", L"0").c_str());
    SetWindowTextW(g_hEditTimeout, rd(L"Timeout", L"3000").c_str());
    CheckDlgButton(g_hMain, IDC_CHK_BLANK,
                   rd(L"IgnoreBlank", L"0") == L"1" ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_hMain, IDC_CHK_LF,
                   rd(L"IgnoreLF", L"0") == L"1" ? BST_CHECKED : BST_UNCHECKED);
}

static void SaveSettings() {
    auto wr = [](const wchar_t* key, const std::wstring& val) {
        WritePrivateProfileStringW(L"Settings", key, val.c_str(), g_iniPath.c_str());
    };
    wr(L"Demo", GetText(g_hEditDemo));
    wr(L"Mine", GetText(g_hEditMine));
    wr(L"Data", GetText(g_hEditData));

    int sel = (int)SendMessageW(g_hComboTrim, CB_GETCURSEL, 0, 0);
    const wchar_t* trims[] = {L"none", L"left", L"right", L"all"};
    wr(L"Trim", (sel >= 0 && sel < 4) ? trims[sel] : L"none");

    wr(L"LineSkip", GetText(g_hEditSkip));
    wr(L"Timeout", GetText(g_hEditTimeout));
    wr(L"IgnoreBlank", IsDlgButtonChecked(g_hMain, IDC_CHK_BLANK) == BST_CHECKED ? L"1" : L"0");
    wr(L"IgnoreLF", IsDlgButtonChecked(g_hMain, IDC_CHK_LF) == BST_CHECKED ? L"1" : L"0");
}

// ---------------------------------------------------------------- 布局
static void Layout(HWND hWnd) {
    RECT rc{};
    GetClientRect(hWnd, &rc);
    int W = rc.right, H = rc.bottom;
    const int M = S(14);
    const int labelW = S(96);
    const int editX = M + labelW + S(8);
    const int btnW = S(84);
    const int btnX = W - M - btnW;
    const int editW = btnX - S(8) - editX;
    const int rowH = S(24);

    auto place = [&](HWND h, int x, int y, int w, int hh) {
        MoveWindow(h, x, y, w, hh, TRUE);
    };

    int y = S(12);
    place(g_hLabelDemo, M, y + S(4), labelW, rowH);
    place(g_hEditDemo, editX, y, editW, rowH);
    place(g_hBtnDemo, btnX, y, btnW, rowH);

    y += S(32);
    place(g_hLabelMine, M, y + S(4), labelW, rowH);
    place(g_hEditMine, editX, y, editW, rowH);
    place(g_hBtnMine, btnX, y, btnW, rowH);

    y += S(32);
    place(g_hLabelData, M, y + S(4), labelW, rowH);
    place(g_hEditData, editX, y, editW, rowH);
    place(g_hBtnData, btnX, y, btnW, rowH);

    // 选项行
    y += S(38);
    int x = M;
    place(g_hLabelOpt, x, y + S(4), S(70), rowH);
    x += S(74);
    place(g_hComboTrim, x, y, S(124), S(200));
    x += S(132);
    place(g_hLabelSkip, x, y + S(4), S(48), rowH);
    x += S(50);
    place(g_hEditSkip, x, y, S(44), rowH);
    x += S(50);
    place(g_hLabelSkipSuffix, x, y + S(4), S(18), rowH);
    x += S(22);
    place(g_hLabelTimeout, x, y + S(4), S(38), rowH);
    x += S(40);
    place(g_hEditTimeout, x, y, S(56), rowH);
    x += S(60);
    place(g_hLabelTimeoutSuffix, x, y + S(4), S(24), rowH);
    x += S(30);
    place(g_hChkBlank, x, y + S(2), S(100), rowH);
    x += S(104);
    place(g_hChkLF, x, y + S(2), S(120), rowH);

    // 按钮行
    y += S(36);
    int bx = M;
    place(g_hBtnStart, bx, y, S(110), S(32)); bx += S(118);
    place(g_hBtnPause, bx, y, S(96), S(32));  bx += S(104);
    place(g_hBtnStop, bx, y, S(88), S(32));   bx += S(96);
    place(g_hBtnClear, bx, y, S(104), S(32)); bx += S(112);
    place(g_hBtnOpenOut, bx, y, S(110), S(32));

    place(g_hProgress, W - M - S(210), y + S(7), S(210), S(18));

    // 日志
    int logY = y + S(44);
    int logH = H - logY - S(40);
    if (logH < S(80)) logH = S(80);
    place(g_hLog, M, logY, W - 2 * M, logH);

    place(g_hStatus, M, H - S(28), W - 2 * M, S(20));
}

// ---------------------------------------------------------------- 窗口过程
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hMain = hWnd;

        auto makeStatic = [&](const wchar_t* text) {
            return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 0, 0, hWnd, nullptr, g_hInst, nullptr);
        };
        auto makeEdit = [&](int id, DWORD extra = 0) {
            return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | extra,
                                   0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        };
        auto makeButton = [&](int id, const wchar_t* text) {
            return CreateWindowExW(0, L"BUTTON", text,
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                   0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)id, g_hInst, nullptr);
        };

        g_hLabelDemo = makeStatic(L"DEMO 程序：");
        g_hEditDemo = makeEdit(IDC_EDIT_DEMO, ES_READONLY);
        g_hBtnDemo = makeButton(IDC_BTN_DEMO, L"浏览...");

        g_hLabelMine = makeStatic(L"我的程序：");
        g_hEditMine = makeEdit(IDC_EDIT_MINE, ES_READONLY);
        g_hBtnMine = makeButton(IDC_BTN_MINE, L"浏览...");

        g_hLabelData = makeStatic(L"测试数据：");
        g_hEditData = makeEdit(IDC_EDIT_DATA, ES_READONLY);
        g_hBtnData = makeButton(IDC_BTN_DATA, L"浏览...");

        g_hLabelOpt = makeStatic(L"忽略空格：");
        g_hComboTrim = CreateWindowExW(0, L"COMBOBOX", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                       WS_VSCROLL,
                                       0, 0, 0, 0, hWnd, (HMENU)IDC_COMBO_TRIM, g_hInst, nullptr);
        SendMessageW(g_hComboTrim, CB_ADDSTRING, 0, (LPARAM)L"不忽略");
        SendMessageW(g_hComboTrim, CB_ADDSTRING, 0, (LPARAM)L"忽略行首空格");
        SendMessageW(g_hComboTrim, CB_ADDSTRING, 0, (LPARAM)L"忽略行尾空格");
        SendMessageW(g_hComboTrim, CB_ADDSTRING, 0, (LPARAM)L"忽略首尾空格");
        SendMessageW(g_hComboTrim, CB_SETCURSEL, 0, 0);

        g_hLabelSkip = makeStatic(L"跳过前");
        g_hEditSkip = makeEdit(IDC_EDIT_SKIP);
        g_hLabelSkipSuffix = makeStatic(L"行");
        g_hLabelTimeout = makeStatic(L"超时");
        g_hEditTimeout = makeEdit(IDC_EDIT_TIMEOUT);
        g_hLabelTimeoutSuffix = makeStatic(L"ms");

        g_hChkBlank = CreateWindowExW(0, L"BUTTON", L"忽略空行",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                      0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_BLANK, g_hInst, nullptr);
        g_hChkLF = CreateWindowExW(0, L"BUTTON", L"忽略换行符差异",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_LF, g_hInst, nullptr);

        g_hBtnStart = makeButton(IDC_BTN_START, L"开始测试");
        g_hBtnPause = makeButton(IDC_BTN_PAUSE, L"暂停");
        g_hBtnStop = makeButton(IDC_BTN_STOP, L"停止");
        g_hBtnClear = makeButton(IDC_BTN_CLEAR, L"清空日志");
        g_hBtnOpenOut = makeButton(IDC_BTN_OPENOUT, L"打开输出目录");

        g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                      WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd,
                                      (HMENU)IDC_PROGRESS, g_hInst, nullptr);
        SendMessageW(g_hProgress, PBM_SETRANGE32, 0, 100);

        g_hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                 ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                 0, 0, 0, 0, hWnd, (HMENU)IDC_LOG, g_hInst, nullptr);
        SendMessageW(g_hLog, EM_SETLIMITTEXT, (WPARAM)0x7FFFFFFE, 0);

        g_hStatus = CreateWindowExW(0, L"STATIC", L"就绪", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    0, 0, 0, 0, hWnd, (HMENU)IDC_STATUS, g_hInst, nullptr);

        // 字体
        g_fontUI = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_fontMono = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

        for (HWND h : {g_hLabelDemo, g_hEditDemo, g_hBtnDemo, g_hLabelMine, g_hEditMine,
                       g_hBtnMine, g_hLabelData, g_hEditData, g_hBtnData, g_hLabelOpt,
                       g_hComboTrim, g_hLabelSkip,
                       g_hEditSkip, g_hLabelSkipSuffix, g_hLabelTimeout, g_hEditTimeout,
                       g_hLabelTimeoutSuffix, g_hChkBlank, g_hChkLF, g_hBtnStart, g_hBtnPause,
                       g_hBtnStop, g_hBtnClear, g_hBtnOpenOut, g_hStatus}) {
            SendMessageW(h, WM_SETFONT, (WPARAM)g_fontUI, TRUE);
        }
        SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_fontMono, TRUE);

        LoadSettings();

        UpdateButtons(false, false);
        SetStatus(L"就绪：请选择 DEMO 程序、自己的程序和测试数据。");
        return 0;
    }

    case WM_SIZE:
        if (g_hLog) Layout(hWnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = S(820);
        mmi->ptMinTrackSize.y = S(560);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        if (code != BN_CLICKED && code != 0) break;

        switch (id) {
        case IDC_BTN_DEMO: {
            std::wstring p;
            if (PickFile(hWnd, L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0",
                         L"选择 DEMO 程序", p)) {
                SetWindowTextW(g_hEditDemo, p.c_str());
            }
            break;
        }
        case IDC_BTN_MINE: {
            std::wstring p;
            if (PickFile(hWnd, L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0\0",
                         L"选择自己的程序", p)) {
                SetWindowTextW(g_hEditMine, p.c_str());
            }
            break;
        }
        case IDC_BTN_DATA: {
            std::wstring p;
            if (PickFile(hWnd, L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0",
                         L"选择测试数据（每行一组）", p)) {
                SetWindowTextW(g_hEditData, p.c_str());
                if (GetText(g_hEditDemo).empty()) {
                    std::wstring demo = FindFileInDir(ParentDir(p), L"*demo*.exe");
                    if (!demo.empty()) SetWindowTextW(g_hEditDemo, demo.c_str());
                }
            }
            break;
        }
        case IDC_BTN_CLEAR:
            SetWindowTextW(g_hLog, L"");
            break;

        case IDC_BTN_OPENOUT: {
            std::wstring dir = g_outDir;
            if (dir.empty()) {
                wchar_t buf[MAX_PATH]{};
                DWORD n = GetTempPathW(MAX_PATH, buf);
                dir = JoinPath(std::wstring(buf, n), L"gui_compare_out");
            }
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            ShellExecuteW(hWnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }

        case IDC_BTN_PAUSE: {
            if (!g_running) break;
            if (g_uiPaused) {
                g_resumeReq = true;
                g_pauseReq = false;
                SetEvent(g_hResume);
                UpdateButtons(true, false);
                SetStatus(L"继续测试...");
            } else {
                g_pauseReq = true;
                g_resumeReq = false;
                UpdateButtons(true, true);
                SetStatus(L"已暂停（当前这组结束后暂停）。点击“继续”恢复。");
            }
            break;
        }

        case IDC_BTN_STOP: {
            g_stop = true;
            g_resumeReq = true;
            SetEvent(g_hResume);
            TerminateChild();
            SetStatus(L"正在停止...");
            break;
        }

        case IDC_BTN_START: {
            if (g_running) break;

            std::wstring demo = GetText(g_hEditDemo);
            std::wstring mine = GetText(g_hEditMine);
            std::wstring data = GetText(g_hEditData);

            if (demo.empty() || mine.empty() || data.empty()) {
                MessageBoxW(hWnd, L"请先选择 DEMO 程序、自己的程序和测试数据。",
                            L"提示", MB_OK | MB_ICONINFORMATION);
                break;
            }

            // 取出 txt_compare：内嵌资源 → exe 同目录 → 上次记住的路径 → 请用户选一次
            std::wstring tool = EnsureEmbeddedTool();
            if (tool.empty()) tool = AskUserForTool(hWnd);
            if (tool.empty()) {
                SetStatus(L"已取消：没有 txt_compare，无法比较。");
                break;
            }

            if (!std::filesystem::exists(demo) || !std::filesystem::exists(mine) ||
                !std::filesystem::exists(data) || !std::filesystem::exists(tool)) {
                MessageBoxW(hWnd, L"有文件路径不存在，请重新选择。", L"提示",
                            MB_OK | MB_ICONWARNING);
                break;
            }

            Opts opt;
            int sel = (int)SendMessageW(g_hComboTrim, CB_GETCURSEL, 0, 0);
            const wchar_t* trims[] = {L"none", L"left", L"right", L"all"};
            opt.trim = (sel >= 0 && sel < 4) ? trims[sel] : L"none";

            opt.lineskip = _wtoi(GetText(g_hEditSkip).c_str());
            if (opt.lineskip < 0) opt.lineskip = 0;
            if (opt.lineskip > 100) opt.lineskip = 100;

            int tmo = _wtoi(GetText(g_hEditTimeout).c_str());
            if (tmo < 100) tmo = 100;
            if (tmo > 60000) tmo = 60000;
            opt.timeoutMs = (DWORD)tmo;

            opt.ignoreBlank = IsDlgButtonChecked(hWnd, IDC_CHK_BLANK) == BST_CHECKED;
            opt.ignoreLF = IsDlgButtonChecked(hWnd, IDC_CHK_LF) == BST_CHECKED;

            SaveSettings();

            SetWindowTextW(g_hLog, L"");
            SetStatus(L"正在准备...");
            SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

            g_stop = false;
            g_pauseReq = false;
            g_resumeReq = false;
            ResetEvent(g_hResume);
            UpdateButtons(true, false);

            PostLog(L"=== 测试开始 ===\r\n");
            PostLog(L"DEMO 程序  : " + demo + L"\r\n");
            PostLog(L"我的程序   : " + mine + L"\r\n");
            PostLog(L"测试数据   : " + data + L"\r\n");
            PostLog(L"比较引擎   : " + std::wstring(g_toolExternal ? L"外部" : L"内置") +
                    L" txt_compare（" + tool + L"）\r\n");
            PostLog(L"忽略空格   : " + std::wstring(TrimText(opt.trim)) + L", 跳过前 " +
                    std::to_wstring(opt.lineskip) + L" 行, 忽略空行=" +
                    (opt.ignoreBlank ? L"是" : L"否") + L", 忽略换行符差异=" +
                    (opt.ignoreLF ? L"是" : L"否") + L", 超时=" +
                    std::to_wstring(opt.timeoutMs) + L" ms\r\n");
            PostLog(L"------------------------------------------------------------\r\n");

            if (g_worker.joinable()) g_worker.join();
            g_running = true;
            g_worker = std::thread(WorkerThread, demo, mine, data, tool, opt);
            break;
        }
        default:
            break;
        }
        return 0;
    }

    case WM_APP_LOG: {
        auto* p = (std::wstring*)lParam;
        if (p) {
            AppendLog(*p);
            delete p;
        }
        return 0;
    }

    case WM_APP_PROGRESS: {
        int cur = (int)wParam;
        int total = (int)lParam;
        SendMessageW(g_hProgress, PBM_SETRANGE32, 0, total > 0 ? total : 1);
        SendMessageW(g_hProgress, PBM_SETPOS, cur, 0);
        SetStatus(L"正在测试第 " + std::to_wstring(cur) + L" / " + std::to_wstring(total) + L" 组...");
        return 0;
    }

    case WM_APP_PAUSED: {
        auto* pi = (PauseInfo*)lParam;
        if (pi) {
            UpdateButtons(true, true);
            SetStatus(L"已暂停：第 " + std::to_wstring(pi->cur) + L" / " +
                      std::to_wstring(pi->total) + L" 组" + pi->reason +
                      L"。点击“继续”执行下一组，或点击“停止”。");
            delete pi;
        }
        MessageBeep(MB_ICONWARNING);
        return 0;
    }

    case WM_APP_DONE: {
        auto* di = (DoneInfo*)lParam;
        if (di) {
            g_running = false;
            UpdateButtons(false, false);
            SendMessageW(g_hProgress, PBM_SETPOS, di->total, 0);
            std::wstring s = L"测试结束：共 " + std::to_wstring(di->total) + L" 组，通过 " +
                             std::to_wstring(di->passed) + L"，不同/出错 " +
                             std::to_wstring(di->failed);
            if (di->stopped) s += L"（已中止）";
            SetStatus(s);
            if (di->outDir != g_outDir) g_outDir = di->outDir;
            delete di;
        }
        return 0;
    }

    case WM_CLOSE: {
        if (g_running) {
            if (MessageBoxW(hWnd, L"测试正在进行，确定要停止并退出吗？", L"确认",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 0;
            }
            g_stop = true;
            g_resumeReq = true;
            SetEvent(g_hResume);
            TerminateChild();
            if (g_worker.joinable()) g_worker.join();
            g_running = false;
        }
        SaveSettings();
        DestroyWindow(hWnd);
        return 0;
    }

    case WM_DESTROY:
        if (g_worker.joinable()) g_worker.join();
        CleanupEmbeddedTool();
        if (g_fontUI) DeleteObject(g_fontUI);
        if (g_fontMono) DeleteObject(g_fontMono);
        if (g_hResume) CloseHandle(g_hResume);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------- 入口
int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_hInst = hInst;

    SetProcessDPIAware();
    {
        HDC dc = GetDC(nullptr);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        if (g_dpi <= 0) g_dpi = 96;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    SweepStaleToolDirs();

    g_hResume = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_iniPath = JoinPath(ExeDir(), L"gui_compare.ini");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"GuiCompareWndClass";
    if (!RegisterClassExW(&wc)) return 0;

    RECT rc{0, 0, S(980), S(720)};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND hWnd = CreateWindowExW(
        0, wc.lpszClassName, L"txt_compare 输出对比测试工具",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 0;

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
