// app.cpp - Windows 原生 GUI（Win32 自绘卡片式深色主题 + 托盘）
#include "../common/webdav.h"
#include "../common/config.h"
#include "webdav_fs.h"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>  // std::min/std::max
#include <string>
#include <vector>
#include <map>
#include <mutex>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// DPI manifest + high-DPI support (per-monitor v2 where available)
// @note We use the embedded manifest at windows/app.manifest
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// DPI scaling helpers
static int g_dpi = 96;
static int dpiScale(int px) { return MulDiv(px, g_dpi, 96); }

using namespace wddisk;

// ---------------- 配色（现代深色） ----------------
static constexpr COLORREF CLR_BG      = RGB(15, 17, 23);    // 窗口背景
static constexpr COLORREF CLR_CARD    = RGB(23, 26, 34);    // 卡片
static constexpr COLORREF CLR_CARD_HI = RGB(30, 34, 44);    // 卡片悬停
static constexpr COLORREF CLR_BORDER  = RGB(42, 47, 60);    // 边框
static constexpr COLORREF CLR_ACCENT  = RGB(77, 132, 255);  // 主色
static constexpr COLORREF CLR_ACCENT_D= RGB(60, 106, 214);  // 主色按下
static constexpr COLORREF CLR_TEXT    = RGB(235, 238, 245);
static constexpr COLORREF CLR_SUB     = RGB(140, 148, 165); // 次要文字
static constexpr COLORREF CLR_GREEN   = RGB(84, 200, 130);
static constexpr COLORREF CLR_INPUT   = RGB(22, 25, 33);    // 输入框背景
static constexpr COLORREF CLR_DANGER  = RGB(235, 90, 90);

// ---------------- 全局 ----------------
static ConfigStore* g_cfg = nullptr;
static std::map<int, DavFileSystem*> g_mounted;

// 字体
static HFONT fTitle, fName, fSub, fBtn, fStatus, fBadge, fIcon, fTiny;
// 笔刷
static HBRUSH brBg, brCard, brCardHi, brAccent, brAccentD, brInput, brGreen;

// 卡片布局
struct CardBtn { RECT rc; int cardIdx; int cmd; };
struct Card {
    RECT rc; int idx; std::vector<CardBtn> btns;
    COLORREF logoBg = CLR_ACCENT;
    std::wstring logoLetter = L"W";
    std::string logoName = "自定义";
};
static std::vector<Card> g_cards;
static int g_sel = -1, g_hoverCard = -1, g_hoverBtn = -1;
static RECT g_rcAdd; static bool g_hoverAdd = false;
static std::wstring g_status = L"就绪";
static int g_scroll = 0; // 暂不实现滚动，卡片少时够用

// 托盘
#define WM_TRAY (WM_APP + 1)
#define WM_PROGRESS_TICK (WM_APP + 6)
static NOTIFYICONDATAW g_nid = {};
static HICON g_hAppIconSmall = nullptr;
static HICON g_hAppIconBig = nullptr;
static HICON g_hAppIcon = nullptr;
static bool g_trayTipShown = false;

// 传输进度（状态栏 / 卡片显示）与开机自启 UI 状态
static std::wstring g_xferText;      // 例："↑ 上传 backup.zip 64%"
static bool g_onlineState = true;
static RECT g_rcAutoStart = {};      // 自启复选框（自绘）
static bool g_hoverAutoStart = false;

// 侧边栏导航
enum { PAGE_DRIVES = 0, PAGE_SETTINGS = 1, PAGE_ABOUT = 2 };
static int g_page = PAGE_DRIVES;
static int g_hoverNav = -1;                 // 悬停的导航项
static RECT g_rcNav[3] = {};                // 三个导航项热区
static RECT g_rcOpenCache = {}, g_rcClearCache = {};   // 设置页按钮
static bool g_hoverOpenCache = false, g_hoverClearCache = false;
static std::wstring g_cacheInfo;             // 缓存占用文本
static bool g_autoStartCached = false;
static DWORD g_autoStartChk = 0;

// UTF-8 <-> UTF-16
static std::wstring w(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring out(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}
static std::string u8(const std::wstring& ws) {
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// exe 所在目录（配置与缓存优先放这里，便于便携使用/迁移）
static std::wstring exeDir() {
    wchar_t p[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring s(p, n);
    size_t k = s.rfind(L'\\');
    return (k == std::wstring::npos) ? L"." : s.substr(0, k);
}

// 目录是否可写（安装到 Program Files 时普通用户不可写，需要回退）
static bool dirWritable(const std::wstring& dir) {
    if (dir.empty() || dir == L".") return false;
    std::wstring probe = dir + L"\\.wddisk_write_probe";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static std::wstring configDir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;

    // 1) 优先 exe 同目录（用户要求：账号配置跟随安装目录）
    std::wstring ed = exeDir();
    if (dirWritable(ed)) {
        cached = ed;
        // 首次从旧位置迁移配置
        std::wstring newCfg = cached + L"\\config.json";
        if (GetFileAttributesW(newCfg.c_str()) == INVALID_FILE_ATTRIBUTES) {
            wchar_t* p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
                std::wstring oldCfg = std::wstring(p) + L"\\Wddisk\\config.json";
                CoTaskMemFree(p);
                CopyFileW(oldCfg.c_str(), newCfg.c_str(), TRUE);
            }
        }
        return cached;
    }

    // 2) 回退：%LOCALAPPDATA%\Wddisk（安装目录只读时）
    {
        wchar_t* p = nullptr;
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p);
        cached = std::wstring(p) + L"\\Wddisk";
        CoTaskMemFree(p);
        CreateDirectoryW(cached.c_str(), nullptr);
    }
    return cached;
}

// ---------------- DPAPI 密码加密 ----------------
static std::string dpapiEncrypt(const std::string& plain) {
    DATA_BLOB in = { (DWORD)plain.size(), (BYTE*)plain.data() };
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, L"Wddisk", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return "";
    static const char* b64t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    for (DWORD i = 0; i < out.cbData; i += 3) {
        unsigned v = out.pbData[i] << 16;
        if (i + 1 < out.cbData) v |= out.pbData[i + 1] << 8;
        if (i + 2 < out.cbData) v |= out.pbData[i + 2];
        r += b64t[(v >> 18) & 63]; r += b64t[(v >> 12) & 63];
        r += i + 1 < out.cbData ? b64t[(v >> 6) & 63] : '=';
        r += i + 2 < out.cbData ? b64t[v & 63] : '=';
    }
    LocalFree(out.pbData);
    return r;
}

static bool dpapiDecrypt(const std::string& b64, std::string& plain) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string bin;
    unsigned buf = 0; int bits = 0;
    for (char c : b64) {
        if (c == '=') break;
        int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; bin += (char)((buf >> bits) & 0xFF); }
    }
    if (bin.empty()) return false;
    DATA_BLOB in = { (DWORD)bin.size(), (BYTE*)bin.data() };
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    plain.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

// 开机自启（HKCU\Software\Microsoft\Windows\CurrentVersion\Run）
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VAL = L"Wddisk";
static bool autostartEnabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    bool has = RegQueryValueExW(k, RUN_VAL, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS;
    RegCloseKey(k);
    return has && sz > 2;
}
static bool setAutostart(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok;
    if (on) {
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring val = L"\"" + std::wstring(exe) + L"\" --tray";
        ok = RegSetValueExW(k, RUN_VAL, 0, REG_SZ, (const BYTE*)val.c_str(),
            (DWORD)((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        ok = RegDeleteValueW(k, RUN_VAL) == ERROR_SUCCESS || GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(k);
    return ok;
}

static bool isMounted(int idx) {
    return g_mounted.count(idx) && g_mounted[idx] != nullptr;
}

// 后台挂载/卸载状态（防止 UI 线程冻结导致窗口无响应）
#include <set>
#include <thread>
#include <atomic>
static std::set<int> g_busy;              // 正在挂载/卸载的卡片
static std::atomic<int> g_busyCount{0};   // 在飞后台任务数
static HWND g_editDlg = nullptr;          // 打开中的编辑对话框
#define WM_OP_DONE (WM_APP + 2)           // wParam=idx, lParam=1挂载成功/0失败|2卸载完成

static bool isBusy(int idx) { return g_busy.count(idx) != 0; }

struct OpResult { int idx = -1; bool ok = false; bool isMount = false; std::string err; DavFileSystem* fs = nullptr; };
static std::mutex g_opMtx;
static OpResult g_opRes;

// ---------------- 绘制辅助 ----------------
static HBRUSH solidBrush(COLORREF c) { return CreateSolidBrush(c); }

// 线性渐变填充（垂直，GradientFill）
static void fillGradientV(HDC dc, RECT rc, COLORREF c1, COLORREF c2) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    TRIVERTEX tv[2];
    tv[0].x = rc.left;   tv[0].y = rc.top;
    tv[0].Red   = (COLOR16)(GetRValue(c1) << 8);
    tv[0].Green = (COLOR16)(GetGValue(c1) << 8);
    tv[0].Blue  = (COLOR16)(GetBValue(c1) << 8);
    tv[0].Alpha = 0xFF00;
    tv[1].x = rc.right;  tv[1].y = rc.bottom;   // 对角点，保证宽度非 0
    tv[1].Red   = (COLOR16)(GetRValue(c2) << 8);
    tv[1].Green = (COLOR16)(GetGValue(c2) << 8);
    tv[1].Blue  = (COLOR16)(GetBValue(c2) << 8);
    tv[1].Alpha = 0xFF00;
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(dc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

// 多层阴影：下方偏移几个像素逐层加深
static void drawCardShadow(HDC dc, RECT rc, int hoverLift) {
    const int layers = 4;
    static const int offsets[4] = { 6, 4, 2, 1 };
    static const COLORREF shadowClrs[4] = {
        RGB(6, 7, 12),  RGB(9, 10, 16), RGB(11, 13, 20), RGB(13, 15, 24)
    };
    for (int i = 0; i < layers; ++i) {
        RECT s = rc;
        OffsetRect(&s, 0, offsets[i] + hoverLift);
        InflateRect(&s, i, i);
        HBRUSH br = solidBrush(shadowClrs[i]);
        HBRUSH oldBr = (HBRUSH)SelectObject(dc, br);
        HPEN nullPen = (HPEN)GetStockObject(NULL_PEN);
        HPEN oldPen = (HPEN)SelectObject(dc, nullPen);
        RoundRect(dc, s.left, s.top, s.right, s.bottom, 14 * 2, 14 * 2);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBr);
        DeleteObject(br);
    }
}

// 窗口圆角：用 SetWindowRgn 裁剪（Win10 也生效；Win11 上叠合 DWM 圆角）
static void applyRoundRegion(HWND hwnd) {
    if (IsZoomed(hwnd) || IsIconic(hwnd)) { SetWindowRgn(hwnd, nullptr, TRUE); return; }
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    int r = dpiScale(12);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2);
    if (rgn) SetWindowRgn(hwnd, rgn, TRUE);  // 成功后系统接管该 region
}

// Win10/11 深色标题栏（用于带系统 caption 的编辑对话框）
static void applyDarkFrame(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *Fn)(HWND, DWORD, LPCVOID, DWORD);
    Fn fn = (Fn)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn) {
        int pref = 2; // DWMWCP_ROUND
        fn(hwnd, 33, &pref, sizeof(pref));
        BOOL dark = TRUE;
        fn(hwnd, 20, &dark, sizeof(dark));
        fn(hwnd, 19, &dark, sizeof(dark));
    }
    // 不要用 DwmExtendFrameIntoClientArea(-1)，否则标题栏会变玻璃白
}

static void fillRound(HDC dc, RECT rc, int rad, HBRUSH br) {
    HBRUSH old = (HBRUSH)SelectObject(dc, br);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, rad * 2, rad * 2);
    SelectObject(dc, oldPen); DeleteObject(pen);
    SelectObject(dc, old);
}

static void strokeRound(HDC dc, RECT rc, int rad, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    HBRUSH oldBr = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, rad * 2, rad * 2);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen); DeleteObject(pen);
}

static void drawText(HDC dc, const std::wstring& s, RECT rc, HFONT f, COLORREF c, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    HFONT old = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &rc, flags);
    SelectObject(dc, old);
}

static bool ptIn(POINT p, const RECT& r) { return PtInRect(&r, p); }

static bool inCardButton(POINT p, int* cardIdx, int* cmd) {
    for (auto& c : g_cards)
        for (auto& b : c.btns)
            if (ptIn(p, b.rc)) { *cardIdx = b.cardIdx; *cmd = b.cmd; return true; }
    return false;
}

// ---------------- 业务逻辑 ----------------
static void relayout(HWND hwnd);

static void doMount(int idx, HWND hwnd) {
    extern void fslog(const char* fmt, ...);
    fslog("doMount: enter idx=%d mounted=%d busy=%d", idx, (int)isMounted(idx), (int)isBusy(idx));
    if (isMounted(idx) || isBusy(idx)) return;
    auto& s = g_cfg->servers()[idx];
    std::wstring mp;
    if (s.mountPoint.size() >= 2 && s.mountPoint[1] == ':') {
        mp = w(s.mountPoint.c_str());
    } else {
        for (wchar_t c = L'Z'; c >= L'F'; --c) {
            wchar_t root[4] = { c, L':', L'\\', 0 };
            if (GetDriveTypeW(root) == DRIVE_NO_ROOT_DIR) { mp = std::wstring(1, c) + L":"; break; }
        }
        if (mp.empty()) { g_status = L"没有可用盘符"; InvalidateRect(hwnd, nullptr, TRUE); return; }
        s.mountPoint = u8(mp);
        g_cfg->save();
    }
    g_busy.insert(idx);
    ++g_busyCount;
    g_status = w(("正在挂载 " + s.name + " -> " + s.mountPoint + " …").c_str());
    relayout(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    DavConfig cfgCopy = s;
    std::wstring cacheDir = configDir() + L"\\cache";
    std::thread([idx, hwnd, cfgCopy, mp, cacheDir]() {
        // 后台执行挂载（含网络验证），不阻塞 UI 线程
        DavFileSystem* fs = new DavFileSystem(cfgCopy, cacheDir);
        std::string err;
        bool ok = fs->mount(mp, err);
        std::lock_guard<std::mutex> l(g_opMtx);
        g_opRes = { idx, ok, true, ok ? std::string() : err, ok ? fs : nullptr };
        if (!ok) delete fs;
        PostMessageW(hwnd, WM_OP_DONE, 0, 0);
    }).detach();
}

static void doUnmount(int idx, HWND hwnd) {
    auto it = g_mounted.find(idx);
    if (it == g_mounted.end() || isBusy(idx)) return;
    g_busy.insert(idx);
    ++g_busyCount;
    g_status = L"正在卸载…";
    relayout(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
    DavFileSystem* fs = it->second;
    std::thread([idx, hwnd, fs]() {
        // 后台卸载：排空上传队列 + 停止调度，不阻塞 UI 线程
        fs->unmount();
        delete fs;
        std::lock_guard<std::mutex> l(g_opMtx);
        g_opRes = { idx, true, false, std::string(), nullptr };
        PostMessageW(hwnd, WM_OP_DONE, 0, 0);
    }).detach();
}

static void onOpDone(HWND hwnd) {
    OpResult r;
    { std::lock_guard<std::mutex> l(g_opMtx); r = g_opRes; g_opRes.idx = -1; }
    if (r.idx < 0) return;
    extern void fslog(const char* fmt, ...);
    fslog("opDone: idx=%d mount=%d ok=%d err=%s", r.idx, (int)r.isMount, (int)r.ok, r.err.c_str());
    g_busy.erase(r.idx);
    --g_busyCount;
    if (r.isMount) {
        if (r.ok && r.fs) {
            g_mounted[r.idx] = r.fs;
            g_status = w(("已挂载 " + g_cfg->servers()[r.idx].name + " -> " + g_cfg->servers()[r.idx].mountPoint).c_str());
        } else {
            g_status = w(("挂载失败: " + r.err).c_str());
            // 不弹模态框（会卡住窗口导致"关不掉"），用托盘气泡提示错误
            g_nid.uFlags = NIF_INFO;
            std::wstring info = w(("挂载失败: " + r.err).c_str());
            wcsncpy_s(g_nid.szInfo, info.c_str(), _TRUNCATE);
            wcscpy_s(g_nid.szInfoTitle, L"Wddisk");
            g_nid.dwInfoFlags = NIIF_ERROR;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_trayTipShown = true;
        }
    } else {
        g_mounted.erase(r.idx);
        g_status = L"已卸载";
    }
    relayout(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void openInExplorer(HWND hwnd, int idx) {
    if (isBusy(idx)) return;
    auto& s = g_cfg->servers()[idx];
    if (!isMounted(idx)) doMount(idx, hwnd);
    std::wstring path = w(s.mountPoint.c_str()) + L"\\";
    ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------- 编辑对话框（自绘深色） ----------------
enum { IDC_ED_NAME = 101, IDC_ED_URL, IDC_ED_USER, IDC_ED_PASS, IDC_ED_DRIVE, IDC_ED_NOTLS };

struct EditDlg {
    HWND hwnd, parent;
    bool isNew, ok = false;
    DavConfig* cfg;
    int editIdx;
    RECT rcName, rcUrl, rcUser, rcPass, rcCombo, rcTls, rcSave, rcCancel, rcAuto;
    int focusField = 0; // 1..4 = 名称/URL/用户/密码, 5 = 盘符, 0 = 无
    bool hoverSave = false, hoverCancel = false, hoverTls = false, hoverAuto = false, hoverCombo = false;
    bool tlsChecked = false;
    bool autoMount = false;
    // 自绘输入框：4 个（名称/URL/用户/密码），密码框自动走密文显示
    std::wstring seReal[4];
    // 可用盘符列表（动态枚举，跳过 A/B 软驱与已占用盘符）
    std::vector<wchar_t> driveList;
    int driveSel = 0;   // driveList 索引
    wchar_t origDrive = 0;  // 编辑前配置的盘符（用于标注"已配置"）
    bool driveDropOpen = false;
    HWND drivePop = nullptr;
    HWND hiddenEdit = nullptr;  // 单个系统 EDIT 承接 IME/剪贴板/键盘，1x1 不可见
    bool hiddenEditSyncing = false;
    // 光标闪烁
    bool cursorVisible = true;
    int cursorFieldForTimer = 0;
};

static bool g_dlgTrack = false;
#define WM_DRIVE_PICK (WM_APP + 10)

// 枚举可挂载的盘符：C..Z 中未被占用的（跳过 A/B 软驱）；
// 若当前配置的盘符已被占用（通常是本程序自己挂载的），也保留在列表中。
static std::vector<wchar_t> buildDriveList(const std::string& current) {
    DWORD mask = GetLogicalDrives();
    std::vector<wchar_t> out;
    wchar_t cur = 0;
    if (!current.empty()) {
        char c0 = (char)toupper((unsigned char)current[0]);
        if (c0 >= 'C' && c0 <= 'Z') cur = (wchar_t)c0;
    }
    for (wchar_t ch = L'C'; ch <= L'Z'; ++ch) {
        bool used = (mask & (1u << (ch - L'A'))) != 0;
        if (!used || ch == cur) out.push_back(ch);
    }
    if (out.empty()) {  // 极端情况：全部占用时仍列出 C..Z
        for (wchar_t ch = L'C'; ch <= L'Z'; ++ch) out.push_back(ch);
    }
    return out;
}

// ---------------- 自绘输入框辅助 ----------------
static void drawSelfEdit(HDC dc, HWND hwnd, EditDlg* d, RECT rc, int fieldIdx) {
    // 深色输入框底 + 圆角边框（聚焦 ACCENT / 普通 BORDER）
    COLORREF c = (d->focusField == fieldIdx + 1) ? CLR_ACCENT : CLR_BORDER;
    fillRound(dc, rc, 8, brInput);
    strokeRound(dc, rc, 8, c);
    RECT textRc = { rc.left + 12, rc.top + 6, rc.right - 12, rc.bottom - 6 };

    std::wstring disp;
    if (fieldIdx == 3) { disp.assign(d->seReal[fieldIdx].size(), L'\x2022'); }
    else disp = d->seReal[fieldIdx];

    // 密码框空显示提示（浅色）
    COLORREF tClr = CLR_TEXT;
    if (disp.empty()) { tClr = CLR_SUB; disp = L"(必填)"; }

    drawText(dc, disp, textRc, fBtn, tClr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 聚焦 + 光标可见 → 画竖线光标（空内容/密码框不画）
    if (d->focusField == fieldIdx + 1 && d->cursorVisible && !d->seReal[fieldIdx].empty()) {
        HFONT oldF = (HFONT)SelectObject(dc, fBtn);
        SIZE sz; GetTextExtentPoint32W(dc, disp.c_str(), (int)disp.size(), &sz);
        int cx = textRc.left + sz.cx + 1;
        int cy1 = textRc.top + 3, cy2 = textRc.bottom - 3;
        SelectObject(dc, oldF);
        HPEN pen = CreatePen(PS_SOLID, 2, CLR_TEXT);
        HPEN oldP = (HPEN)SelectObject(dc, pen);
        MoveToEx(dc, cx, cy1, nullptr);
        LineTo(dc, cx, cy2);
        SelectObject(dc, oldP);
        DeleteObject(pen);
    }
}

// ---------------- 盘符下拉自绘弹出窗口（两列网格） ----------------
static const int DRIVE_ITEM_H = 30;
static const int DRIVE_ITEM_W = 92;

static void driveGridGeom(int n, int& rows, int& cols) {
    rows = (n + 1) / 2;
    cols = (n > rows) ? 2 : 1;
}
// 返回命中索引，-1 表示未命中
static int driveHitTest(EditDlg* d, POINT p) {
    int n = (int)d->driveList.size();
    if (n == 0) return -1;
    int rows, cols; driveGridGeom(n, rows, cols);
    int col = (p.x - 5) / DRIVE_ITEM_W;
    int row = (p.y - 5) / DRIVE_ITEM_H;
    if (col < 0 || col >= cols || row < 0 || row >= rows) return -1;
    int idx = col * rows + row;
    return (idx >= 0 && idx < n) ? idx : -1;
}

static LRESULT CALLBACK DrivePopProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditDlg* d = (EditDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        if (!d) break;
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
        FillRect(mem, &rc, brCard);
        strokeRound(mem, rc, 8, CLR_BORDER);

        int n = (int)d->driveList.size();
        int rows, cols; driveGridGeom(n, rows, cols);
        for (int i = 0; i < n; ++i) {
            int col = i / rows, row = i % rows;
            int ix = 5 + col * DRIVE_ITEM_W;
            int iy = 5 + row * DRIVE_ITEM_H;
            RECT ir = { ix, iy, ix + DRIVE_ITEM_W - 2, iy + DRIVE_ITEM_H - 2 };
            bool sel = (i == d->driveSel);
            if (sel) fillRound(mem, ir, 6, brAccent);
            wchar_t s[8];
            swprintf(s, 8, L"%c:", d->driveList[i]);
            drawText(mem, s, { ir.left + 10, ir.top, ir.left + 38, ir.bottom },
                fBtn, sel ? CLR_TEXT : CLR_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            bool isCur = (d->origDrive && d->origDrive == d->driveList[i]);
            drawText(mem, isCur ? L"占用" : L"可用",
                { ir.left + 36, ir.top, ir.right - 6, ir.bottom },
                fTiny, sel ? RGB(225, 235, 255) : RGB(120, 128, 145),
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        if (d) {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = driveHitTest(d, p);
            if (idx >= 0 && idx != d->driveSel) {
                d->driveSel = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, idx >= 0 ? IDC_HAND : IDC_ARROW));
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!d) break;
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = driveHitTest(d, p);
        if (idx >= 0) {
            HWND parent = GetParent(hwnd);
            PostMessageW(parent, WM_DRIVE_PICK, idx, 0);
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 子类化隐藏系统 EDIT：拦截 Tab/Enter/Esc 由父对话框统一处理，其它走系统默认
static WNDPROC g_origHiddenEditProc = nullptr;
static LRESULT CALLBACK HiddenEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_TAB) {
            // 转交给父窗口的 EditDlgProc 统一处理
            return SendMessageW(GetParent(hwnd), msg, wp, lp);
        }
    }
    return g_origHiddenEditProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK EditDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditDlg* d = (EditDlg*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CLOSE: { DestroyWindow(hwnd); PostQuitMessage(0); return 0; }
    case WM_CREATE: {
        // 通过 CREATESTRUCTW.lpCreateParams 拿到外部栈上的 EditDlg*（必须在 CreateWindowExW 调用时就传入）
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        d = (EditDlg*)cs->lpCreateParams;
        // 1x1 不可见系统 EDIT：承接 IME / 剪贴板 / 光标，自绘层只负责显示
        d->hiddenEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | ES_AUTOHSCROLL, -100, -100, 1, 1, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        // 先保存原始 WndProc，再子类化（否则创建期间的 WM_PAINT 会命中 null g_origHiddenEditProc → 崩溃）
        g_origHiddenEditProc = (WNDPROC)GetWindowLongPtrW(d->hiddenEdit, GWLP_WNDPROC);
        SetWindowLongPtrW(d->hiddenEdit, GWLP_WNDPROC, (LONG_PTR)HiddenEditProc);
        SetTimer(hwnd, 2, 530, nullptr);  // 光标闪烁
        return 0;
    }
    case WM_DESTROY: { KillTimer(hwnd, 2); if (d->drivePop) DestroyWindow(d->drivePop); return 0; }
    case WM_TIMER: {
        if (wp == 2 && d->focusField >= 1 && d->focusField <= 4) {
            d->cursorVisible = !d->cursorVisible;
            RECT rc;
            switch (d->focusField) {
            case 1: rc = d->rcName; break;
            case 2: rc = d->rcUrl;  break;
            case 3: rc = d->rcUser; break;
            case 4: rc = d->rcPass; break;
            default: return 0;
            }
            InflateRect(&rc, 4, 4);
            InvalidateRect(hwnd, &rc, FALSE);
        }
        return 0;
    }
    case WM_COMMAND: {
        // EN_CHANGE：hiddenEdit 内容变化 → 同步回 seReal[focusField-1] 并重绘
        if (d->hiddenEdit && (HWND)lp == d->hiddenEdit &&
            HIWORD(wp) == EN_CHANGE && d->focusField >= 1 && d->focusField <= 4) {
            if (!d->hiddenEditSyncing) {
                wchar_t buf[4096]; GetWindowTextW(d->hiddenEdit, buf, 4096);
                d->seReal[d->focusField - 1] = buf;
                RECT rc;
                switch (d->focusField) {
                case 1: rc = d->rcName; break; case 2: rc = d->rcUrl; break;
                case 3: rc = d->rcUser; break; case 4: rc = d->rcPass; break;
                default: return 0;
                }
                InflateRect(&rc, 4, 4);
                InvalidateRect(hwnd, &rc, FALSE);
            }
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
        FillRect(mem, &rc, brBg);

        wchar_t title[64]; GetWindowTextW(hwnd, title, 64);
        drawText(mem, title, { 24, 18, rc.right, 48 }, fTitle, CLR_TEXT);
        auto label = [&](const wchar_t* t, RECT r) { drawText(mem, t, r, fSub, CLR_SUB); };
        label(L"名称",       { 24, d->rcName.top - 20, 300, d->rcName.top });
        label(L"服务器 URL", { 24, d->rcUrl.top  - 20, 300, d->rcUrl.top });
        label(L"用户名",     { 24, d->rcUser.top - 20, 300, d->rcUser.top });
        label(L"密码",       { 24, d->rcPass.top - 20, 300, d->rcPass.top });
        label(L"挂载盘符",   { 24, d->rcCombo.top - 20, 300, d->rcCombo.top });

        drawSelfEdit(mem, hwnd, d, d->rcName, 0);
        drawSelfEdit(mem, hwnd, d, d->rcUrl,  1);
        drawSelfEdit(mem, hwnd, d, d->rcUser, 2);
        drawSelfEdit(mem, hwnd, d, d->rcPass, 3);

        // 盘符下拉按钮（右箭头 ▾）
        fillRound(mem, d->rcCombo, 8, brInput);
        strokeRound(mem, d->rcCombo, 8, (d->hoverCombo || d->driveDropOpen) ? CLR_ACCENT : CLR_BORDER);
        {
            wchar_t drv[4] = { d->driveList.empty() ? L'Z' : d->driveList[d->driveSel], L':', 0 };
            drawText(mem, drv, { d->rcCombo.left + 12, d->rcCombo.top, d->rcCombo.right - 28, d->rcCombo.bottom },
                fBtn, CLR_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT arrow = { d->rcCombo.right - 24, d->rcCombo.top + 2,
                           d->rcCombo.right - 4,  d->rcCombo.bottom - 2 };
            POINT pts[3] = {
                { arrow.left,  arrow.top + (arrow.bottom - arrow.top) / 2 },
                { arrow.right, arrow.top + (arrow.bottom - arrow.top) / 2 },
                { arrow.left + (arrow.right - arrow.left) / 2, arrow.bottom }
            };
            SetDCBrushColor(mem, (d->hoverCombo || d->driveDropOpen) ? CLR_ACCENT : CLR_SUB);
            Polygon(mem, pts, 3);
        }

        // TLS / AutoMount 复选框（沿用原自绘）
        auto drawCheck = [&](RECT rc, bool checked, bool hover, RECT labelRc, const wchar_t* text) {
            fillRound(mem, rc, 8, brCard); strokeRound(mem, rc, 8, CLR_BORDER);
            RECT box = { rc.left + 14, rc.top + (rc.bottom - rc.top - 16) / 2,
                         rc.left + 30, rc.top + (rc.bottom - rc.top + 16) / 2 };
            strokeRound(mem, box, 4, checked ? CLR_ACCENT : CLR_SUB);
            if (checked) fillRound(mem, { box.left + 3, box.top + 3, box.right - 3, box.bottom - 3 }, 3, brAccent);
            drawText(mem, text, labelRc, fBtn, hover ? CLR_TEXT : CLR_SUB);
        };
        drawCheck(d->rcTls,  d->tlsChecked,  d->hoverTls,  { d->rcTls.left + 44, d->rcTls.top,
                   d->rcTls.right, d->rcTls.bottom }, L"跳过 TLS 证书验证");
        drawCheck(d->rcAuto, d->autoMount,   d->hoverAuto, { d->rcAuto.left + 44, d->rcAuto.top,
                   d->rcAuto.right, d->rcAuto.bottom }, L"程序启动时自动挂载此服务器");

        // 按钮
        fillRound(mem, d->rcCancel, 8, d->hoverCancel ? brCardHi : brCard);
        strokeRound(mem, d->rcCancel, 8, CLR_BORDER);
        drawText(mem, L"取 消", d->rcCancel, fBtn, CLR_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        fillRound(mem, d->rcSave, 8, d->hoverSave ? brAccent : brAccentD);
        drawText(mem, L"保 存", d->rcSave, fBtn, CLR_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        bool hs = ptIn(p, d->rcSave), hc = ptIn(p, d->rcCancel),
             ht = ptIn(p, d->rcTls), hAu = ptIn(p, d->rcAuto), hCo = ptIn(p, d->rcCombo);
        if (hs != d->hoverSave || hc != d->hoverCancel || ht != d->hoverTls ||
            hAu != d->hoverAuto || hCo != d->hoverCombo) {
            d->hoverSave = hs; d->hoverCancel = hc; d->hoverTls = ht;
            d->hoverAuto = hAu; d->hoverCombo = hCo;
            InvalidateRect(hwnd, nullptr, FALSE);
            bool anyField = ptIn(p, d->rcName) || ptIn(p, d->rcUrl) ||
                            ptIn(p, d->rcUser) || ptIn(p, d->rcPass);
            SetCursor(LoadCursorW(nullptr, (hs || hc || ht || hAu || hCo) ? IDC_HAND :
                                  anyField ? IDC_IBEAM : IDC_ARROW));
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        d->hoverSave = d->hoverCancel = d->hoverTls = d->hoverAuto = d->hoverCombo = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        // 自绘输入框点击 → 同步 hiddenEdit 内容 + SetFocus
        int clickedField = 0;
        if      (ptIn(p, d->rcName)) clickedField = 1;
        else if (ptIn(p, d->rcUrl))  clickedField = 2;
        else if (ptIn(p, d->rcUser)) clickedField = 3;
        else if (ptIn(p, d->rcPass)) clickedField = 4;
        if (clickedField > 0) {
            bool changed = (d->focusField != clickedField);
            d->focusField = clickedField;
            d->cursorVisible = true;
            d->hiddenEditSyncing = true;
            SetWindowTextW(d->hiddenEdit, d->seReal[clickedField - 1].c_str());
            d->hiddenEditSyncing = false;
            SetFocus(d->hiddenEdit);
            SendMessageW(d->hiddenEdit, EM_SETSEL, 0, -1);
            if (changed) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        // 盘符下拉
        if (ptIn(p, d->rcCombo)) {
            if (!d->drivePop) {
                POINT sc0 = { 0, 0 }; ClientToScreen(hwnd, &sc0);
                int n = (int)d->driveList.size();
                int rows, cols; driveGridGeom(n, rows, cols);
                int popW = 10 + cols * DRIVE_ITEM_W;
                int popH = 10 + rows * DRIVE_ITEM_H;
                int popX = sc0.x + d->rcCombo.left;
                int popY = sc0.y + d->rcCombo.bottom + 2;
                // 超出工作区时向上弹
                RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                if (popY + popH > work.bottom) popY = sc0.y + d->rcCombo.top - popH - 2;
                if (popX + popW > work.right) popX = work.right - popW - 8;
                if (popX < work.left) popX = work.left + 8;
                if (popY < work.top) popY = work.top + 8;
                d->drivePop = CreateWindowExW(0, L"STATIC", L"",
                    WS_POPUP | WS_CLIPCHILDREN, popX, popY, popW, popH,
                    hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                SetWindowLongPtrW(d->drivePop, GWLP_WNDPROC, (LONG_PTR)DrivePopProc);
                SetWindowLongPtrW(d->drivePop, GWLP_USERDATA, (LONG_PTR)d);
            }
            ShowWindow(d->drivePop, SW_SHOW);
            d->driveDropOpen = true;
            InvalidateRect(d->drivePop, nullptr, TRUE);
            InvalidateRect(hwnd, nullptr, FALSE);
            // 让弹窗收到鼠标消息，父窗口继续走自己的模态循环
            SetFocus(hwnd);
            return 0;
        }
        if (ptIn(p, d->rcTls))  d->tlsChecked  = !d->tlsChecked;
        else if (ptIn(p, d->rcAuto)) d->autoMount = !d->autoMount;
        else {
            // 点空白失焦
            if (d->focusField > 0) { d->focusField = 0; InvalidateRect(hwnd, nullptr, FALSE); }
            if (d->drivePop)       { ShowWindow(d->drivePop, SW_HIDE); d->driveDropOpen = false; }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (ptIn(p, d->rcSave)) {
            // 保存前把 hiddenEdit 最后一次变化同步
            if (d->focusField >= 1 && d->focusField <= 4) {
                wchar_t buf[4096]; GetWindowTextW(d->hiddenEdit, buf, 4096);
                d->seReal[d->focusField - 1] = buf;
            }
            d->ok = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (ptIn(p, d->rcCancel)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wp == VK_ESCAPE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (wp == VK_RETURN) {
            // 保存前先把 hiddenEdit 最后同步一下
            if (d->focusField >= 1 && d->focusField <= 4) {
                wchar_t buf[4096]; GetWindowTextW(d->hiddenEdit, buf, 4096);
                d->seReal[d->focusField - 1] = buf;
            }
            d->ok = true; PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        }
        if (wp == VK_TAB) {
            bool shift = (GetKeyState(VK_SHIFT) < 0);
            if (d->focusField < 1 || d->focusField > 4) d->focusField = shift ? 4 : 1;
            else {
                if (shift) d->focusField = (d->focusField == 1) ? 4 : d->focusField - 1;
                else       d->focusField = (d->focusField == 4) ? 1 : d->focusField + 1;
            }
            d->cursorVisible = true;
            // 同步 hiddenEdit 内容到新字段
            if (d->hiddenEdit) {
                d->hiddenEditSyncing = true;
                SetWindowTextW(d->hiddenEdit, d->seReal[d->focusField - 1].c_str());
                d->hiddenEditSyncing = false;
                SetFocus(d->hiddenEdit);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;  // 其它键让 hiddenEdit 原处理走（IM 继续）
    }
    case WM_DRIVE_PICK: {
        d->driveSel = (int)wp;
        d->driveDropOpen = false;
        if (d->drivePop) ShowWindow(d->drivePop, SW_HIDE);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static const wchar_t* ED_CLS = L"WddiskEditWnd";

static bool openEditor(HWND parent, int editIdx) {
    extern void fslog(const char* fmt, ...);
    fslog("openEditor: enter editIdx=%d", editIdx);
    bool isNew = editIdx < 0;
    DavConfig c;
    if (!isNew) c = g_cfg->servers()[editIdx];

    static bool clsReg = false;
    if (!clsReg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = EditDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = ED_CLS;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        clsReg = true;
    }

    RECT pr; GetWindowRect(parent, &pr);
    // 客户区目标 480x556（底部按钮区与复选框区分开，避免重叠）
    int W = 480, H = 556;
    RECT adj = { 0, 0, W, H };
    AdjustWindowRect(&adj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int WW = adj.right - adj.left, WH = adj.bottom - adj.top;
    int X = pr.left + (pr.right - pr.left - WW) / 2;
    int Y = pr.top + (pr.bottom - pr.top - WH) / 2;
    // Clamp to screen in case parent is minimized/off-screen (--mount --tray mode)
    int sx = GetSystemMetrics(0), sy = GetSystemMetrics(1), sw = GetSystemMetrics(78), sh = GetSystemMetrics(79);
    if (sw <= 0 || sh <= 0) { sw = sx; sh = sy; }
    if (X < 0) X = 40;
    if (Y < 0) Y = 40;
    if (X + WW > sw) X = sw - WW - 40;
    if (Y + WH > sh) Y = sh - WH - 40;

    EditDlg d = {};
    d.parent = parent; d.isNew = isNew; d.editIdx = editIdx;
    d.tlsChecked = !c.verifyTls;
    d.autoMount = c.autoMount;
    d.rcName  = { 24, 84,  W - 24, 118 };
    d.rcUrl   = { 24, 156, W - 24, 190 };
    d.rcUser  = { 24, 228, W - 24, 262 };
    d.rcPass  = { 24, 300, W - 24, 334 };
    d.rcCombo = { 24, 374, 144, 404 };
    d.rcTls   = { 164, 366, W - 24, 412 };
    d.rcAuto  = { 24, 420, W - 24, 470 };
    d.rcSave  = { W - 208, H - 56, W - 116, H - 22 };
    d.rcCancel= { W - 104, H - 56, W - 12,  H - 22 };
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, ED_CLS, isNew ? L"添加服务器" : L"编辑服务器",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, X, Y, WW, WH, parent, nullptr,
        GetModuleHandleW(nullptr), (LPVOID)&d);
    fslog("openEditor: dlg=%p gle=%lu", (void*)dlg, GetLastError());
    applyDarkFrame(dlg);
    d.hwnd = dlg;
    g_editDlg = dlg;

    // 自绘输入框初始值（EDIT / COMBOBOX 全部自绘，零 Win32 原生控件）
    d.seReal[0] = w(c.name.c_str());
    d.seReal[1] = w(c.url.c_str());
    d.seReal[2] = w(c.username.c_str());
    d.seReal[3] = w(c.password.c_str());
    // 动态枚举可用盘符（跳过 A/B 与已占用盘符）
    if (!c.mountPoint.empty()) {
        char c0 = (char)toupper((unsigned char)c.mountPoint[0]);
        if (c0 >= 'C' && c0 <= 'Z') d.origDrive = (wchar_t)c0;
    }
    d.driveList = buildDriveList(c.mountPoint);
    d.driveSel = (int)d.driveList.size() - 1;  // 默认选最后一个（Z 优先，符合习惯）
    if (d.origDrive) {
        for (size_t i = 0; i < d.driveList.size(); ++i)
            if (d.driveList[i] == d.origDrive) { d.driveSel = (int)i; break; }
    }
    {
        std::string dl;
        for (wchar_t ch : d.driveList) { dl += (char)ch; dl += ' '; }
        extern void fslog(const char* fmt, ...);
        fslog("openEditor: driveList=[%s] sel=%c", dl.c_str(),
            d.driveList.empty() ? '?' : (char)d.driveList[d.driveSel]);
    }

    ShowWindow(dlg, SW_SHOW);
    EnableWindow(parent, FALSE);
    // 隐藏系统 EDIT 承接 IME/剪贴板，初始同步名称字段
    d.focusField = 1;
    d.hiddenEditSyncing = true;
    SetWindowTextW(d.hiddenEdit, d.seReal[0].c_str());
    d.hiddenEditSyncing = false;
    SetFocus(d.hiddenEdit);
    InvalidateRect(dlg, nullptr, FALSE);

    // 简化模态循环：Tab/Enter/Esc 由 HiddenEditProc → EditDlgProc case WM_KEYDOWN 拦截
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    bool ok = d.ok;
    if (ok) {
        DavConfig nc;
        nc.name = u8(d.seReal[0]);
        nc.url  = u8(d.seReal[1]);
        nc.username = u8(d.seReal[2]);
        nc.password = u8(d.seReal[3]);
        nc.verifyTls = !d.tlsChecked;
        nc.autoMount = d.autoMount;
        nc.mountPoint = std::string(1, (char)(d.driveList.empty() ? L'Z' : d.driveList[d.driveSel])) + ":";
        if (nc.name.empty()) nc.name = "服务器";
        while (nc.url.size() > 1 && nc.url.back() == '/') nc.url.pop_back();
        if (isNew) g_cfg->servers().push_back(nc);
        else g_cfg->servers()[editIdx] = nc;
        g_cfg->save();
    }
    EnableWindow(parent, TRUE);
    g_editDlg = nullptr;
    DestroyWindow(dlg);
    fslog("openEditor: exit ok=%d", (int)ok);
    return ok;
}

// ---------------- 弹出菜单 ----------------
enum { IDM_EDIT = 3001, IDM_DEL, IDM_MOUNT, IDM_UNMOUNT, IDM_OPEN };
enum { IDM_OPEN2 = 3101, IDM_EDIT2, IDM_MOUNT2, IDM_DEL2 };
enum { TRAY_OPEN = 4001, TRAY_MOUNTALL, TRAY_UNMOUNTALL, TRAY_EXIT };

// ---------------- 主窗口布局与绘制 ----------------
static constexpr int CARD_H = 92;
static constexpr int CARD_GAP = 14;
static constexpr int MARGIN = 24;
static constexpr int HEADER_H = 84;
static constexpr int FOOTER_H = 34;
static constexpr int SIDEBAR_W = 96;
// 自绘标题栏高度（自定义 caption，保证深色 + 圆角同时成立）
static constexpr int TITLE_H = 38;
static RECT g_rcMinBtn = {}, g_rcCloseBtn = {};
static int g_hoverTitle = -1;   // 0=最小化 1=关闭

// 按 URL 关键字自动匹配网盘 logo（颜色 + 字母）
static void matchLogoByUrl(const std::string& url, COLORREF& bg, std::wstring& letter, std::string& name) {
    struct E { const char* key; COLORREF c; wchar_t l; const char* n; };
    static const E t[] = {
        { "123pan",     RGB(39,194,129),  L'1', "123云盘"   },
        { "123yun",     RGB(39,194,129),  L'1', "123云盘"   },
        { "aliyundrive",RGB(255,140,0),   L'A', "阿里云盘" },
        { "aliyun",     RGB(255,140,0),   L'A', "阿里云盘" },
        { "baidu",      RGB(41,50,225),   L'B', "百度网盘" },
        { "netdisk",    RGB(41,50,225),   L'B', "百度网盘" },
        { "weiyun",     RGB(52,120,246),  L'V', "腾讯微云" },
        { "tencent",    RGB(52,120,246),  L'V', "腾讯微云" },
        { "lanzou",     RGB(155,89,182),  L'L', "蓝奏云"   },
        { "nutstore",   RGB(231,76,60),   L'N', "坚果云"   },
        { "jbox",       RGB(231,76,60),   L'N', "坚果云"   },
        { "s3",         RGB(231,178,60),  L'S', "S3兼容"   },
        { "b2",         RGB(231,178,60),  L'S', "S3兼容"   },
        { "minio",      RGB(231,178,60),  L'S', "S3兼容"   },
        { "icloud",     RGB(0,122,255),   L'i', "iCloud"   },
    };
    for (auto& e : t) {
        if (url.find(e.key) != std::string::npos) {
            bg = e.c; letter = std::wstring(1, e.l); name = e.n; return;
        }
    }
    // 兜底：取主机名首字符
    bg = CLR_ACCENT; name = "WebDAV";
    size_t p = url.find("//");
    if (p != std::string::npos) {
        size_t h = p + 2;
        while (h < url.size() && url[h] != '/' && url[h] != ':' && url[h] != '.') {
            wchar_t wc = (wchar_t)url[h];
            letter = std::wstring(1, wc);
            break;
        }
    }
    if (letter.empty()) letter = L"W";
}

static void relayout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right;
    g_cards.clear();
    if (!g_cfg) return;
    {
        extern void fslog(const char* fmt, ...);
        fslog("relayout: cards=%d", (int)g_cfg->servers().size());
    }
    auto& sv = g_cfg->servers();
    int y = HEADER_H + 6;
    int cardW = W - SIDEBAR_W - MARGIN * 2;
    int cardX = SIDEBAR_W + MARGIN;
    for (int i = 0; i < (int)sv.size(); ++i) {
        Card c;
        c.idx = i;
        c.rc = { cardX, y, cardX + cardW, y + CARD_H };
        matchLogoByUrl(sv[i].url, c.logoBg, c.logoLetter, c.logoName);
        // 右侧按钮（挂载/卸载进行中时无按钮）
        if (!isBusy(i)) {
            int bx = c.rc.right - 16;
            auto mk = [&](int w, int cmd) {
                CardBtn b;
                b.cardIdx = i; b.cmd = cmd;
                b.rc = { bx - w, y + CARD_H / 2 - 16, bx, y + CARD_H / 2 + 16 };
                bx -= w + 10;
                c.btns.push_back(b);
            };
            mk(64, isMounted(i) ? IDM_UNMOUNT : IDM_MOUNT); // 主按钮
            mk(64, IDM_OPEN);
            mk(36, IDM_EDIT); // ⋯ 菜单入口
        }
        g_cards.push_back(c);
        y += CARD_H + CARD_GAP;
    }
}

static void drawAppIconOnDC(HDC dc, int x, int y, int size) {
    RECT rc = { x, y, x + size, y + size };
    fillRound(dc, rc, size / 4, brAccent);
    HFONT old = (HFONT)SelectObject(dc, fBadge);
    SetTextColor(dc, CLR_TEXT);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, L"W", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

// 侧边栏导航图标（纯 GDI 绘制）
static void drawNavIcon(HDC dc, int kind, int cx, int cy, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, 2, c);
    HPEN op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (kind == 0) {
        // 驱动器：圆角矩形 + 中间分隔线 + 右侧小灯
        RoundRect(dc, cx - 11, cy - 8, cx + 11, cy + 8, 6, 6);
        MoveToEx(dc, cx - 11, cy + 1, nullptr);
        LineTo(dc, cx + 11, cy + 1);
        SelectObject(dc, op); DeleteObject(pen);
        SelectObject(dc, ob);
        HBRUSH nb = solidBrush(c);
        HBRUSH ob2 = (HBRUSH)SelectObject(dc, nb);
        HPEN np = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx + 3, cy - 6, cx + 8, cy - 1);
        SelectObject(dc, np); SelectObject(dc, ob2); DeleteObject(nb);
        return;
    } else if (kind == 1) {
        // 设置：三条滑块
        MoveToEx(dc, cx - 11, cy - 7, nullptr); LineTo(dc, cx + 11, cy - 7);
        MoveToEx(dc, cx - 11, cy, nullptr);     LineTo(dc, cx + 11, cy);
        MoveToEx(dc, cx - 11, cy + 7, nullptr); LineTo(dc, cx + 11, cy + 7);
        SelectObject(dc, ob);
        HBRUSH nb = solidBrush(c);
        HBRUSH ob2 = (HBRUSH)SelectObject(dc, nb);
        HPEN np = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - 9, cy - 10, cx - 3, cy - 4);
        Ellipse(dc, cx + 3, cy - 3, cx + 9, cy + 3);
        Ellipse(dc, cx - 3, cy + 4, cx + 3, cy + 10);
        SelectObject(dc, np); SelectObject(dc, ob2); DeleteObject(nb);
        SelectObject(dc, op); DeleteObject(pen);
        return;
    } else {
        // 关于：圆 + i
        Ellipse(dc, cx - 11, cy - 11, cx + 11, cy + 11);
        SelectObject(dc, op); DeleteObject(pen);
        SelectObject(dc, ob);
        HBRUSH nb = solidBrush(c);
        HBRUSH ob2 = (HBRUSH)SelectObject(dc, nb);
        HPEN np = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - 1, cy - 6, cx + 2, cy - 3);
        SelectObject(dc, np); SelectObject(dc, ob2); DeleteObject(nb);
        HPEN p2 = CreatePen(PS_SOLID, 2, c);
        HPEN op2 = (HPEN)SelectObject(dc, p2);
        MoveToEx(dc, cx, cy - 1, nullptr);
        LineTo(dc, cx, cy + 6);
        SelectObject(dc, op2); DeleteObject(p2);
        return;
    }
    SelectObject(dc, op); DeleteObject(pen);
    SelectObject(dc, ob);
}

static void paintMain(HWND hwnd, HDC dc) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    // 双缓冲
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HBITMAP oldBmp = (HBITMAP)SelectObject(mem, bmp);
    // 背景：微妙的径向渐变从左上角
    fillGradientV(mem, rc, RGB(15, 17, 23), RGB(10, 11, 17));

    // ================= 自绘标题栏（深色 + 可随窗口裁剪为圆角） =================
    {
        RECT tb = { 0, 0, W, TITLE_H };
        FillRect(mem, &tb, solidBrush(RGB(17, 19, 26)));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(33, 37, 49));
        HPEN op = (HPEN)SelectObject(mem, pen);
        MoveToEx(mem, 0, TITLE_H - 1, nullptr);
        LineTo(mem, W, TITLE_H - 1);
        SelectObject(mem, op); DeleteObject(pen);

        drawAppIconOnDC(mem, 14, (TITLE_H - 20) / 2, 20);
        drawText(mem, L"Wddisk", { 42, 0, 100, TITLE_H }, fBtn, CLR_TEXT,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        drawText(mem, L"WebDAV 网络磁盘", { 90, 0, 260, TITLE_H }, fSub, CLR_SUB,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        g_rcCloseBtn = { W - 46, 0, W, TITLE_H };
        g_rcMinBtn   = { W - 92, 0, W - 46, TITLE_H };
        // 最小化按钮
        if (g_hoverTitle == 0) FillRect(mem, &g_rcMinBtn, solidBrush(RGB(38, 42, 54)));
        {
            int cx = (g_rcMinBtn.left + g_rcMinBtn.right) / 2, cy = TITLE_H / 2;
            HPEN p2 = CreatePen(PS_SOLID, 1, CLR_TEXT);
            HPEN o2 = (HPEN)SelectObject(mem, p2);
            MoveToEx(mem, cx - 5, cy, nullptr); LineTo(mem, cx + 5, cy);
            SelectObject(mem, o2); DeleteObject(p2);
        }
        // 关闭按钮
        if (g_hoverTitle == 1) FillRect(mem, &g_rcCloseBtn, solidBrush(RGB(196, 43, 28)));
        {
            int cx = (g_rcCloseBtn.left + g_rcCloseBtn.right) / 2, cy = TITLE_H / 2;
            HPEN p2 = CreatePen(PS_SOLID, 1, CLR_TEXT);
            HPEN o2 = (HPEN)SelectObject(mem, p2);
            MoveToEx(mem, cx - 5, cy - 5, nullptr); LineTo(mem, cx + 5, cy + 5);
            MoveToEx(mem, cx + 5, cy - 5, nullptr); LineTo(mem, cx - 5, cy + 5);
            SelectObject(mem, o2); DeleteObject(p2);
        }
    }
    // 以下所有内容坐标整体下移 TITLE_H（鼠标事件里同样偏移）
    SetViewportOrgEx(mem, 0, TITLE_H, nullptr);
    H -= TITLE_H;

    // ================= 左侧边栏 =================
    RECT sidebar = { 0, 0, SIDEBAR_W, H };
    FillRect(mem, &sidebar, solidBrush(RGB(16, 18, 25)));
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(34, 38, 50));
        HPEN op = (HPEN)SelectObject(mem, pen);
        MoveToEx(mem, SIDEBAR_W - 1, 0, nullptr);
        LineTo(mem, SIDEBAR_W - 1, H);
        SelectObject(mem, op); DeleteObject(pen);
    }
    // 品牌 logo
    {
        int LX = (SIDEBAR_W - 44) / 2, LY = 18;
        RECT lr = { LX, LY, LX + 44, LY + 44 };
        fillGradientV(mem, lr, RGB(132, 170, 255), RGB(50, 92, 220));
        HFONT oldF = (HFONT)SelectObject(mem, fTitle);
        SetTextColor(mem, RGB(255, 255, 255));
        SetBkMode(mem, TRANSPARENT);
        DrawTextW(mem, L"W", -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, oldF);
    }
    // 导航项
    {
        const wchar_t* navLabels[3] = { L"驱动器", L"设置", L"关于" };
        for (int i = 0; i < 3; ++i) {
            RECT nr = { 10, 92 + i * 74, SIDEBAR_W - 10, 92 + i * 74 + 66 };
            g_rcNav[i] = nr;
            bool active = (g_page == i);
            bool hov = (g_hoverNav == i);
            if (active) fillRound(mem, nr, 10, solidBrush(RGB(31, 42, 66)));
            else if (hov) fillRound(mem, nr, 10, solidBrush(RGB(24, 27, 37)));
            if (active) {
                RECT bar = { nr.left, nr.top + 16, nr.left + 3, nr.bottom - 16 };
                fillRound(mem, bar, 1, brAccent);
            }
            COLORREF ic = active ? RGB(135, 172, 255) : (hov ? CLR_TEXT : RGB(126, 134, 152));
            drawNavIcon(mem, i, (nr.left + nr.right) / 2, nr.top + 22, ic);
            drawText(mem, navLabels[i], { nr.left, nr.top + 36, nr.right, nr.bottom - 2 },
                fSub, active ? CLR_TEXT : (hov ? CLR_TEXT : CLR_SUB),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    // 侧边栏底部：网络状态 + 版本
    {
        int dy = H - 74;
        HBRUSH nb = solidBrush(g_onlineState ? CLR_GREEN : RGB(240, 190, 80));
        HBRUSH ob = (HBRUSH)SelectObject(mem, nb);
        HPEN op = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
        Ellipse(mem, SIDEBAR_W / 2 - 4, dy, SIDEBAR_W / 2 + 4, dy + 8);
        SelectObject(mem, op); SelectObject(mem, ob); DeleteObject(nb);
        drawText(mem, L"1.0", { 0, dy + 14, SIDEBAR_W, dy + 32 }, fTiny, RGB(96, 104, 122),
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ================= 内容区页头 =================
    int cx0 = SIDEBAR_W;
    fillGradientV(mem, { cx0, 0, W, HEADER_H }, RGB(20, 23, 33), RGB(15, 17, 23));
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(36, 40, 52));
        HPEN op = (HPEN)SelectObject(mem, pen);
        MoveToEx(mem, cx0, HEADER_H, nullptr);
        LineTo(mem, W, HEADER_H);
        SelectObject(mem, op); DeleteObject(pen);
    }
    {
        const wchar_t* pageTitle[3] = { L"驱动器", L"设置", L"关于" };
        drawText(mem, pageTitle[g_page], { cx0 + MARGIN, 14, W - 320, 48 }, fTitle, CLR_TEXT);
        std::wstring sub;
        if (g_page == PAGE_DRIVES) {
            int mountedCount = 0;
            for (auto& kv : g_mounted) if (kv.second) ++mountedCount;
            wchar_t b[128];
            swprintf(b, 128, L"%d 个服务器 · %d 个已挂载", (int)g_cfg->servers().size(), mountedCount);
            sub = b;
        } else if (g_page == PAGE_SETTINGS) {
            sub = L"启动行为与本地缓存";
        } else {
            sub = L"版本与组件信息";
        }
        drawText(mem, sub, { cx0 + MARGIN, 44, W - 320, 68 }, fSub, CLR_SUB);
    }
    // 添加按钮（仅驱动器页）
    bool hasCards = !g_cfg->servers().empty();
    if (g_page == PAGE_DRIVES) {
        g_rcAdd = { W - MARGIN - 140, 24, W - MARGIN, 60 };
        if (g_hoverAdd) {
            fillGradientV(mem, g_rcAdd, RGB(120, 160, 255), RGB(77, 132, 255));
            strokeRound(mem, g_rcAdd, 9, RGB(200, 220, 255), 1);
        } else {
            fillGradientV(mem, g_rcAdd, RGB(105, 145, 255), RGB(70, 110, 230));
            strokeRound(mem, g_rcAdd, 9, RGB(140, 170, 240), 1);
        }
        drawText(mem, L"＋ 添加服务器", g_rcAdd, fBtn, CLR_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        g_rcAdd = {};
    }

    // ================= 页面内容 =================
    if (g_page == PAGE_DRIVES) {
    if (!hasCards) {
        // 空状态：居中卡片 + logo + 引导文案
        int cw = 440, ch = 210;
        int areaCx = cx0 + (W - cx0) / 2;
        int areaCy = HEADER_H + (H - HEADER_H - FOOTER_H) / 2 - ch / 2;
        RECT box = { areaCx - cw / 2, areaCy, areaCx + cw / 2, areaCy + ch };
        fillRound(mem, box, 18, solidBrush(RGB(21, 24, 33)));
        strokeRound(mem, box, 18, RGB(42, 47, 60), 1);
        RECT lr = { areaCx - 30, box.top + 34, areaCx + 30, box.top + 94 };
        fillGradientV(mem, lr, RGB(126, 166, 255), RGB(56, 96, 218));
        strokeRound(mem, lr, 14, RGB(180, 200, 240), 1);
        {
            HFONT old = (HFONT)SelectObject(mem, fTitle);
            SetTextColor(mem, RGB(255, 255, 255));
            SetBkMode(mem, TRANSPARENT);
            DrawTextW(mem, L"W", -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, old);
        }
        drawText(mem, L"还没有服务器配置", { box.left, box.top + 106, box.right, box.top + 136 },
            fName, CLR_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawText(mem, L"点击右上角「＋ 添加服务器」接入你的网盘", { box.left, box.top + 138, box.right, box.top + 162 },
            fSub, CLR_SUB, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        drawText(mem, L"支持 123 云盘 / 阿里云盘 / 坚果云 / 群晖 / Nextcloud 等 WebDAV 服务",
            { box.left, box.top + 168, box.right, box.top + 190 },
            fSub, RGB(96, 103, 120), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    for (auto& c : g_cards) {
        auto& s = g_cfg->servers()[c.idx];
        bool mounted = isMounted(c.idx);
        bool hov = (g_hoverCard == c.idx);

        // --- 多层阴影：悬停时阴影更深 + 抬升 ---
        int hoverLift = hov ? 4 : 0;
        drawCardShadow(mem, c.rc, hoverLift);

        RECT cardRc = c.rc;
        OffsetRect(&cardRc, 0, -hoverLift);  // 悬停时卡片上移

        fillRound(mem, cardRc, 14, hov ? brCardHi : brCard);
        // 顶部 1px 内高光，提升卡片质感
        {
            RECT tl = { cardRc.left + 16, cardRc.top + 1, cardRc.right - 16, cardRc.top + 2 };
            fillRound(mem, tl, 1, solidBrush(hov ? RGB(56, 63, 80) : RGB(46, 52, 66)));
        }
        // 选中态主色竖条；已挂载但未选中时显示绿色竖条
        if (g_sel == c.idx || mounted) {
            RECT edge = { cardRc.left, cardRc.top + 14, cardRc.left + 3, cardRc.bottom - 14 };
            fillRound(mem, edge, 1, g_sel == c.idx ? brAccent : solidBrush(CLR_GREEN));
        }
        strokeRound(mem, cardRc, 14, (g_sel == c.idx) ? CLR_ACCENT : CLR_BORDER, g_sel == c.idx ? 2 : 1);

        // --- 左侧 logo（52x52 圆角 + 字母/汉字，主色渐变） ---
        {
            int LX = cardRc.left + 18;
            int LY = cardRc.top + (CARD_H - 52) / 2;
            RECT lr = { LX, LY, LX + 52, LY + 52 };
            // logo 用渐变填充
            COLORREF topC = c.logoBg;
            COLORREF botC = c.logoBg;
            // logoBg 加深 30%
            int r = GetRValue(c.logoBg), gg = GetGValue(c.logoBg), b = GetBValue(c.logoBg);
            topC = RGB(std::min(255, r + 30), std::min(255, gg + 30), std::min(255, b + 30));
            botC = RGB(std::max(0, r - 40),   std::max(0, gg - 40),   std::max(0, b - 40));
            fillGradientV(mem, lr, topC, botC);
            // 亮化主色做细边框（避免刺眼纯白）
            strokeRound(mem, lr, 12, RGB(std::min(255, r + 70), std::min(255, gg + 70), std::min(255, b + 70)), 1);
            HFONT old = (HFONT)SelectObject(mem, fTitle);
            SetTextColor(mem, RGB(255,255,255));
            SetBkMode(mem, TRANSPARENT);
            DrawTextW(mem, c.logoLetter.c_str(), -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, old);
            // 盘符徽章角标
            if (!s.mountPoint.empty()) {
                RECT br = { lr.right - 24, lr.bottom - 24, lr.right - 2, lr.bottom - 2 };
                fillRound(mem, br, 6, solidBrush(mounted ? RGB(52,84,168) : RGB(30,34,44)));
                wchar_t drv = (wchar_t)toupper((unsigned char)s.mountPoint[0]);
                drawText(mem, std::wstring(1, drv), br, fTiny, mounted ? RGB(170,205,255) : RGB(150,158,175),
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        // 文本区（logo 右侧 16px 起）
        int tx = cardRc.left + 18 + 52 + 16;
        drawText(mem, w(s.name.c_str()), { tx, cardRc.top + 14, cardRc.right - 320, cardRc.top + 40 }, fName, CLR_TEXT);
        drawText(mem, w(s.url.c_str()), { tx, cardRc.top + 42, cardRc.right - 320, cardRc.top + 64 }, fSub, CLR_SUB);

        // 状态 pill（圆角标签）+ 用户名
        int sy = cardRc.top + 66;
        bool busy = isBusy(c.idx);
        std::wstring stat = busy ? L"处理中" : mounted ? L"已挂载" : L"未挂载";
        std::wstring usr = w(s.username.c_str());
        COLORREF pillBg   = busy ? RGB(58, 47, 20) : mounted ? RGB(22, 56, 40) : RGB(34, 37, 46);
        COLORREF pillFg   = busy ? RGB(240, 190, 80) : mounted ? RGB(110, 220, 160) : RGB(140, 148, 165);
        COLORREF dotColor = busy ? RGB(240, 190, 80) : mounted ? CLR_GREEN : RGB(90, 96, 110);
        {
            // 计算 pill 宽度
            HFONT oldF = (HFONT)SelectObject(mem, fSub);
            SIZE sz; GetTextExtentPoint32W(mem, stat.c_str(), (int)stat.size(), &sz);
            SelectObject(mem, oldF);
            int pw = sz.cx + 34, ph = 22;
            RECT pill = { tx, sy, tx + pw, sy + ph };
            fillRound(mem, pill, 11, solidBrush(pillBg));
            HBRUSH dotBr = solidBrush(dotColor);
            HBRUSH oldBr = (HBRUSH)SelectObject(mem, dotBr);
            HPEN oldPen = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, pill.left + 9, sy + 7, pill.left + 17, sy + 15);
            SelectObject(mem, oldPen);
            SelectObject(mem, oldBr);
            DeleteObject(dotBr);
            drawText(mem, stat, { pill.left + 21, sy, pill.right - 6, sy + ph }, fSub, pillFg,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            // 用户名 / 挂载点
            std::wstring extra = mounted && !s.mountPoint.empty() ? (w(s.mountPoint.c_str()) + L" · " + usr)
                                                                 : usr;
            if (!extra.empty() && !busy)
                drawText(mem, extra, { pill.right + 10, sy, cardRc.right - 320, sy + ph },
                    fSub, CLR_SUB, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // 按钮：主色按钮用渐变填充 + 图标符号
        for (auto& b : c.btns) {
            bool hb = (g_hoverBtn >= 0 && g_hoverBtn == (int)(&b - &c.btns[0]) && g_hoverCard == c.idx);
            const wchar_t* txt =
                b.cmd == IDM_MOUNT ? L"↑ 挂载" : b.cmd == IDM_UNMOUNT ? L"↓ 卸载" :
                b.cmd == IDM_OPEN ? L"↗ 打开" : L"⋯";
            if (b.cmd == IDM_MOUNT || (b.cmd == IDM_OPEN && mounted)) {
                // 主色按钮：渐变 + 细白边
                COLORREF topC = hb ? RGB(112, 152, 255) : RGB(105, 145, 255);
                COLORREF botC = hb ? RGB(70, 110, 230) : RGB(77, 132, 255);
                fillGradientV(mem, b.rc, topC, botC);
                strokeRound(mem, b.rc, 8, RGB(200, 220, 255), 1);  // subtle highlight border
                drawText(mem, txt, b.rc, fBtn, CLR_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                fillRound(mem, b.rc, 8, hb ? brCardHi : brCard);
                strokeRound(mem, b.rc, 8, CLR_BORDER);
                drawText(mem, txt, b.rc, fBtn, CLR_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
    }
    } // end PAGE_DRIVES
    else if (g_page == PAGE_SETTINGS) {
        int x = cx0 + MARGIN, w = W - cx0 - MARGIN * 2;
        int y = HEADER_H + 20;

        // 卡片容器
        RECT panel = { x, y, x + w, y + 316 };
        fillRound(mem, panel, 14, brCard);
        strokeRound(mem, panel, 14, CLR_BORDER, 1);

        // —— 开机自启 ——
        {
            drawText(mem, L"开机自启", { x + 24, y + 20, x + w - 200, y + 46 }, fName, CLR_TEXT);
            drawText(mem, L"登录 Windows 后自动在托盘启动 Wddisk", { x + 24, y + 46, x + w - 200, y + 68 },
                fSub, CLR_SUB);
            // 开关：右侧
            RECT sw = { x + w - 92, y + 28, x + w - 24, y + 58 };
            g_rcAutoStart = sw;
            bool on = g_autoStartCached;
            if (on) fillGradientV(mem, sw, RGB(105, 145, 255), RGB(70, 110, 230));
            else fillRound(mem, sw, 15, solidBrush(RGB(48, 53, 66)));
            int knob = sw.top + 3, kd = 24;
            int kx = on ? (sw.right - kd - 3) : (sw.left + 3);
            fillRound(mem, { kx, knob, kx + kd, knob + kd }, 12, solidBrush(RGB(250, 252, 255)));
            // 状态文字放在开关左侧，避免与滑块重叠
            drawText(mem, on ? L"已开启" : L"已关闭",
                { sw.left - 84, sw.top, sw.left - 12, sw.bottom }, fSub,
                on ? CLR_GREEN : CLR_SUB, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        // 分隔线
        {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(36, 40, 52));
            HPEN op = (HPEN)SelectObject(mem, pen);
            MoveToEx(mem, x + 24, y + 92, nullptr); LineTo(mem, x + w - 24, y + 92);
            SelectObject(mem, op); DeleteObject(pen);
        }
        // —— 配置文件位置（账号数据跟随安装目录）——
        {
            drawText(mem, L"配置文件", { x + 24, y + 106, x + w - 220, y + 130 }, fName, CLR_TEXT);
            std::wstring cfgPath = configDir() + L"\\config.json";
            drawText(mem, cfgPath, { x + 24, y + 130, x + w - 24, y + 152 }, fSub, CLR_SUB,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            RECT b = { x + w - 118, y + 112, x + w - 24, y + 144 };
            g_rcOpenCache = b;
            fillRound(mem, b, 8, g_hoverOpenCache ? brCardHi : solidBrush(RGB(34, 38, 50)));
            strokeRound(mem, b, 8, CLR_BORDER);
            drawText(mem, L"打开所在目录", b, fBtn, CLR_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // 分隔线
        {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(36, 40, 52));
            HPEN op = (HPEN)SelectObject(mem, pen);
            MoveToEx(mem, x + 24, y + 168, nullptr); LineTo(mem, x + w - 24, y + 168);
            SelectObject(mem, op); DeleteObject(pen);
        }
        // —— 缓存 ——
        {
            drawText(mem, L"本地缓存", { x + 24, y + 182, x + w - 220, y + 206 }, fName, CLR_TEXT);
            drawText(mem, g_cacheInfo.empty() ? L"正在统计…" : g_cacheInfo,
                { x + 24, y + 206, x + w - 140, y + 228 }, fSub, CLR_SUB);
            RECT b2 = { x + w - 110, y + 188, x + w - 24, y + 220 };
            g_rcClearCache = b2;
            fillRound(mem, b2, 8, g_hoverClearCache ? solidBrush(RGB(72, 36, 40)) : solidBrush(RGB(48, 30, 34)));
            strokeRound(mem, b2, 8, RGB(96, 52, 58));
            drawText(mem, L"清空", b2, fBtn, RGB(255, 150, 150), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // 分隔线
        {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(36, 40, 52));
            HPEN op = (HPEN)SelectObject(mem, pen);
            MoveToEx(mem, x + 24, y + 244, nullptr); LineTo(mem, x + w - 24, y + 244);
            SelectObject(mem, op); DeleteObject(pen);
        }
        // —— 传输设置（只读展示）——
        drawText(mem, L"性能参数", { x + 24, y + 258, x + w - 200, y + 282 }, fName, CLR_TEXT);
        drawText(mem, L"分块 1 MB · 顺序预取 4 MB · 目录缓存 30 秒 · 失败重试 3 次（指数退避）",
            { x + 24, y + 282, x + w - 24, y + 304 }, fSub, CLR_SUB);
    }
    else if (g_page == PAGE_ABOUT) {
        int x = cx0 + MARGIN, w = W - cx0 - MARGIN * 2;
        int y = HEADER_H + 20;

        RECT panel = { x, y, x + w, y + 236 };
        fillRound(mem, panel, 14, brCard);
        strokeRound(mem, panel, 14, CLR_BORDER, 1);
        // logo
        {
            RECT lr = { x + 28, y + 24, x + 28 + 56, y + 24 + 56 };
            fillGradientV(mem, lr, RGB(132, 170, 255), RGB(50, 92, 220));
            HFONT old = (HFONT)SelectObject(mem, fTitle);
            SetTextColor(mem, RGB(255, 255, 255));
            SetBkMode(mem, TRANSPARENT);
            DrawTextW(mem, L"W", -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, old);
        }
        drawText(mem, L"Wddisk · WebDAV 网络磁盘", { x + 100, y + 26, x + w - 24, y + 54 }, fName, CLR_TEXT);
        drawText(mem, L"版本 1.0.0  ·  Windows x64", { x + 100, y + 54, x + w - 24, y + 76 }, fSub, CLR_SUB);
        drawText(mem, L"把任意 WebDAV / 云盘挂载为本地驱动器的原生客户端",
            { x + 100, y + 78, x + w - 24, y + 100 }, fSub, CLR_SUB);
        // 组件
        {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(36, 40, 52));
            HPEN op = (HPEN)SelectObject(mem, pen);
            MoveToEx(mem, x + 24, y + 118, nullptr); LineTo(mem, x + w - 24, y + 118);
            SelectObject(mem, op); DeleteObject(pen);
        }
        const wchar_t* rows[3][2] = {
            { L"文件系统", L"WinFsp 2.0（用户态文件系统框架）" },
            { L"传输协议", L"WebDAV / HTTP(S)，分块 Range 下载 + 后台上传队列" },
            { L"安全", L"Windows DPAPI 密码加密存储，支持自签名证书" },
        };
        for (int i = 0; i < 3; ++i) {
            int ry = y + 132 + i * 30;
            drawText(mem, rows[i][0], { x + 24, ry, x + 140, ry + 24 }, fSub, RGB(120, 128, 145));
            drawText(mem, rows[i][1], { x + 144, ry, x + w - 24, ry + 24 }, fSub, CLR_TEXT);
        }
    }

    // ---- 底部状态栏（仅内容区，侧边栏保持通高）----
    RECT foot = { SIDEBAR_W, H - FOOTER_H, W, H };
    FillRect(mem, &foot, solidBrush(RGB(19, 21, 28)));
    HPEN ln = CreatePen(PS_SOLID, 1, CLR_BORDER);
    HPEN oln = (HPEN)SelectObject(mem, ln);
    MoveToEx(mem, SIDEBAR_W, foot.top, nullptr); LineTo(mem, W, foot.top);
    SelectObject(mem, oln); DeleteObject(ln);

    int mountedCount = 0;
    for (auto& kv : g_mounted) if (kv.second) ++mountedCount;
    // 左侧：网络状态点 + 统计（内容区起点）
    {
        HBRUSH nb = solidBrush(g_onlineState ? CLR_GREEN : RGB(240, 190, 80));
        HBRUSH ob = (HBRUSH)SelectObject(mem, nb);
        HPEN op = (HPEN)SelectObject(mem, GetStockObject(NULL_PEN));
        int dy = (foot.top + foot.bottom) / 2;
        Ellipse(mem, SIDEBAR_W + MARGIN, dy - 4, SIDEBAR_W + MARGIN + 8, dy + 4);
        SelectObject(mem, op);
        SelectObject(mem, ob);
        DeleteObject(nb);
    }
    wchar_t buf[160];
    swprintf(buf, 160, L"%d 个配置 · 已挂载 %d 个", (int)g_cfg->servers().size(), mountedCount);
    std::wstring left = buf;
    if (!g_onlineState) left = L"网络异常，自动重试中  ·  " + left;
    drawText(mem, left, { SIDEBAR_W + MARGIN + 16, foot.top, 560, foot.bottom }, fSub,
        g_onlineState ? CLR_SUB : RGB(240, 190, 80));
    // 右侧优先显示传输进度，其次操作状态
    std::wstring right = g_xferText.empty() ? g_status : g_xferText;
    drawText(mem, right, { 520, foot.top, W - MARGIN, foot.bottom }, fSub,
        g_xferText.empty() ? CLR_TEXT : RGB(150, 190, 255), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // 恢复绘制原点后再拷贝（否则 BitBlt 源坐标也会带偏移，画面整体上移）
    SetViewportOrgEx(mem, 0, 0, nullptr);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------- 托盘 ----------------
static HICON makeAppIconEx(int S) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, S, S);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    RECT rc = { 0, 0, S, S };
    fillRound(mem, rc, 8, brAccent);
    HFONT oldF = (HFONT)SelectObject(mem, fIcon);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, CLR_TEXT);
    DrawTextW(mem, L"W", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, oldF);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    HDC mdc = CreateCompatibleDC(nullptr);
    HBITMAP oldM = (HBITMAP)SelectObject(mdc, mask);
    PatBlt(mdc, 0, 0, S, S, BLACKNESS);
    SelectObject(mdc, oldM);
    DeleteDC(mdc);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = bmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(bmp);
    return icon;
}
static HICON makeAppIcon() { return makeAppIconEx(32); }

static void addTray(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_hAppIcon;
    wcscpy_s(g_nid.szTip, L"Wddisk · WebDAV 网络磁盘");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void removeTray() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

static void showTrayMenu(HWND hwnd) {
    POINT p; GetCursorPos(&p);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, TRAY_OPEN, L"打开 Wddisk");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, TRAY_MOUNTALL, L"挂载全部");
    AppendMenuW(m, MF_STRING, TRAY_UNMOUNTALL, L"卸载全部");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, TRAY_EXIT, L"退出");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

static bool g_mainTracked = false;

// ---------------- 主窗口过程 ----------------
static UINT g_msgTaskbarCreated = 0;
static bool g_quitting = false;
#define WM_QUIT_DONE (WM_APP + 5)

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        addTray(hwnd); // Explorer 重启后恢复托盘图标
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        // 优先用 exe 内嵌的资源图标（app.rc → app.ico），失败时回退到运行时绘制
        HINSTANCE hi = GetModuleHandleW(nullptr);
        auto loadIcon = [hi](int sz) -> HICON {
            HICON h = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
            return h ? h : nullptr;
        };
        g_hAppIconBig   = loadIcon(48);
        g_hAppIcon      = loadIcon(32);
        g_hAppIconSmall = loadIcon(16);
        if (!g_hAppIconBig)   g_hAppIconBig   = makeAppIconEx(48);
        if (!g_hAppIcon)      g_hAppIcon      = makeAppIconEx(32);
        if (!g_hAppIconSmall) g_hAppIconSmall = makeAppIconEx(16);
        // 应用到主窗口（标题栏 / 任务栏 / Alt+Tab）
        SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)g_hAppIconBig);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hAppIconSmall);
        addTray(hwnd);
        relayout(hwnd);
        g_autoStartCached = autostartEnabled();
        g_autoStartChk = GetTickCount();
        // 进度轮询定时器（传输进度 / 网络状态 → 状态栏）
        SetTimer(hwnd, 1, 500, nullptr);
        // 启动时自动挂载勾选了"自动挂载"的配置
        for (int i = 0; i < (int)g_cfg->servers().size(); ++i)
            if (g_cfg->servers()[i].autoMount && !isMounted(i)) doMount(i, hwnd);
        return 0;
    }
    case WM_TIMER: {
        if (wp != 1) break;
        auto& pr = WinHttpClient::progress();
        int phase = pr.phase.load();
        std::wstring txt;
        if (phase == 1 || phase == 2) {
            long long done = pr.done.load(), total = pr.total.load();
            std::string nm;
            { std::lock_guard<std::mutex> l(pr.nameMtx); nm = pr.name; }
            size_t slash = nm.rfind('/');
            if (slash != std::string::npos) nm = nm.substr(slash + 1);
            // URL 解码
            std::string dec;
            for (size_t i = 0; i < nm.size(); ++i) {
                if (nm[i] == '%' && i + 2 < nm.size()) {
                    char b[3] = { nm[i + 1], nm[i + 2], 0 };
                    dec += (char)strtoul(b, nullptr, 16); i += 2;
                } else dec += nm[i];
            }
            wchar_t buf[256];
            int pct = total > 0 ? (int)(done * 100 / total) : 0;
            swprintf(buf, 256, phase == 1 ? L"↑ 正在上传 %s  %d%%" : L"↓ 正在下载 %s  %d%%",
                w(dec.c_str()).c_str(), pct);
            txt = buf;
        }
        bool onlineNow = WinHttpClient::online().load();
        // 设置页：缓存统计（每 2 秒刷新一次）
        static DWORD lastCacheCalc = 0;
        if (g_page == PAGE_SETTINGS && GetTickCount() - lastCacheCalc > 2000) {
            lastCacheCalc = GetTickCount();
            std::wstring dir = configDir() + L"\\cache";
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            unsigned long long bytes = 0;
            int files = 0;
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                        ++files;
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            wchar_t b[128];
            if (bytes >= (1ULL << 20))
                swprintf(b, 128, L"%d 个文件 · %.1f MB", files, (double)bytes / (1024 * 1024));
            else
                swprintf(b, 128, L"%d 个文件 · %llu KB", files, bytes / 1024);
            g_cacheInfo = b;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (txt != g_xferText || onlineNow != g_onlineState) {
            g_xferText = txt;
            g_onlineState = onlineNow;
            RECT rc; GetClientRect(hwnd, &rc);
            RECT foot = { 0, rc.bottom - FOOTER_H, rc.right, rc.bottom };
            InvalidateRect(hwnd, &foot, FALSE);
        }
        break;
    }
    case WM_OP_DONE: onOpDone(hwnd); return 0;
    case WM_QUIT_DONE:
        removeTray();
        DestroyWindow(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paintMain(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: {
        applyRoundRegion(hwnd);
        relayout(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        if (p.y >= 0 && p.y < TITLE_H) {
            // 标题栏按钮交给客户区处理，其余区域拖动窗口
            if (ptIn(p, g_rcMinBtn) || ptIn(p, g_rcCloseBtn)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (p.y < TITLE_H) {
            int ht = ptIn(p, g_rcCloseBtn) ? 1 : (ptIn(p, g_rcMinBtn) ? 0 : -1);
            if (ht != g_hoverTitle) { g_hoverTitle = ht; InvalidateRect(hwnd, nullptr, FALSE); }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            if (!g_mainTracked) {
                TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&t); g_mainTracked = true;
            }
            return 0;
        }
        if (g_hoverTitle != -1) { g_hoverTitle = -1; InvalidateRect(hwnd, nullptr, FALSE); }
        p.y -= TITLE_H;
        int hc = -1, hb = -1, hn = -1;
        bool ha = (g_page == PAGE_DRIVES) && ptIn(p, g_rcAdd);
        bool has = (g_page == PAGE_SETTINGS) && ptIn(p, g_rcAutoStart);
        bool hoc = (g_page == PAGE_SETTINGS) && ptIn(p, g_rcOpenCache);
        bool hcc = (g_page == PAGE_SETTINGS) && ptIn(p, g_rcClearCache);
        // 侧边栏导航悬停
        for (int i = 0; i < 3; ++i) if (ptIn(p, g_rcNav[i])) { hn = i; break; }
        if (!ha && !has && !hoc && !hcc && hn < 0 && g_page == PAGE_DRIVES) {
            for (size_t i = 0; i < g_cards.size(); ++i) {
                if (ptIn(p, g_cards[i].rc)) {
                    hc = (int)i;
                    for (size_t j = 0; j < g_cards[i].btns.size(); ++j)
                        if (ptIn(p, g_cards[i].btns[j].rc)) { hb = (int)j; break; }
                    break;
                }
            }
        }
        if (hc != g_hoverCard || hb != g_hoverBtn || ha != g_hoverAdd || has != g_hoverAutoStart ||
            hn != g_hoverNav || hoc != g_hoverOpenCache || hcc != g_hoverClearCache) {
            g_hoverCard = hc; g_hoverBtn = hb; g_hoverAdd = ha; g_hoverAutoStart = has;
            g_hoverNav = hn; g_hoverOpenCache = hoc; g_hoverClearCache = hcc;
            InvalidateRect(hwnd, nullptr, FALSE);
            SetCursor(LoadCursorW(nullptr, (ha || has || hoc || hcc || hn >= 0 || hb >= 0) ? IDC_HAND : IDC_ARROW));
        }
        if (!g_mainTracked) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t); g_mainTracked = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        bool need = false;
        if (g_hoverCard >= 0 || g_hoverBtn >= 0 || g_hoverAdd || g_hoverNav >= 0 ||
            g_hoverOpenCache || g_hoverClearCache) {
            g_hoverCard = -1; g_hoverBtn = -1; g_hoverAdd = false;
            g_hoverNav = -1; g_hoverOpenCache = false; g_hoverClearCache = false;
            need = true;
        }
        if (g_hoverTitle != -1) { g_hoverTitle = -1; need = true; }
        if (need) InvalidateRect(hwnd, nullptr, FALSE);
        g_mainTracked = false;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (p.y < TITLE_H) {
            if (ptIn(p, g_rcCloseBtn)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            else if (ptIn(p, g_rcMinBtn)) ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        p.y -= TITLE_H;
        {
            extern void fslog(const char* fmt, ...);
            fslog("ui: click %d,%d page=%d", p.x, p.y, g_page);
        }
        // 侧边栏导航
        for (int i = 0; i < 3; ++i) {
            if (ptIn(p, g_rcNav[i])) {
                if (g_page != i) {
                    g_page = i;
                    g_hoverCard = g_hoverBtn = -1; g_hoverNav = i;
                    if (g_page == PAGE_SETTINGS) {
                        g_autoStartCached = autostartEnabled();
                        g_autoStartChk = GetTickCount();
                    }
                    relayout(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }
        // 设置页交互
        if (g_page == PAGE_SETTINGS) {
            if (ptIn(p, g_rcAutoStart)) {
                bool now = !autostartEnabled();
                if (setAutostart(now)) {
                    g_autoStartCached = now;
                    g_autoStartChk = GetTickCount();
                    g_status = now ? L"已开启开机自启" : L"已关闭开机自启";
                } else {
                    g_status = L"设置开机自启失败（可能被安全软件拦截）";
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ptIn(p, g_rcOpenCache)) {
                std::wstring dir = configDir();
                ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (ptIn(p, g_rcClearCache)) {
                std::wstring dir = configDir() + L"\\cache";
                WIN32_FIND_DATAW fd;
                std::wstring pat = dir + L"\\*";
                HANDLE h = FindFirstFileW(pat.c_str(), &fd);
                int n = 0;
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
                            ++n;
                        }
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                }
                wchar_t b[128];
                swprintf(b, 128, L"已清理 %d 个缓存文件", n);
                g_status = b;
                g_cacheInfo = L"0 KB（已清空）";
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        // 卡片按钮
        int ci, cmd;
        if (inCardButton(p, &ci, &cmd)) {
            {
                extern void fslog(const char* fmt, ...);
                fslog("ui: cardbtn ci=%d cmd=%d", ci, cmd);
            }
            g_sel = ci;
            switch (cmd) {
            case IDM_MOUNT: doMount(ci, hwnd); break;
            case IDM_UNMOUNT: doUnmount(ci, hwnd); break;
            case IDM_OPEN: openInExplorer(hwnd, ci); break;
            case IDM_EDIT: {
                // ⋯ 菜单
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, IDM_OPEN2, L"打开");
                AppendMenuW(m, MF_STRING, IDM_EDIT2, L"编辑…");
                AppendMenuW(m, MF_STRING, IDM_MOUNT2, isMounted(ci) ? L"卸载" : L"挂载");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, IDM_DEL2, L"删除");
                ClientToScreen(hwnd, &p);
                SetForegroundWindow(hwnd);
                int r = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD, p.x, p.y, 0, hwnd, nullptr);
                DestroyMenu(m);
                switch (r) {
                case IDM_OPEN2: openInExplorer(hwnd, ci); break;
                case IDM_EDIT2: if (openEditor(hwnd, ci)) { relayout(hwnd); InvalidateRect(hwnd, nullptr, TRUE); } break;
                case IDM_MOUNT2: if (isMounted(ci)) doUnmount(ci, hwnd); else doMount(ci, hwnd); break;
                case IDM_DEL2:
                    if (isMounted(ci)) doUnmount(ci, hwnd);
                    g_cfg->servers().erase(g_cfg->servers().begin() + ci);
                    g_cfg->save();
                    g_sel = -1;
                    g_status = L"已删除";
                    relayout(hwnd);
                    InvalidateRect(hwnd, nullptr, TRUE);
                    break;
                }
                break;
            }
            }
            return 0;
        }
        // 添加按钮
        if (ptIn(p, g_rcAdd)) {
            if (openEditor(hwnd, -1)) { relayout(hwnd); InvalidateRect(hwnd, nullptr, TRUE); }
            return 0;
        }
        // 开机自启开关
        if (ptIn(p, g_rcAutoStart)) {
            bool now = !autostartEnabled();
            if (setAutostart(now)) g_status = now ? L"已开启开机自启" : L"已关闭开机自启";
            else g_status = L"设置开机自启失败（注册表写入被拒绝）";
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        // 卡片选择
        g_sel = -1;
        for (auto& c : g_cards) if (ptIn(p, c.rc)) { g_sel = c.idx; break; }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (p.y < TITLE_H) return 0;
        p.y -= TITLE_H;
        for (auto& c : g_cards)
            if (ptIn(p, c.rc)) {
                g_sel = c.idx;
                openInExplorer(hwnd, c.idx);
                InvalidateRect(hwnd, nullptr, FALSE);
                break;
            }
        return 0;
    }
    case WM_TRAY: {
        if (lp == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lp == WM_RBUTTONUP) {
            showTrayMenu(hwnd);
        }
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case TRAY_OPEN: ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); break;
        case TRAY_MOUNTALL:
            for (int i = 0; i < (int)g_cfg->servers().size(); ++i) if (!isMounted(i)) doMount(i, hwnd);
            break;
        case TRAY_UNMOUNTALL:
            for (int i = 0; i < (int)g_cfg->servers().size(); ++i)
                if (isMounted(i) && !isBusy(i)) doUnmount(i, hwnd);
            break;
        case TRAY_EXIT: {
            if (g_quitting) break;
            g_quitting = true;
            ShowWindow(hwnd, SW_HIDE);
            removeTray();
            std::thread([hwnd]() {
                // 后台排空上传队列并卸载，避免 UI 冻结
                for (auto& kv : g_mounted) { kv.second->unmount(); delete kv.second; }
                g_mounted.clear();
                PostMessageW(hwnd, WM_QUIT_DONE, 0, 0);
            }).detach();
            break;
        }
        }
        return 0;
    }
    case WM_CLOSE: {
        extern void fslog(const char* fmt, ...);
        fslog("wmclose: editDlg=%d quitting=%d", (int)(g_editDlg != nullptr), (int)g_quitting);
        // 编辑对话框打开时：聚焦对话框（主窗口此时是禁用的模态父窗口）
        if (g_editDlg) { SetForegroundWindow(g_editDlg); return 0; }
        // 正在退出中：等待后台卸载完成
        if (g_quitting) return 0;
        // 最小化到托盘
        ShowWindow(hwnd, SW_HIDE);
        if (!g_trayTipShown) {
            g_nid.uFlags = NIF_INFO;
            wcscpy_s(g_nid.szInfo, L"Wddisk 已在后台运行，双击托盘图标可重新打开");
            wcscpy_s(g_nid.szInfoTitle, L"Wddisk");
            g_nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_trayTipShown = true;
        }
        return 0;
    }
    case WM_DESTROY:
        removeTray();
        PostQuitMessage(0);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize = { 680, 480 };
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    CoInitialize(nullptr);

    // Set process DPI awareness before creating any windows
    {
        HMODULE u = LoadLibraryW(L"user32.dll");
        typedef BOOL (WINAPI *SA)(DPI_AWARENESS_CONTEXT);
        SA sa = (SA)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (sa) { DPI_AWARENESS_CONTEXT v2 = (DPI_AWARENESS_CONTEXT)(intptr_t)(-4); (void)sa(v2); }
        FreeLibrary(u);
    }

    // 单实例：已有实例在运行时唤出其窗口后退出
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\WddiskSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS && !(lpCmdLine && wcsncmp(lpCmdLine, L"--mount", 7) == 0)) {
        // 唤出已有主窗口
        HWND old = FindWindowW(L"WddiskMain", nullptr);
        if (old) {
            PostMessageW(old, WM_TRAY, 0, WM_LBUTTONUP);
        }
        CloseHandle(hMutex);
        CoUninitialize();
        return 0;
    }

    // 配置（路径可能含中文，必须传宽字符路径给 ConfigStore）
    std::wstring cfgPath = configDir() + L"\\config.json";
    static std::string cfgPathU8 = u8(cfgPath);
    static std::wstring cfgPathW = cfgPath;
    static ConfigStore store(cfgPathU8, cfgPathW);
    store.setCrypto(dpapiEncrypt, dpapiDecrypt);
    g_cfg = &store;
    // 平滑升级：首次运行（从旧版明文配置升级）时触发 save() 写入密文
    // dpapi: 前缀的密文在 setCrypto→reload() 里已经正确解密，再次 save() 会重新加密（密码字段会变）
    // 纯明文（无前缀）也会在此处被 dpapiEncrypt 加密写回
    if (!store.servers().empty()) store.save();
    if (store.decryptFailed()) {
        fslog("app: DPAPI 解密失败（换用户/换机器），需重新输入密码");
        g_status = L"部分密码无法解密，请编辑服务器重新输入密码";
    }

    // 命令行模式: wddisk.exe --mount [序号]  挂载指定配置并驻留
    extern void fslog(const char* fmt, ...);
    fslog("app: --mount mode entered");
    bool trayMode = (lpCmdLine && wcsncmp(lpCmdLine, L"--tray", 6) == 0);
    if (lpCmdLine && wcsncmp(lpCmdLine, L"--mount", 7) == 0) {
        int idx = 0;
        const wchar_t* p = lpCmdLine + 7;
        while (*p == L' ') ++p;
        if (*p >= L'0' && *p <= L'9') idx = (int)(*p - L'0');
        if (idx < (int)g_cfg->servers().size()) {
            DavConfig& s = g_cfg->servers()[idx];
            std::wstring mp;
            if (s.mountPoint.size() >= 2 && s.mountPoint[1] == L':') mp = w(s.mountPoint.c_str());
            else {
                for (wchar_t c = L'Z'; c >= L'F'; --c) {
                    wchar_t root[4] = { c, L':', L'\\', 0 };
                    if (GetDriveTypeW(root) == DRIVE_NO_ROOT_DIR) { mp = std::wstring(1, c) + L":"; break; }
                }
                if (mp.empty()) { MessageBoxW(nullptr, L"没有可用盘符", L"Wddisk", MB_ICONERROR); return 1; }
                s.mountPoint = u8(mp);
                g_cfg->save();
            }
            DavFileSystem* fs = new DavFileSystem(s, configDir() + L"\\cache");
            std::string err;
            fslog("app: mounting idx=%d -> %s", idx, s.mountPoint.c_str());
            if (!fs->mount(mp, err)) {
                fslog("app: mount FAILED %s", err.c_str());
                std::wstring msg = w(("挂载失败: " + err).c_str());
                MessageBoxW(nullptr, msg.c_str(), L"Wddisk", MB_ICONERROR);
                delete fs;
                return 1;
            }
            g_mounted[idx] = fs;
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
            return 0;
        }
        MessageBoxW(nullptr, L"配置序号不存在", L"Wddisk", MB_ICONERROR);
        return 1;
    }

    // 字体 + 笔刷（DPI 感知，Segoe UI 英文 + 雅黑中文）
    {
        HMODULE u = LoadLibraryW(L"user32.dll");
        typedef int (WINAPI *FD)(HWND);
        FD fd = (FD)GetProcAddress(u, "GetDpiForWindow");
        HWND tmp = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);
        if (fd) g_dpi = fd(tmp); else g_dpi = GetDeviceCaps(GetDC(nullptr), LOGPIXELSX);
        DestroyWindow(tmp);
        FreeLibrary(u);
    }
    auto mkFont = [&](int sz, int weight, const wchar_t* name) -> HFONT {
        int s = -MulDiv(sz, g_dpi, 96);
        return CreateFontW(s, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, name);
    };
    fTitle  = mkFont(20, FW_BOLD,      L"Segoe UI");
    fName   = mkFont(16, FW_SEMIBOLD,  L"Microsoft YaHei UI");
    fSub    = mkFont(12, FW_NORMAL,    L"Microsoft YaHei UI");
    fBtn    = mkFont(13, FW_NORMAL,    L"Segoe UI");
    fStatus = mkFont(11, FW_NORMAL,    L"Microsoft YaHei UI");
    fBadge  = mkFont(16, FW_BOLD,      L"Segoe UI");
    fIcon   = mkFont(28, FW_BOLD,      L"Segoe UI");
    fTiny   = mkFont(9,  FW_BOLD,      L"Segoe UI");

    brBg     = solidBrush(CLR_BG);
    brCard   = solidBrush(CLR_CARD);
    brCardHi = solidBrush(CLR_CARD_HI);
    brAccent = solidBrush(CLR_ACCENT);
    brAccentD = solidBrush(CLR_ACCENT_D);
    brInput  = solidBrush(CLR_INPUT);
    brGreen  = solidBrush(CLR_GREEN);

    // 注册主窗口
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"WddiskMain";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    int W = 880, H = 600;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int X = work.left + ((work.right - work.left) - W) / 2;
    int Y = work.top + ((work.bottom - work.top) - H) / 2;

    // 无系统标题栏（自绘 caption）：才能同时做到深色标题栏 + 真圆角
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"WddiskMain", L"Wddisk · WebDAV 网络磁盘",
        WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, X, Y, W, H, nullptr, nullptr, hInst, nullptr);

    // 自绘标题栏后只需裁剪窗口圆角（DWM 深色属性对无 caption 窗口无影响）
    applyRoundRegion(hwnd);

    ShowWindow(hwnd, trayMode ? SW_HIDE : nCmdShow);
    if (trayMode) {
        // 启动后自动挂载所有勾选 autoMount 的配置，然后停留在托盘
        for (int i = 0; i < (int)g_cfg->servers().size(); ++i)
            if (g_cfg->servers()[i].autoMount && !isMounted(i)) doMount(i, hwnd);
    }
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
