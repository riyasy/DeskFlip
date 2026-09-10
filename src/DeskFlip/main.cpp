// Window, tray icon, context menu, About box, settings file, message loop, and the render gate.
// Everything Direct2D is in clockwindow.cpp.
//
// The window is a transparent, borderless overlay (a DComp swapchain supplies the alpha) that you
// interact with directly, the way a desktop widget should: drag the clock to move it, right-click
// it for the menu, and turn on Resize to get a corner grip. It never takes focus. The menu is the
// whole configuration UI, and the tray icon (when it is switched on) opens the same menu; the
// only other window in the program is the About box, and it is modeless -- see there.
//
// The loop's one rule: nothing renders unless it moves. Idle blocks on a waitable timer set to
// the next second boundary; a flip blocks on DXGI's frame-latency object inside Render().

#include "clockwindow.h"
#include "resource.h"

#include <appmodel.h>  // GetCurrentPackageFullName -- packaged or loose exe
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>  // GET_X_LPARAM

// The packaged half of "Start with Windows". Header-only and shipped with the Windows SDK, so it
// costs a #include and windowsapp.lib rather than a dependency; see SetStartupEnabled.
#include <winrt/Windows.Foundation.h>       // IAsyncOperation::get -- the blocking wait
#include <winrt/Windows.ApplicationModel.h>  // StartupTask

#include <algorithm>  // std::min / std::max -- the project builds with NOMINMAX
#include <cassert>
#include <cmath>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")  // CoInitializeEx; the .vcxproj sets no AdditionalDependencies
#pragma comment(lib, "advapi32.lib")  // the Run key and the theme's light/dark value
#pragma comment(lib, "dwmapi.lib")    // the About box's dark title bar
#pragma comment(lib, "windowsapp.lib")  // StartupTask, for the packaged build

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define WM_TRAYICON (WM_APP + 1)
#define WM_ENDRESIZE (WM_APP + 2)  // posted by the outside-click hook; see ApplyResizeMode
constexpr UINT TRAY_UID = 787;
constexpr int MENU_SECONDS = 1;
constexpr int MENU_TOPMOST = 2;
constexpr int MENU_RESIZE = 3;
constexpr int MENU_EXIT = 4;
constexpr int MENU_LIGHT = 5;
constexpr int MENU_STARTUP = 6;
constexpr int MENU_TRAY = 7;
constexpr int MENU_ABOUT = 8;

static Clock g_clock;
static HINSTANCE g_inst = nullptr;
static HICON g_trayIcon = nullptr;  // owned by us: LoadIconMetric hands over a fresh HICON
static bool g_needsPaint = true;   // the OS asked us to paint, or the layout changed
static bool g_resizeMode = false;  // Resize ticked in the menu: the corner grip is live
static UINT g_dpi = 96;            // the monitor the clock is on; WM_DPICHANGED's "from" value

struct Settings {
    bool showSeconds = true;
    bool topmost = true;      // above other windows; off lets them cover the clock
    bool tray = true;         // the notification-area icon, which is the other way to the menu
    bool useWarp = false;     // UseWarp=1 for the software device: ~8 MB less RAM, ~3x the CPU
    bool lightMode = false;   // dark ink on light cards; same geometry, only the palette swaps
    int scalePct = 0;         // 0 = automatic (fit, capped at 1:1)
    int posX = -1, posY = -1; // < 0 = automatic (centred)
};
static Settings g_settings;

// %LOCALAPPDATA%\DeskFlip.ini, the same place DeskTick keeps its own, and the same
// WritePrivateProfileString mechanism let-it-rain uses. A handful of ints do not justify a format
// with a parser.
//
// Local rather than Roaming, and that is the packaged build's doing: the MSIX container redirects
// %LOCALAPPDATA% into the package's own LocalCache, so the file is the package's and goes with it
// on uninstall. In Roaming it is the real folder either way, so an uninstalled Store app would
// leave its settings behind for good -- and would sync them to a machine whose copy of the app
// might not be installed at all. Loose, this is the real folder and nothing changes.
static const wchar_t* IniPath() {
    static wchar_t path[MAX_PATH] = L"";
    if (!path[0]) {
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return nullptr;
        wcscat_s(path, L"\\DeskFlip.ini");
        // The file used to live in Roaming: move an existing one over rather than starting the
        // user from defaults. MoveFileW refuses when the destination exists, which is exactly the
        // "only if we have nothing yet" test, and fails harmlessly when there is nothing to move.
        // Drop this once nobody runs a build older than it.
        wchar_t legacy[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, legacy))) {
            wcscat_s(legacy, L"\\DeskFlip.ini");
            MoveFileW(legacy, path);
        }
    }
    return path;
}

static void LoadSettings() {
    const wchar_t* ini = IniPath();
    if (!ini) return;
    g_settings.showSeconds = GetPrivateProfileIntW(L"Settings", L"ShowSeconds", 1, ini) != 0;
    // Topmost replaced HideBehind, which said the same thing backwards (the menu item reads
    // "Always on top" now, the way every other widget words it). An ini written by an older build
    // has only the old key, so it becomes this one's default and the setting survives the
    // rename; SaveSettings then deletes it.
    const int legacy = GetPrivateProfileIntW(L"Settings", L"HideBehind", 0, ini) ? 0 : 1;
    g_settings.topmost = GetPrivateProfileIntW(L"Settings", L"Topmost", legacy, ini) != 0;
    g_settings.tray = GetPrivateProfileIntW(L"Settings", L"Tray", 1, ini) != 0;
    g_settings.lightMode = GetPrivateProfileIntW(L"Settings", L"LightMode", 0, ini) != 0;
    // Read once at startup and never re-read: swapping the renderer means tearing down the D3D
    // device and every baked bitmap with it, so it takes a restart. That is also why it is an ini
    // key and not a checkbox next to the other two -- a dialog toggle that quietly does nothing
    // until you relaunch is worse than a line in a file you had to open on purpose.
    //
    // Defaults to 0 (hardware). WARP costs ~8 MB less RAM and ~3x the CPU while a flip is in
    // flight; that CPU is free at the default size but real at full size with seconds on, and
    // waking the CPU for graphics work is the worse half of the trade on a laptop. Set UseWarp=1
    // if the memory matters more -- an existing ini already has the key written, so this default
    // only reaches fresh installs.
    g_settings.useWarp = GetPrivateProfileIntW(L"Settings", L"UseWarp", 0, ini) != 0;
    g_settings.scalePct = GetPrivateProfileIntW(L"Settings", L"ScalePct", 0, ini);
    g_settings.posX = GetPrivateProfileIntW(L"Settings", L"PosX", -1, ini);
    g_settings.posY = GetPrivateProfileIntW(L"Settings", L"PosY", -1, ini);
}

static void SaveSettings() {
    const wchar_t* ini = IniPath();
    if (!ini) return;
    auto put = [&](const wchar_t* key, int v) {
        wchar_t buf[16];
        _itow_s(v, buf, 10);
        WritePrivateProfileStringW(L"Settings", key, buf, ini);
        };
    put(L"ShowSeconds", g_settings.showSeconds);
    put(L"Topmost", g_settings.topmost);
    put(L"Tray", g_settings.tray);
    // nullptr deletes the key: HideBehind was migrated into Topmost above, and leaving a stale
    // one in the file invites someone to edit the half that no longer does anything.
    WritePrivateProfileStringW(L"Settings", L"HideBehind", nullptr, ini);
    put(L"LightMode", g_settings.lightMode);
    put(L"UseWarp", g_settings.useWarp);  // written back so the key is discoverable in the file
    put(L"ScalePct", g_settings.scalePct);
    put(L"PosX", g_settings.posX);
    put(L"PosY", g_settings.posY);
}

// ---------------------------------------------------------------------------
// The UI language
// ---------------------------------------------------------------------------
// The English string is the key. No numeric ids, no resource.h, nothing to keep in sync, and a
// missing key answers itself -- so a half-translated file is a working file and English is the
// fallback for free. The cost is that the English literal at each call site is now an identifier:
// editing one silently drops its translations.
//
// Storage is lang\<locale>.ini, read with GetPrivateProfileSectionW -- one call returns the whole
// [Strings] section as a double-null-terminated block of key=value, which is a parser we then do
// not write. Same API the settings file already uses, so this adds no dependency.
//
// **The files must be UTF-16LE with a BOM.** The profile APIs decide the encoding from the BOM
// alone; without one they read the file in the system codepage and every CJK and Cyrillic string
// arrives as mojibake, silently, with the file looking perfectly correct in an editor. Nothing
// here ever writes to them, so the API cannot rewrite one as ANSI behind us.
//
// Only the chrome goes through this -- the menu and the About box. The one string that does not
// is the fatal "Direct3D initialisation failed" box, which is a developer-facing dead end rather
// than UI, and the app names in OTHER_APPS, which are products rather than copy.
static wchar_t g_strings[8192];  // whole file, keys included; the UI is 15 short strings

// lang\, beside the exe. The .vcxproj copies the folder next to the binary after the link, the
// same way it does assets\, so there is no walk up out of a build tree to do.
static const wchar_t* LangDir() {
    static wchar_t dir[MAX_PATH] = L"";
    if (!dir[0]) {
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;
        wcscat_s(dir, L"\\lang");
    }
    return dir;
}

// Try one lang\<name>.ini. False if it isn't there or holds no [Strings].
static bool LoadLang(const wchar_t* name) {
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, L"%s\\%s.ini", LangDir(), name) < 0) return false;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    // Returns characters copied, not counting the final null. Zero means no such section, or an
    // empty one -- either way there is nothing to use.
    if (GetPrivateProfileSectionW(L"Strings", g_strings, _countof(g_strings), path) == 0) {
        g_strings[0] = 0;
        return false;
    }
    return true;
}

// Last resort for one language: any lang\<prefix>-*.ini at all. This is what serves the regional
// variants nobody ships a file for -- es-MX, es-AR and es-CO all land on es-ES.ini, de-AT and
// de-CH on de-DE.ini. Without it every one of those users would get English while a translation
// of their own language sat unread in the folder.
//
// It resolves by directory order, which picks the *language* right and can pick the *flavour*
// wrong: zh-HK takes zh-CN.ini because zh-CN sorts first. The fix is one file and no code --
// the full-name tier is tried before this is reached, so dropping in a zh-HK.ini overrides it.
static bool LoadLangByPrefix(const wchar_t* prefix) {
    wchar_t pat[MAX_PATH];
    if (swprintf_s(pat, L"%s\\%s-*.ini", LangDir(), prefix) < 0) return false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (wchar_t* dot = wcsrchr(fd.cFileName, L'.')) *dot = 0;  // LoadLang appends .ini itself
        ok = LoadLang(fd.cFileName);
    } while (!ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static void LocInit() {
    // The user's *display language* chain, most preferred first -- NOT GetUserDefaultLocaleName,
    // which is the Region setting and answers a different question. The two genuinely differ in
    // the field, and reading the region to pick the UI language gets it wrong in both directions.
    //
    // The chain matters as much as the name: Windows answers e.g. "de-AT" -> "de" -> "de-DE", and
    // following it is how a language pack we have no exact file for still resolves to one we do.
    wchar_t langs[512] = { 0 };
    ULONG count = 0, cch = _countof(langs);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, langs, &cch)) return;

    // Every tier for one language before moving to the next, which is the whole point of a
    // preference order: a de-AT primary must reach de-DE.ini before an en-US secondary is even
    // considered. Full name first, and that is load-bearing rather than tidy -- pt-PT/pt-BR and
    // zh-CN/zh-TW are different files, and going straight to the "pt" or "zh" prefix would hand
    // half of those users the other half's translation.
    for (const wchar_t* p = langs; *p; p += wcslen(p) + 1) {
        wchar_t name[LOCALE_NAME_MAX_LENGTH];
        if (wcscpy_s(name, p) != 0) continue;
        if (LoadLang(name)) return;                     // de-DE.ini
        wchar_t* dash = wcschr(name, L'-');
        if (!dash) continue;
        *dash = 0;
        if (LoadLang(name)) return;                     // de.ini
        if (LoadLangByPrefix(name)) return;             // de-AT -> de-DE.ini
    }
}

static const wchar_t* T(const wchar_t* en) {
    if (!en || !g_strings[0]) return en;
    // Linear scan of the double-null-terminated block. Fifteen entries, walked once when a menu
    // or the About box is built and never per frame -- an index would cost more code than it
    // saves.
    for (const wchar_t* p = g_strings; *p; p += wcslen(p) + 1) {
        const wchar_t* eq = wcschr(p, L'=');
        if (!eq || eq == p) continue;  // no key, or no value: skip
        if (CompareStringOrdinal(p, (int)(eq - p), en, -1, TRUE) != CSTR_EQUAL) continue;
        // An empty value is a string a translator has not filled in yet. That is a normal state
        // of a shipped file, and it means English.
        return eq[1] ? eq + 1 : en;
    }
    return en;
}

// Read the clock's current layout back out and remember it. Called when a drag ends. The
// window IS the case now (padded for the shadow), so the case's screen position is just the
// window's, inset by that padding.
static void CaptureLayout(HWND hwnd) {
    RECT wr;
    GetWindowRect(hwnd, &wr);
    const float pad = Clock::PadFor(g_clock.Scale());
    g_settings.scalePct = (int)(g_clock.Scale() * 100.f + 0.5f);
    g_settings.posX = (int)(wr.left + pad);
    g_settings.posY = (int)(wr.top + pad);
}

// The desktop's icon view -- SHELLDLL_DefView, a child of Progman or a WorkerW. Owning our
// window to it (GWL_HWNDPARENT) puts us in the desktop's window group instead of the ordinary
// app group, and Show Desktop only hides the latter. Same technique as DateLine's
// WindowHelper.SetAsDesktopChild.
static HWND FindDesktopView() {
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        HWND view = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (view) { *(HWND*)lp = view; return FALSE; }
        return TRUE;
        }, (LPARAM)&result);
    return result;
}

// Topmost unless the user asked for the clock to be coverable. Owned by the desktop's icon
// view unconditionally: that's what lets a window this small survive Show Desktop at all
// (DateLine is the proof -- small window, always owned, never hidden by Win+D), not just the
// not-topmost case. The setting only changes the topmost bit.
static void ApplyTopmost(HWND hwnd) {
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)FindDesktopView());
    SetWindowPos(hwnd, g_settings.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Pull a window back onto a monitor. Both windows need it and for the same reason -- the clock
// because a monitor it was parked on can be unplugged or the taskbar can move under it, the About
// box because it opens beside a clock that is usually at a screen edge.
//
// Clamped against the *work* area, not the monitor: the clock is owned by the desktop and draws
// behind the taskbar, so a spot under it is as good as off-screen. MONITOR_DEFAULTTONEAREST
// because once the monitor the clock was on is gone there is no correct answer left, only a
// reachable one.
static void ClampToMonitor(HWND h) {
    RECT r;
    if (!h || !GetWindowRect(h, &r)) return;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) return;
    int x = r.left, y = r.top;
    const int w = r.right - r.left, ht = r.bottom - r.top;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    if (y + ht > mi.rcWork.bottom) y = mi.rcWork.bottom - ht;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    if (x != r.left || y != r.top)
        SetWindowPos(h, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = hwnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }
}

// Add or remove the notification icon to match the setting, and the only thing that touches it:
// the menu toggle, startup, and the shell restarting all go through here.
//
// On by default, because the clock is the kind of window you can lose behind a maximised one and
// the menu is the only way to Exit. Turning it off is for the user who wants the desktop and
// nothing else, and who knows the clock itself is still right-clickable.
static void TraySync(HWND hwnd) {
    if (!g_settings.tray) { RemoveTrayIcon(hwnd); return; }
    if (g_trayIcon) return;  // already up; NIM_ADD twice would fail and NIM_MODIFY buys nothing
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAYICON;
    // LoadIconMetric picks the small-icon frame for this DPI out of the .ico rather than
    // smearing a 256px one down. It is a comctl32 v6 export, so it only links to something at
    // runtime because DeskFlip.manifest declares the v6 dependency -- without that manifest
    // the process dies at load with "the ordinal 380 could not be located".
    //
    // The HICON is ours to destroy -- the shell copies it into its own tray bitmap rather than
    // taking ownership -- so it is kept in g_trayIcon and released in RemoveTrayIcon. It also
    // doubles as "the icon is currently up", which is what the early-out above reads.
    LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_DESKFLIP), LIM_SMALL, &g_trayIcon);
    nid.hIcon = g_trayIcon;
    wcscpy_s(nid.szTip, L"DeskFlip");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION;  // v4 breaks the context menu; let-it-rain hit this too
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

// ---------------------------------------------------------------------------
// Start with Windows
// ---------------------------------------------------------------------------
// The registry value IS the state -- there is nothing cached to keep in sync, and the menu asks
// afresh every time it opens, because Task Manager's Startup tab can turn the entry off behind
// our back. Per-user and non-elevated by design: no scheduled task, no service, no UAC prompt to
// tick a menu item.
//
// A packaged build cannot use that key at all: the value would have to name the exe under
// WindowsApps, which is not launchable that way and would start the app without its package
// identity even if it were. A package declares a windows.startupTask extension in its manifest
// and asks Windows to enable it instead -- same menu item, same "nothing is cached" rule,
// different API. Hence the fork, and hence Packaged().

// GetCurrentPackageFullName answers APPMODEL_ERROR_NO_PACKAGE with no package and asks for a
// bigger buffer when there is one, so a length probe with no buffer is the whole check.
static bool Packaged() {
    UINT32 n = 0;
    return GetCurrentPackageFullName(&n, nullptr) == ERROR_INSUFFICIENT_BUFFER;
}

static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t RUN_VAL[] = L"DeskFlip";
// Must match TaskId in Package.appxmanifest's windows.startupTask extension. A mismatch is not a
// build error -- GetAsync just throws at runtime and the menu item goes quietly dead.
static const wchar_t TASK_ID[] = L"DeskFlipStartup";

static bool StartupEnabled() {
    if (Packaged()) {
        // Both packaged calls swallow everything: this API throws, and a menu check mark is not
        // worth taking the app down for.
        try {
            using namespace winrt::Windows::ApplicationModel;
            return StartupTask::GetAsync(TASK_ID).get().State() == StartupTaskState::Enabled;
        }
        catch (...) { return false; }
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    const bool on = RegQueryValueExW(k, RUN_VAL, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void SetStartupEnabled(bool on) {
    if (Packaged()) {
        try {
            using namespace winrt::Windows::ApplicationModel;
            auto t = StartupTask::GetAsync(TASK_ID).get();
            // RequestEnableAsync is a silent no-op once the user has switched the app off in
            // Task Manager -- the state sticks at DisabledByUser and only Task Manager can lift
            // it. Nothing here caches, so the next menu simply comes up unticked, which is true.
            if (on) t.RequestEnableAsync().get(); else t.Disable();
        }
        catch (...) {}
        return;
    }
    HKEY k;
    // The key exists on every Windows install, but RegCreateKeyEx opens an existing key rather
    // than failing, so it covers the one that doesn't.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k,
        nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t exe[MAX_PATH], val[MAX_PATH + 4];
        const DWORD n = GetModuleFileNameW(nullptr, exe, _countof(exe));
        // Quoted: an unquoted path with a space in it is read by CreateProcess as a program name
        // plus arguments, and "C:\Program" does not exist.
        if (n && n < _countof(exe)) {
            swprintf_s(val, L"\"%s\"", exe);
            RegSetValueExW(k, RUN_VAL, 0, REG_SZ, (const BYTE*)val,
                (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
        }
    }
    else {
        RegDeleteValueW(k, RUN_VAL);
    }
    RegCloseKey(k);
}

// Show seconds is a geometry change: the case gains or loses a group, so the window -- which IS
// the case -- has to be resized to match, and that WM_SIZE is what rebakes. Called from the menu.
static void ApplyShowSeconds(HWND hwnd) {
    g_clock.SetShowSeconds(g_settings.showSeconds);
    const RECT wr = Clock::WindowRectFor(g_settings.showSeconds, g_settings.scalePct / 100.f,
        g_settings.posX, g_settings.posY);
    SetWindowPos(hwnd, nullptr, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    g_needsPaint = true;
}

// ---------------------------------------------------------------------------
// Dark mode
// ---------------------------------------------------------------------------
// There is no public API for menu theming -- uxtheme ordinals 135 (SetPreferredAppMode) and 136
// (FlushMenuThemes) are what Explorer itself uses. AllowDark means "follow the system setting",
// so nothing here reads the registry or picks a colour.
//
// The build check is load-bearing rather than politeness: before 17763 those ordinals are
// unrelated private functions with different signatures. Everything stays null on an older build
// and IsDarkMode() answers false, which is the right answer there -- no dark mode to follow.
//
// This covers the context menu. The About box themes its own controls below, because a common
// control needs a per-control opt-in; it reads IsDarkMode() so the two can never disagree.
static void (WINAPI* g_refreshColorPolicy)();  // ordinal 104
static bool (WINAPI* g_shouldAppsUseDark)();   // ordinal 132
static void (WINAPI* g_flushMenuThemes)();     // ordinal 136

static bool IsDarkMode() {
    return g_shouldAppsUseDark && g_shouldAppsUseDark();
}

// Order matters, and it is the whole reason 104 is here: uxtheme caches the light/dark policy, so
// flushing the menu theme on its own re-resolves to the value cached at startup and the menu
// never changes colour.
static void ReflushMenuTheme() {
    if (g_refreshColorPolicy) g_refreshColorPolicy();
    if (g_flushMenuThemes) g_flushMenuThemes();
}

static void InitDarkMode() {
    OSVERSIONINFOW vi{ sizeof(vi) };  // RTL_OSVERSIONINFOW is the same layout
    LONG(WINAPI * getVer)(OSVERSIONINFOW*) = (LONG(WINAPI*)(OSVERSIONINFOW*))
        GetProcAddress(GetModuleHandleW(L"ntdll"), "RtlGetVersion");
    if (!getVer || getVer(&vi) != 0 || vi.dwBuildNumber < 17763) return;
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");  // never freed: process lifetime
    if (!ux) return;
    int (WINAPI * setAppMode)(int) = (int (WINAPI*)(int))GetProcAddress(ux, MAKEINTRESOURCEA(135));
    g_refreshColorPolicy = (void (WINAPI*)())GetProcAddress(ux, MAKEINTRESOURCEA(104));
    g_shouldAppsUseDark = (bool (WINAPI*)())GetProcAddress(ux, MAKEINTRESOURCEA(132));
    g_flushMenuThemes = (void (WINAPI*)())GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setAppMode) setAppMode(1);  // 1 = AllowDark
    ReflushMenuTheme();
}

// ---------------------------------------------------------------------------
// The About box
// ---------------------------------------------------------------------------
// The only other window in the program, and modeless for a hard reason: a modal DialogBox runs
// its own message loop, and ours is the one that waits on the tick timer -- going modal would
// freeze the clock for as long as the box was open. So it is a plain window, wWinMain's loop
// pumps IsDialogMessageW for it (Tab, Escape, mnemonics -- a plain window gets none of that for
// free), and WM_DESTROY has to close it explicitly because it is deliberately ownerless.
//
// It follows the *system* light/dark setting rather than the clock's own Light mode: this is
// ordinary Win32 chrome sitting among the user's other windows, not part of the clock face.

struct OtherApp {
    int icon;
    const wchar_t* name;
    const wchar_t* blurb;
    const wchar_t* store;  // ms-windows-store: opens the Store app
    const wchar_t* web;    // https: used only if that scheme is dead
};

// Two URLs each, and the order matters. ms-windows-store://pdp goes straight to the Store app;
// an https://apps.microsoft.com link is a web page, so the browser wins it and the Store only
// opens if the page decides to hand off. The scheme is not registered on every Windows (LTSC,
// Server, a stripped image), so the web page is the fallback for a click that would otherwise do
// nothing -- never the first choice.
//
// cid is the campaign id Partner Center reports on, attributing installs within 24 hours of the
// click. Same value on both URLs so either route attributes the same, and named for the surface.
static const OtherApp OTHER_APPS[] = {
    { IDI_FLYPHOTOS, L"FlyPhotos",
      L"Fast, lightweight, and minimalist photo viewer designed for the modern Windows",
      L"ms-windows-store://pdp/?productid=9PMSK128V1QT&cid=DeskFlipAbout",
      L"https://apps.microsoft.com/detail/9pmsk128v1qt?cid=DeskFlipAbout&mode=full" },
    { IDI_LETITRAIN, L"Let It Rain",
      L"Desktop Rain and Snow Simulator for Windows",
      L"ms-windows-store://pdp/?productid=9P1H1VCJHJZP&cid=DeskFlipAbout",
      L"https://apps.microsoft.com/detail/9p1h1vcjhjzp?cid=DeskFlipAbout&mode=full" },
    { IDI_DESKTICK, L"DeskTick",
      L"Customizable, minimal, transparent desktop clock widgets for Windows",
      L"ms-windows-store://pdp/?productid=9NQGFVNBX4WJ&cid=DeskFlipAbout",
      L"https://apps.microsoft.com/detail/9nqgfvnbx4wj?cid=DeskFlipAbout&mode=full" },
};

constexpr int A_MARGIN = 16, A_WIDTH = 372, A_ICON = 40, A_GAP = 14;
constexpr int ID_LINK = 901;  // the mailto link
constexpr int ID_APP0 = 910;  // + app index; an app's icon and its name share one
constexpr COLORREF DARK_BG = RGB(32, 32, 32), DARK_FG = RGB(255, 255, 255);
constexpr DWORD ABOUT_STYLE = WS_POPUPWINDOW | WS_CAPTION;

static HWND g_about = nullptr;
static HFONT g_aFont = nullptr, g_aTitle = nullptr, g_aHead = nullptr;  // body, "DeskFlip", heads
static HICON g_aIcons[_countof(OTHER_APPS)] = {};

// Windows themes a control's glyphs but never the surface behind them, so both WM_ERASEBKGND and
// WM_CTLCOLORSTATIC need this. The dark brush is created once and never deleted -- one GDI object
// for the life of the process is cheaper than tracking its lifetime.
static HBRUSH ThemeBrush() {
    static HBRUSH dark = nullptr;
    if (!IsDarkMode()) return GetSysColorBrush(COLOR_WINDOW);
    if (!dark) dark = CreateSolidBrush(DARK_BG);
    return dark;
}

// The title bar is DWM's, and no theme class reaches it.
static void ThemeCaption(HWND h) {
    const BOOL dark = IsDarkMode();
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// What `s` actually measures in font `f`: pass w to wrap into it and read r.bottom, or 0 for one
// line and read r.right. DT_CALCRECT is the only way to know -- a STATIC will not tell you, and
// guessing leaves either a blank line under a short blurb or a clickable strip of nothing beside
// a short name.
static RECT TextExtent(HWND p, HFONT f, const wchar_t* s, int w) {
    HDC dc = GetDC(p);
    HGDIOBJ old = SelectObject(dc, f);
    RECT r{ 0, 0, w, 0 };
    DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_LEFT | (w ? DT_WORDBREAK : DT_SINGLELINE));
    SelectObject(dc, old);
    ReleaseDC(p, dc);
    return r;
}

// One text row. Returns the y below it, so the layout reads as a column of rows and no
// coordinate is written twice.
static int AboutText(const wchar_t* s, HFONT f, int x, int y, int w, int h, int id = 0,
    DWORD extra = 0) {
    HWND t = CreateWindowExW(0, L"STATIC", s, WS_CHILD | WS_VISIBLE | SS_LEFT | extra,
        x, y, w, h, g_about, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(t, WM_SETFONT, (WPARAM)f, TRUE);
    return y + h;
}

// Lay the box out top to bottom at the current DPI. Every child, font and icon is rebuilt here,
// so this is also the DPI-change path.
static void AboutBuild() {
    HWND c;
    while ((c = GetWindow(g_about, GW_CHILD)) != nullptr) DestroyWindow(c);
    for (HFONT* f : { &g_aFont, &g_aTitle, &g_aHead }) if (*f) { DeleteObject(*f); *f = nullptr; }
    for (HICON& i : g_aIcons) if (i) { DestroyIcon(i); i = nullptr; }

    const UINT dpi = GetDpiForWindow(g_about);
#define A(x) MulDiv(x, (int)dpi, 96)

    // Derived from the system message font rather than naming a family, so the box follows
    // whatever Windows is set to -- including a user's larger text.
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    g_aFont = CreateFontIndirectW(&lf);
    LOGFONTW big = lf; big.lfHeight = lf.lfHeight * 9 / 5; big.lfWeight = FW_SEMIBOLD;
    g_aTitle = CreateFontIndirectW(&big);
    LOGFONTW mid = lf; mid.lfWeight = FW_SEMIBOLD;
    g_aHead = CreateFontIndirectW(&mid);

    const int x = A(A_MARGIN), w = A(A_WIDTH) - 2 * A(A_MARGIN);
    int y = A(A_MARGIN);

    // From resource.h, so this box and the exe's own properties cannot disagree. _CRT_WIDE makes
    // a wide literal of the narrow macro the .rc needs.
    // The product name and the version are identity, not copy: untranslated, like the app names
    // in OTHER_APPS below.
    y = AboutText(_CRT_WIDE(VER_PRODUCT), g_aTitle, x, y, w, A(30));
    y = AboutText(_CRT_WIDE(VER_DISPLAY), g_aFont, x, y + A(2), w, A(18));
    // \u00A9 rather than a pasted copyright sign: this is a wide literal in a BOM-less .cpp, so
    // a universal character name is the only escape the compiler resolves whatever encoding it
    // reads the file in. Keep this file ASCII-only for that reason.
    // Assembled from three pieces rather than translated whole, because only the last piece is
    // prose. Hand a translator the finished sentence and every one of them has to retype the
    // holder inside their value, where a typo is a wrong copyright notice -- and rebranding would
    // drop all seventeen translations at once, the key having changed. So the holder comes from
    // VER_COMPANY untranslated and only the sentence goes through T().
    wchar_t copyright[160];
    swprintf_s(copyright, L"\u00A9 %s. %s", _CRT_WIDE(VER_COMPANY), T(L"All rights reserved."));
    y = AboutText(copyright, g_aFont, x, y, w, A(18));

    // Height measured rather than fixed at one line, for the same reason the blurbs below are:
    // this sentence is short in English and not in every language.
    const wchar_t* feedback = T(L"Report issues or send feedback to");
    y = AboutText(feedback, g_aFont, x, y + A(14), w,
        std::max(A(18), (int)TextExtent(g_about, g_aFont, feedback, w).bottom));
    // A SysLink rather than blue static text: it gets the hand cursor, keyboard focus and the
    // theme's own link colour for free, in both light and dark.
    HWND link = CreateWindowExW(0, WC_LINK,
        L"<a href=\"mailto:ryftools@outlook.com\">ryftools@outlook.com</a>",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, w, A(20), g_about,
        (HMENU)(INT_PTR)ID_LINK, g_inst, nullptr);
    SendMessageW(link, WM_SETFONT, (WPARAM)g_aFont, TRUE);
    y += A(20);

    y = AboutText(T(L"Other apps"), g_aHead, x, y + A(18), w, A(20)) + A(6);

    for (int i = 0; i < (int)_countof(OTHER_APPS); i++) {
        g_aIcons[i] = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(OTHER_APPS[i].icon),
            IMAGE_ICON, A(A_ICON), A(A_ICON), 0);
        // SS_NOTIFY on the icon and the name, sharing one id: both open the same Store page, and
        // nothing here looks either control up by id.
        HWND ic = CreateWindowExW(0, L"STATIC", nullptr,
            WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZECONTROL | SS_NOTIFY,
            x, y, A(A_ICON), A(A_ICON), g_about, (HMENU)(INT_PTR)(ID_APP0 + i), g_inst, nullptr);
        SendMessageW(ic, STM_SETICON, (WPARAM)g_aIcons[i], 0);

        const int tx = x + A(A_ICON + A_GAP), tw = w - A(A_ICON + A_GAP);
        // The name is only as wide as the name: it is clickable, and a static stretched to the
        // margin would put the hand cursor over empty space.
        const int nw = (int)TextExtent(g_about, g_aHead, OTHER_APPS[i].name, 0).right;
        int ty = AboutText(OTHER_APPS[i].name, g_aHead, tx, y, std::min(nw, tw), A(18),
            ID_APP0 + i, SS_NOTIFY);
        // Height from DT_CALCRECT: one blurb wraps to two lines and another doesn't, and a fixed
        // two-line box would leave the short one trailing an empty line. Which is also why the
        // translated blurb needs nothing done to it -- the row is sized to whatever it measures.
        const wchar_t* blurb = T(OTHER_APPS[i].blurb);
        ty = AboutText(blurb, g_aFont, tx, ty, tw,
            (int)TextExtent(g_about, g_aFont, blurb, tw).bottom);
        // Keep the row at least as tall as its icon, so the next one clears it.
        y = std::max(ty, y + A(A_ICON)) + A(12);
    }

    RECT rc{ 0, 0, A(A_WIDTH), y + A(A_MARGIN) - A(12) };
    AdjustWindowRectExForDpi(&rc, ABOUT_STYLE, FALSE, 0, dpi);
    SetWindowPos(g_about, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    // Pull it back onto the monitor. The box opens beside the clock and its height is only known
    // here, at the bottom of the layout -- and a clock parked at a screen edge (which is where
    // people park it) otherwise puts half the box off the screen. Measured before this existed:
    // a clock ending at x=1597 on a 1920-wide screen opened a 388-wide box at 1609.
    ClampToMonitor(g_about);

    InvalidateRect(g_about, nullptr, TRUE);
#undef A
}

static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) { DestroyWindow(hwnd); return 0; }
        const int app = LOWORD(wp) - ID_APP0;  // STN_CLICKED on an icon or a name
        if (app >= 0 && app < (int)_countof(OTHER_APPS)) {
            // ShellExecute returns <= 32 when nothing claims the scheme, which is the only way
            // to find out -- so try the Store, then the web page.
            if ((INT_PTR)ShellExecuteW(hwnd, L"open", OTHER_APPS[app].store, nullptr, nullptr,
                SW_SHOWNORMAL) <= 32)
                ShellExecuteW(hwnd, L"open", OTHER_APPS[app].web, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }
    case WM_SETCURSOR: {
        // A child's WM_SETCURSOR reaches us through its DefWindowProc, so the hand for every app
        // row is one handler rather than a subclass each.
        const int id = GetDlgCtrlID((HWND)wp);
        if (id >= ID_APP0 && id < ID_APP0 + (int)_countof(OTHER_APPS)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)lp;
        if (n->idFrom == ID_LINK && (n->code == NM_CLICK || n->code == NM_RETURN)) {
            // The href is in the control, so nothing here has to know the address.
            ShellExecuteW(hwnd, L"open", ((NMLINK*)lp)->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        break;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, ThemeBrush());
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, IsDarkMode() ? DARK_FG : GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)ThemeBrush();
    case WM_DPICHANGED: {
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        AboutBuild();  // AboutBuild sizes it; only the position comes from the message
        return 0;
    }
    case WM_DESTROY:
        for (HFONT* f : { &g_aFont, &g_aTitle, &g_aHead }) if (*f) { DeleteObject(*f); *f = nullptr; }
        for (HICON& i : g_aIcons) if (i) { DestroyIcon(i); i = nullptr; }
        g_about = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Open the box, or raise it if it is already up. `owner` only places the window beside the clock.
static void AboutShow(HWND owner) {
    if (g_about) { SetForegroundWindow(g_about); return; }

    static bool registered = false;
    if (!registered) {
        // The only comctl32 class asked for by name -- WC_LINK, for the mailto address.
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_LINK_CLASS };
        InitCommonControlsEx(&icc);
        WNDCLASSW wc{};
        wc.lpfnWndProc = AboutProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // WM_ERASEBKGND: theme-dependent
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_DESKFLIP));
        wc.lpszClassName = L"DeskFlipAbout";
        RegisterClassW(&wc);
        registered = true;
    }

    RECT rc;
    GetWindowRect(owner, &rc);
    g_about = CreateWindowExW(0, L"DeskFlipAbout", T(L"About DeskFlip"), ABOUT_STYLE,
        rc.right + 12, rc.top, 100, 100, nullptr, nullptr, g_inst, nullptr);
    if (!g_about) return;
    ThemeCaption(g_about);
    AboutBuild();
    ShowWindow(g_about, SW_SHOW);
    SetForegroundWindow(g_about);
}

// Light/dark switched while we were running. Driven from the clock's WM_SETTINGCHANGE rather than
// the box's own, because the order between two top-level windows getting that broadcast is
// undefined and uxtheme's colour cache has to be refreshed first -- see ReflushMenuTheme.
static void AboutThemeChanged() {
    if (!g_about) return;
    ThemeCaption(g_about);
    AboutBuild();  // every colour is chosen in there, so a rebuild is the whole repaint
}

// The entire configuration UI, built fresh each time so the check marks are always current.
// Reached two ways -- right-click the clock, or click the tray icon -- because a widget you can
// lose behind a maximised window still needs one handle that is always reachable.
//
// SetForegroundWindow first, or the menu will not dismiss when you click away from it: this
// window is WS_EX_NOACTIVATE and never becomes foreground on its own. TPM_RETURNCMD keeps the
// handling here instead of scattering it into WM_COMMAND.
static void EndGripDrag(HWND h, bool keep);        // defined with the rest of the drag code below
static void ApplyResizeMode(HWND h, bool on);      // defined with the outside-click hook below

// True only for as long as our own menu is up. The outside-click hook has to ignore the click
// that picks a menu item -- it lands outside the clock, so it would otherwise cancel Resize mode
// as a side effect of using the menu at all.
static bool g_menuUp = false;

static void ShowContextMenu(HWND hwnd) {
    // Opening the menu abandons any drag still in flight and gives up the capture with it.
    // TrackPopupMenu wants the capture for itself, and this is also the escape hatch: whatever
    // state the input got into, a right-click ends it and puts Exit back within reach.
    EndGripDrag(hwnd, false);

    // Grouped: what the clock shows, then where it lives, then the way out.
    //
    // Every label goes through T(). The English literal IS the key the lang\*.ini files are
    // written against, so editing one here silently drops its translations.
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_settings.showSeconds ? MF_CHECKED : 0), MENU_SECONDS, T(L"Show seconds"));
    AppendMenuW(menu, MF_STRING | (g_settings.lightMode ? MF_CHECKED : 0), MENU_LIGHT, T(L"Light mode"));
    AppendMenuW(menu, MF_STRING | (g_resizeMode ? MF_CHECKED : 0), MENU_RESIZE, T(L"Resize"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // The first and third are ours to remember (DeskFlip.ini); the second is a registry value
    // Windows itself reads, so it is asked for fresh every time this menu opens -- the user may
    // well have unticked it in Task Manager's Startup tab since.
    AppendMenuW(menu, MF_STRING | (g_settings.topmost ? MF_CHECKED : 0), MENU_TOPMOST, T(L"Always on top"));
    AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), MENU_STARTUP, T(L"Start with Windows"));
    AppendMenuW(menu, MF_STRING | (g_settings.tray ? MF_CHECKED : 0), MENU_TRAY, T(L"Show in system tray"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // The ellipsis is a universal character name because this file is ASCII-only: MSVC reads a
    // BOM-less .cpp in the system codepage, so a pasted U+2026 would reach the menu as mojibake.
    // It is part of the T() key too, and lang\*.ini spells it as a real U+2026 -- those files are
    // UTF-16 and can.
    AppendMenuW(menu, MF_STRING, MENU_ABOUT, T(L"About DeskFlip\u2026"));
    AppendMenuW(menu, MF_STRING, MENU_EXIT, T(L"Exit"));

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    g_menuUp = true;
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    g_menuUp = false;
    DestroyMenu(menu);

    switch (cmd) {
    case MENU_SECONDS:
        g_settings.showSeconds = !g_settings.showSeconds;
        ApplyShowSeconds(hwnd);
        SaveSettings();
        break;
    case MENU_LIGHT:
        // No geometry changes, so no window resize and no WM_SIZE: SetLightMode does its own
        // rebake. Unlike UseWarp this needs no restart -- the device stays, only the bitmaps
        // are rebuilt.
        g_settings.lightMode = !g_settings.lightMode;
        g_clock.SetLightMode(g_settings.lightMode);
        g_needsPaint = true;
        SaveSettings();
        break;
    case MENU_TOPMOST:
        g_settings.topmost = !g_settings.topmost;
        ApplyTopmost(hwnd);
        SaveSettings();
        break;
    case MENU_STARTUP:
        // No cached copy to flip: the registry value (or the package's startup task) is the
        // state, and it was read a few lines above to draw the check mark.
        SetStartupEnabled(!StartupEnabled());
        break;
    case MENU_TRAY:
        g_settings.tray = !g_settings.tray;
        TraySync(hwnd);
        SaveSettings();
        break;
    case MENU_RESIZE:
        ApplyResizeMode(hwnd, !g_resizeMode);
        break;
    case MENU_ABOUT:
        AboutShow(hwnd);
        break;
    case MENU_EXIT:
        DestroyWindow(hwnd);
        break;
    }
}

// Is the cursor (client px) on the grip? Only meaningful while Resize mode is on. The button is
// round, so the test is too -- GripRect() is its bounding square, and taking the square whole
// would put a live corner of hit region in four places that visibly are not the button.
static bool InGrip(POINT p) {
    const D2D1_RECT_F g = g_clock.GripRect();
    const float r = (g.right - g.left) * 0.5f;
    const float dx = p.x - (g.left + r), dy = p.y - (g.top + r);
    return dx * dx + dy * dy <= r * r;
}

// Where a screen point falls on the clock. WM_NCHITTEST and the outside-click hook both go
// through this, so the two can never drift apart about what counts as "on the clock" -- and the
// grip has to be tested first either way, since half the button hangs out over the shadow's
// padding, which is otherwise not ours.
enum class Where { Off, Case, Grip };

static Where HitClock(HWND h, POINT screen) {
    POINT p = screen;
    ScreenToClient(h, &p);
    if (g_resizeMode && InGrip(p)) return Where::Grip;
    const D2D1_RECT_F r = g_clock.CaseRect();
    if (p.x < r.left || p.x > r.right || p.y < r.top || p.y > r.bottom) return Where::Off;
    return Where::Case;
}

// Resize mode is meant to be transient: turn it on, size the clock, click away. But this window
// is WS_EX_NOACTIVATE, so it has no focus to lose -- there is no WM_KILLFOCUS or WM_ACTIVATEAPP
// to hang "the user is done" off, and a click that lands anywhere else never reaches us at all.
// A low-level mouse hook is the only thing that sees it.
//
// It is installed ONLY while resize mode is on, so the ordinary state pays nothing for this: no
// hook, no callback, no system-wide traffic. The callback stays trivial for the same reason it
// has to -- Windows silently drops a low-level hook whose proc overruns LowLevelHooksTimeout --
// and only posts, leaving the actual state change to the normal message path.
static HHOOK g_outsideHook = nullptr;
static HWND g_hookOwner = nullptr;

static LRESULT CALLBACK OutsideClickProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && !g_menuUp &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN)) {
        const POINT pt = ((MSLLHOOKSTRUCT*)lp)->pt;
        if (g_hookOwner && HitClock(g_hookOwner, pt) == Where::Off)
            PostMessageW(g_hookOwner, WM_ENDRESIZE, 0, 0);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void ApplyResizeMode(HWND h, bool on) {
    if (on == g_resizeMode) return;
    g_resizeMode = on;
    g_clock.SetResizeMode(on);
    g_needsPaint = true;
    if (on) {
        g_hookOwner = h;
        if (!g_outsideHook) g_outsideHook = SetWindowsHookExW(WH_MOUSE_LL, OutsideClickProc, g_inst, 0);
    }
    else if (g_outsideHook) {
        UnhookWindowsHookEx(g_outsideHook);
        g_outsideHook = nullptr;
    }
}

// Moving is the window manager's job (WM_NCHITTEST answers HTCAPTION), so the only drag left
// here is the grip's. The case's top-left corner stays pinned in screen px for the whole drag,
// and the window IS the case, so resizing means a real SetWindowPos, not a position update
// inside a bigger window.
static bool g_gripDrag = false;
static POINT g_anchor{};  // the case's top-left, screen px, fixed for the drag

// Where inside the grip you took hold, as an offset from the case's bottom-right corner (screen
// px, so normally negative in both axes). Without it the corner snaps to the cursor the instant
// you press, so the clock jumps by however far your click was from the corner and then trails the
// pointer by that much for the rest of the drag.
static POINT g_grab{};

// The scale whose case corner sits nearest the point the cursor is asking for. The case's aspect
// ratio is fixed, so the corner cannot go wherever the cursor is -- it can only slide along the
// diagonal (W, H) -- and this is the projection onto that diagonal, i.e. the closest it can get.
//
// The projection is what this needs and `max()` is not, which is not a style choice. On a *square*
// window `px = max(w, h)` weights both axes equally and feels exactly right. Generalised to an
// aspect ratio that is not 1:1 the same formula is max(w/W, ht/H) -- which is what this used to be
// -- and on a case that is 1060x280 it makes a pixel of vertical mouse movement worth 3.8 pixels of
// horizontal one. The hand's unavoidable vertical wobble then drives the width, and the clock
// lurches away from the pointer. Weighting each axis by its own length is what removes that: on a
// wide case the long axis leads, which is what the eye expects.
//
// It still cannot glue the grip to the cursor on an off-diagonal drag -- nothing can, while the
// aspect ratio is locked. A square window has the identical limit; it just hides it.
static float GripScale(float w, float ht, float W, float H) {
    return (w * W + ht * H) / (W * W + H * H);
}

#ifdef _DEBUG
static void DragSelfTest() {
    const float W = 1060.f, H = 280.f;  // a real case, 3 groups
    // On the diagonal the projection is exact: ask for 2x and get 2x.
    assert(fabsf(GripScale(2 * W, 2 * H, W, H) - 2.f) < 1e-4f);
    assert(fabsf(GripScale(W, H, W, H) - 1.f) < 1e-4f);
    // Off the diagonal it lands between the two axis ratios, never outside them -- that is the
    // whole difference from max(), which by construction returns the larger one.
    const float s = GripScale(2 * W, 0.5f * H, W, H);
    assert(s > 0.5f && s < 2.f);
    // Dragging outward always grows, dragging inward always shrinks.
    assert(GripScale(2 * W, 2 * H, W, H) > GripScale(W, H, W, H));
    assert(GripScale(0.5f * W, 0.5f * H, W, H) < GripScale(W, H, W, H));
}
#endif

// WM_MOUSEMOVE only records where the cursor is; FlushDrag() is what actually resizes the window,
// and it's called at most once per drained message batch (see the main loop), not once per
// WM_MOUSEMOVE. A resize is a real SetWindowPos plus a full rebake -- applying it per raw
// mouse-move event outpaces how fast that work can happen, so the queue backs up and the drag
// visibly lags behind the actual cursor. Collapsing to "the latest position, applied once we're
// about to render" is the same pacing philosophy Render() already uses for frames.
static bool g_dragDirty = false;
static POINT g_dragScreenPt{};

static void FlushDrag(HWND h) {
    if (!g_dragDirty) return;
    g_dragDirty = false;
    // Where the corner is being asked to go: the cursor, less wherever inside the grip it was
    // grabbed, measured from the case's pinned top-left.
    const float w = (float)(g_dragScreenPt.x - g_grab.x - g_anchor.x);
    const float ht = (float)(g_dragScreenPt.y - g_grab.y - g_anchor.y);

    const float s = g_clock.ClampScale(GripScale(w, ht, g_clock.CaseW(), g_clock.CaseH()));
    const float pad = Clock::PadFor(s);
    SetWindowPos(h, nullptr, (int)(g_anchor.x - pad), (int)(g_anchor.y - pad),
        (int)ceilf(g_clock.CaseW() * s + 2 * pad), (int)ceilf(g_clock.CaseH() * s + 2 * pad),
        SWP_NOZORDER | SWP_NOACTIVATE);
    g_needsPaint = true;
}

// Every way a grip drag can end: button up, capture stolen, the button turning out not to be
// down any more.
//
// Mouse capture is the one thing that can leave this window looking alive but deaf, so the
// invariant "no drag means no capture" is enforced here rather than assumed. While capture is
// held, hit-testing is bypassed entirely: WM_NCHITTEST never runs, so HTCAPTION never answers and
// the clock cannot be dragged, and a right-click arrives as WM_RBUTTONUP instead of
// WM_NCRBUTTONUP, so the menu never opens -- all while the render loop keeps flipping cards,
// because nothing about rendering depends on input. That is what "stuck, but still animating"
// is. Note the release is *not* guarded by g_gripDrag: getting here with the flag already
// cleared and the capture still held is exactly the state that wedges the window.
static void EndGripDrag(HWND h, bool keep) {
    const bool wasDragging = g_gripDrag;
    if (wasDragging && keep) FlushDrag(h);  // don't strand an unapplied resize
    g_dragDirty = false;
    g_gripDrag = false;
    // GetCapture() == h, not unconditionally: inside WM_CAPTURECHANGED the capture has already
    // gone elsewhere, and releasing then would take it off whoever just got it.
    if (GetCapture() == h) ReleaseCapture();
    if (wasDragging && keep) { CaptureLayout(h); SaveSettings(); }
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // A registered message has no compile-time id, so it cannot be a case label. Registered once
    // on first use; 0 means the OS refused, and no real message id is 0, so the test stays safe.
    // Explorer restarting takes both the notification icon and the SHELLDLL_DefView we are owned
    // to with it -- the tray icon would simply be gone, and the new desktop view would leave us
    // unowned and back to being hidden by Win+D.
    static const UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated && taskbarCreated) {
        if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = nullptr; }  // the old one went with it
        TraySync(h);
        ApplyTopmost(h);  // re-own to the new desktop view
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        TraySync(h);
        return 0;
    case WM_NCHITTEST: {
        // The whole interaction model, in three lines. The shadow's padding is not the clock, so
        // clicks there fall through to the desktop; the grip takes real clicks when Resize is on;
        // and the case itself answers HTCAPTION, which is what makes the window manager's own
        // move loop drag the clock without a line of dragging code on our side.
        switch (HitClock(h, POINT{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) })) {
        case Where::Grip: return HTCLIENT;
        case Where::Case: return HTCAPTION;
        default:          return HTTRANSPARENT;
        }
    }
    case WM_SETCURSOR: {
        // Only the grip hit-tests as HTCLIENT, and only in Resize mode; everything else falls
        // through to the class arrow.
        if (!g_resizeMode || LOWORD(lp) != HTCLIENT) break;
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(h, &p);
        if (!InGrip(p)) break;
        SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
        return TRUE;
    }
    case WM_ENTERSIZEMOVE:
        // The caption drag runs a modal move loop that swallows WM_SETCURSOR, so the move cursor
        // has to be set here; it sticks until WM_EXITSIZEMOVE puts the arrow back.
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return 0;
    case WM_EXITSIZEMOVE:
        // The move loop finished. It moved the real window, so the layout to remember is just
        // wherever the window now is.
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        CaptureLayout(h);
        SaveSettings();
        return 0;
    case WM_LBUTTONDOWN: {
        // Only reachable on the grip -- everywhere else hit-tests as HTCAPTION or HTTRANSPARENT.
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!g_resizeMode || !InGrip(p)) return 0;
        const D2D1_RECT_F r = g_clock.CaseRect();
        g_anchor = { (LONG)r.left, (LONG)r.top };
        POINT br{ (LONG)r.right, (LONG)r.bottom }, cur{ p.x, p.y };
        ClientToScreen(h, &g_anchor);
        ClientToScreen(h, &br);
        ClientToScreen(h, &cur);
        g_grab = { cur.x - br.x, cur.y - br.y };
        g_dragDirty = false;  // stale from a previous drag, if any -- this one starts clean
        g_gripDrag = true;
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_gripDrag) return 0;
        // The message carries the button state, so a WM_LBUTTONUP that never arrived -- capture
        // stolen at the wrong moment, an input desync, a release delivered elsewhere -- cannot
        // latch the drag on forever: the next move without the button down ends it.
        if (!(wp & MK_LBUTTON)) { EndGripDrag(h, true); return 0; }
        POINT screen{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(h, &screen);
        g_dragScreenPt = screen;
        g_dragDirty = true;
        return 0;
    }
    case WM_LBUTTONUP:
        EndGripDrag(h, true);
        return 0;
    case WM_CAPTURECHANGED:
        EndGripDrag(h, false);  // capture stolen: stop where we are rather than keep tracking
        return 0;
    case WM_NCRBUTTONUP:
        // The case hit-tests as HTCAPTION, so a right-click on the clock normally arrives here
        // rather than as WM_CONTEXTMENU. Handling it also stops DefWindowProc opening the system
        // menu.
        ShowContextMenu(h);
        return 0;
    case WM_RBUTTONUP:
        // The client-area twin of the above: a right-click lands here instead whenever
        // hit-testing was bypassed or answered HTCLIENT -- over the grip, or while capture is
        // held. Both routes open the menu, deliberately, because the menu is the only way to
        // reach Exit: if input ever gets into a state this file did not predict, a right-click
        // has to still work. Cheap insurance for the one control that must never be lost.
        ShowContextMenu(h);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g_clock.Resize(); g_needsPaint = true; }
        return 0;
    case WM_DPICHANGED: {
        // Dragged onto a monitor with a different scaling factor. The clock's size is a raw pixel
        // scale the user chose, so following the DPI is what keeps it the same *physical* size --
        // do nothing and it visibly shrinks or grows crossing the boundary.
        //
        // The size is computed from the new scale rather than taken from the suggested rect,
        // which is only approximately our aspect ratio: Resize() asserts that the window is
        // exactly the case plus its padding on both sides, and the OS's suggestion need not
        // satisfy that. Only the position comes from the message.
        EndGripDrag(h, false);  // don't fight a live drag across the boundary
        const RECT* sug = (const RECT*)lp;
        const UINT dpi = HIWORD(wp);
        const float s = g_clock.ClampScale(g_clock.Scale() * (float)dpi / (float)g_dpi);
        g_dpi = dpi;
        const float pad = Clock::PadFor(s);
        SetWindowPos(h, nullptr, sug->left, sug->top,
            (int)ceilf(g_clock.CaseW() * s + 2 * pad), (int)ceilf(g_clock.CaseH() * s + 2 * pad),
            SWP_NOZORDER | SWP_NOACTIVATE);
        CaptureLayout(h);
        SaveSettings();
        g_needsPaint = true;
        return 0;
    }
    case WM_DISPLAYCHANGE:
        // A monitor was unplugged, or one changed resolution under us. Either can leave the clock
        // at coordinates that no longer exist -- and with no taskbar button and nothing to drag,
        // an off-screen clock is only reachable from the tray icon, which may be switched off.
        ClampToMonitor(h);
        CaptureLayout(h);
        SaveSettings();
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) {  // taskbar moved, resized or un-hidden
            ClampToMonitor(h);
            CaptureLayout(h);
            SaveSettings();
            return 0;
        }
        // Light/dark switched while we are running. Re-flush first so the next right-click opens
        // in the new colours, then drive the About box: it is top-level and gets this broadcast
        // too, but the order between us is undefined and its answer would be stale.
        if (lp && !lstrcmpW((const wchar_t*)lp, L"ImmersiveColorSet")) {
            ReflushMenuTheme();
            AboutThemeChanged();
        }
        return 0;
    case WM_PAINT:
        ValidateRect(h, nullptr);  // clear OS dirty region; we paint from the loop, not from here
        g_needsPaint = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;  // we own every pixel; letting GDI flash the background causes tearing
    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP || lp == WM_CONTEXTMENU || lp == WM_RBUTTONUP) ShowContextMenu(h);
        return 0;
    case WM_ENDRESIZE:
        // The user clicked something that is not the clock. Posted, not done inline, so the hook
        // proc stays as short as a low-level hook proc has to be.
        ApplyResizeMode(h, false);
        return 0;
    case WM_DESTROY:
        ApplyResizeMode(h, false);  // takes the hook down with it
        SaveSettings();
        RemoveTrayIcon(h);  // unconditional: a leftover icon sits there until someone hovers it
        if (g_about) DestroyWindow(g_about);  // ownerless, so it does not go when we do
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
#ifdef _DEBUG
    SelfTest();
    DragSelfTest();
#endif
    // Nothing here currently *needs* an initialised apartment -- the shell calls we make
    // (Shell_NotifyIconW, SHGetFolderPathW) don't, and D3D/D2D/DWrite/DComp hand out their objects
    // through their own factories rather than CoCreateInstance. It is here as insurance for the
    // next shell API someone reaches for. RAII because wWinMain has three exit paths and a missed
    // CoUninitialize on one of them is exactly the kind of thing nobody notices.
    struct ComInit {
        ComInit() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); }
        ~ComInit() { CoUninitialize(); }
    } comInit;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = inst;

    // The manifest binds us to Common Controls v6; this is what actually loads it, so the About
    // box's controls are the themed ones.
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    InitDarkMode();  // opt the process into dark menus, and resolve the ordinals IsDarkMode uses
    LocInit();       // before anything builds a menu or a window title

    LoadSettings();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DeskFlipWindow";
    RegisterClassExW(&wc);

    // The window is exactly the case, padded for the shadow (Clock::WindowRectFor) -- not the
    // whole monitor. Surviving Win+D at this size depends on ApplyTopmost's desktop-owner trick,
    // not on window size (see FindDesktopView).
    const RECT wr = Clock::WindowRectFor(g_settings.showSeconds, g_settings.scalePct / 100.f,
        g_settings.posX, g_settings.posY);

    // WS_EX_NOREDIRECTIONBITMAP, not WS_EX_LAYERED, and the difference is the whole resize bug.
    //
    // Both suppress the redirection surface -- the opaque bitmap DWM would otherwise paint for a
    // window -- which is why the layered version looked transparent. But a layered window is
    // hit-tested by the OS against the LAYER's shape, and that shape is fixed at creation: this
    // window never calls UpdateLayeredWindow or SetLayeredWindowAttributes, so nothing ever
    // resized it. Grow the clock past its startup size and the OS stops delivering WM_NCHITTEST
    // for the new area entirely, however correctly we would have answered -- no dragging, no grip,
    // no menu out there. Measured, one process grown 335x124 -> 1116x414: our own hit-testing
    // tracked the window exactly, while WindowFromPoint kept reporting us only inside
    // (0,0)-(332,120), the startup size, unchanged.
    //
    // NOREDIRECTIONBITMAP removes the surface without making the window layered, so the ordinary
    // hit-testing path applies and it tracks the real size. It is creation-only -- there is no
    // SetWindowLong equivalent -- which is fine, because nothing here ever wanted to toggle it.
    // Dropping the layer *without* putting this in its place is the one thing that must not
    // happen: the window then gets a real redirection surface, and a resize leaves the
    // newly-exposed area painted opaque black around the clock.
    //
    // No WS_EX_TRANSPARENT either: the clock is meant to be grabbed, so it takes its own clicks.
    // NOACTIVATE keeps clicks and drags from stealing focus (a widget should never take your
    // keyboard), TOOLWINDOW keeps us out of Alt-Tab.
    const DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"DeskFlip", WS_POPUP,
        wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    if (!g_clock.Init(hwnd, g_settings.showSeconds, g_settings.useWarp, g_settings.lightMode)) {
        MessageBoxW(nullptr, L"Direct3D 11 / Direct2D initialisation failed.", L"DeskFlip", MB_ICONERROR);
        return 1;
    }
    // The "from" value for the first WM_DPICHANGED. Read after the window exists, so it is the
    // DPI of the monitor the clock actually opened on, not the primary one.
    g_dpi = GetDpiForWindow(hwnd);

    ApplyTopmost(hwnd);
    ClampToMonitor(hwnd);  // a saved position can outlive the monitor it was saved on
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    auto Now = [&] {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return double(c.QuadPart - start.QuadPart) / double(freq.QuadPart);
        };

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // Sleep (not spin) until an absolute FILETIME, or until a message arrives.
    auto SleepUntilFileTime = [&](ULONGLONG ft100ns) {
        LARGE_INTEGER due;
        due.QuadPart = (LONGLONG)ft100ns;  // positive => absolute UTC
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        MsgWaitForMultipleObjects(1, &timer, FALSE, INFINITE, QS_ALLINPUT);
        };

    bool running = true;
    while (running) {
        const bool animating = g_clock.Animating() && !IsIconic(hwnd);

        if (!animating) {
            // Nothing is moving. Sleep to the next second boundary -- absolute, so it can't drift
            // off the system clock. 0% CPU in here.
            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER t;
            t.LowPart = ft.dwLowDateTime;
            t.HighPart = ft.dwHighDateTime;
            constexpr ULONGLONG kSecond = 10'000'000ull;
            SleepUntilFileTime(((t.QuadPart / kSecond) + 1) * kSecond);
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            // The About box is a plain window, not a real dialog, so Tab, Escape and mnemonics
            // only work if this loop offers it the message first.
            if (g_about && IsDialogMessageW(g_about, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        // Applies at most once per drained batch, not once per WM_MOUSEMOVE -- see FlushDrag.
        FlushDrag(hwnd);

        const double now = Now();
        const bool changed = g_clock.Tick(now);

        // Render purely on demand. The waitable object inside Render() handles the vsync queue
        // pacing while animating; an idle desktop does zero GPU work.
        if (!IsIconic(hwnd) && (changed || g_clock.Animating() || g_needsPaint)) {
            g_clock.Render(now);
            g_needsPaint = false;
        }
    }

    CloseHandle(timer);
    g_clock.Shutdown();
    return 0;
}
