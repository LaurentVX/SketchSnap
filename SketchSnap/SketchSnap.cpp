// SketchSnap.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "SketchSnap.h"

#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <strsafe.h>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

#define MAX_LOADSTRING 100

// Custom messages and IDs
#define WM_TRAYICON             (WM_USER + 1)
#define WM_TRIGGER_OVERLAY      (WM_USER + 2)
#define ID_HOTKEY_PRINTSCR      1
#define ID_HOTKEY_CTRLSHIFT     2
#define ID_TRAY_ABOUT           2001
#define ID_TRAY_EXIT            2002
#define ID_TRAY_SETTINGS        2003

// Toast notification constants
#define IDT_TOAST_FADEIN        4001
#define IDT_TOAST_HOLD          4002
#define IDT_TOAST_FADEOUT       4003
#define TOAST_WIDTH             380
#define TOAST_HEIGHT            62
#define TOAST_HOLD_MS           1500
#define TOAST_FADE_STEP_MS      20
#define TOAST_MAX_ALPHA         230

// Settings dialog control IDs
#define IDC_EDIT_FOLDER         3001
#define IDC_BTN_BROWSE          3002
#define IDC_RADIO_PNG           3003
#define IDC_RADIO_JPG           3004
#define IDC_BTN_OK              3005
#define IDC_BTN_CANCEL          3006

// Registry key for persisting settings
static const WCHAR* REG_KEY = L"Software\\SketchSnap";

// Overlay window class name
static const WCHAR* OVERLAY_CLASS = L"ScreenShotOverlayClass";
static const WCHAR* TOAST_CLASS = L"ScreenShotToastClass";

// Global Variables:
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
NOTIFYICONDATA nid = {};
ULONG_PTR gdiplusToken = 0;
HHOOK hKeyboardHook = nullptr;
HWND g_hWnd = nullptr;

// Last saved screenshot path (for toast click)
WCHAR g_lastSavedPath[MAX_PATH] = {};

// --- Toast state ---
HWND g_hToast = nullptr;
WCHAR g_toastText[MAX_PATH] = {};
int g_toastAlpha = 0;

// --- Settings ---
WCHAR g_saveFolderPath[MAX_PATH] = {};  // where to save screenshots
enum ImageFormat { FMT_PNG = 0, FMT_JPG = 1 };
ImageFormat g_imageFormat = FMT_PNG;

// --- Overlay state ---
HWND g_hOverlay = nullptr;
HBITMAP g_hScreenBitmap = nullptr;
int g_screenX = 0, g_screenY = 0, g_screenW = 0, g_screenH = 0;

// Drawing data
struct DrawLine
{
    POINT pt1;
    POINT pt2;
    COLORREF color;
    int thickness;
};

struct DrawRect
{
    RECT rc;
    COLORREF color;
    int thickness;
};

// Annotation color palette
static const COLORREF g_colorPalette[] =
{
    RGB(255, 0, 0),      // Red (default)
    RGB(255, 200, 0),    // Yellow
    RGB(0, 200, 0),      // Green
    RGB(255, 128, 0),    // Orange
    RGB(160, 32, 240),   // Purple
    RGB(139, 69, 19),    // Brown
};
static const WCHAR* g_colorNames[] =
{
    L"Red", L"Yellow", L"Green", L"Orange", L"Purple", L"Brown"
};
static const int g_colorCount = sizeof(g_colorPalette) / sizeof(g_colorPalette[0]);

int g_currentColorIndex = 0;   // index into g_colorPalette (default: Red)
int g_penThickness = 3;        // freehand pen thickness (min 1, max 20)
static const int PEN_THICKNESS_MIN = 1;
static const int PEN_THICKNESS_MAX = 20;

// Mouse position for cursor indicator
POINT g_lastMousePos = {};

std::vector<DrawLine> g_lines;
std::vector<DrawRect> g_rects;

// Right mouse drawing state
bool g_isDrawingFreehand = false;
POINT g_lastFreehandPt = {};

bool g_isDrawingRect = false;
POINT g_rectStartPt = {};
RECT g_currentRect = {};

// Left mouse crop state
bool g_isCropping = false;
POINT g_cropStart = {};
RECT g_cropRect = {};

// Forward declarations
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK    OverlayWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK    ToastWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK    LowLevelKeyboardProc(int, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    SettingsDlgProc(HWND, UINT, WPARAM, LPARAM);
void                ShowOverlay();
void                CloseOverlay();
void                SaveAnnotatedScreenshot(const RECT* cropRect);
void                PaintOverlay(HWND hWnd, HDC hdc);
int                 GetEncoderClsid(const WCHAR* format, CLSID* pClsid);
RECT                NormalizeRect(POINT p1, POINT p2);
void                LoadSettings();
void                SaveSettings();
void                EnsureSaveFolderExists();
void                ShowSettingsDialog(HWND hParent);
void                ShowToast(const WCHAR* filePath);
void                DismissToast();

// ============================================================================
// Settings: load from registry (with defaults)
// ============================================================================
void LoadSettings()
{
    // Default: Pictures\SketchSnap
    WCHAR picturesPath[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, 0, picturesPath);
    StringCchPrintfW(g_saveFolderPath, MAX_PATH, L"%s\\SketchSnap", picturesPath);
    g_imageFormat = FMT_PNG;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD cbData = sizeof(g_saveFolderPath);
        RegQueryValueExW(hKey, L"SaveFolder", nullptr, nullptr,
            (LPBYTE)g_saveFolderPath, &cbData);

        DWORD fmt = 0;
        DWORD cbFmt = sizeof(fmt);
        if (RegQueryValueExW(hKey, L"ImageFormat", nullptr, nullptr,
            (LPBYTE)&fmt, &cbFmt) == ERROR_SUCCESS)
        {
            g_imageFormat = (fmt == 1) ? FMT_JPG : FMT_PNG;
        }

        RegCloseKey(hKey);
    }

    EnsureSaveFolderExists();
}

// ============================================================================
// Settings: save to registry
// ============================================================================
void SaveSettings()
{
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
        0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, L"SaveFolder", 0, REG_SZ,
            (const BYTE*)g_saveFolderPath,
            (DWORD)((wcslen(g_saveFolderPath) + 1) * sizeof(WCHAR)));

        DWORD fmt = (DWORD)g_imageFormat;
        RegSetValueExW(hKey, L"ImageFormat", 0, REG_DWORD,
            (const BYTE*)&fmt, sizeof(fmt));

        RegCloseKey(hKey);
    }
}

// ============================================================================
// Ensure the save folder exists (create if needed)
// ============================================================================
void EnsureSaveFolderExists()
{
    // SHCreateDirectoryExW creates all intermediate directories
    SHCreateDirectoryExW(nullptr, g_saveFolderPath, nullptr);
}

// ============================================================================
// Settings dialog (built at runtime, no .rc needed)
// ============================================================================
void ShowSettingsDialog(HWND hParent)
{
    // Build a dialog template in memory
    // This avoids modifying the .rc file
    struct
    {
        DLGTEMPLATE dlg;
        WORD menu;
        WORD wndClass;
        WCHAR title[32];
    } dlgTemplate = {};

    dlgTemplate.dlg.style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlgTemplate.dlg.cx = 260;
    dlgTemplate.dlg.cy = 110;
    dlgTemplate.dlg.cdit = 0;
    dlgTemplate.menu = 0;
    dlgTemplate.wndClass = 0;
    StringCchCopyW(dlgTemplate.title, 32, L"Settings");

    DialogBoxIndirectParamW(hInst, &dlgTemplate.dlg, hParent, SettingsDlgProc, 0);
}

INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (message)
    {
    case WM_INITDIALOG:
    {
        // Set a reasonable dialog font
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // --- "Save folder:" label ---
        HWND hLabel = CreateWindowW(L"STATIC", L"Save folder:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 12, 70, 16, hDlg, nullptr, hInst, nullptr);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Folder path edit ---
        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_saveFolderPath,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            80, 10, 290, 22, hDlg, (HMENU)(INT_PTR)IDC_EDIT_FOLDER, hInst, nullptr);
        SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Browse button ---
        HWND hBrowse = CreateWindowW(L"BUTTON", L"...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            374, 10, 30, 22, hDlg, (HMENU)(INT_PTR)IDC_BTN_BROWSE, hInst, nullptr);
        SendMessage(hBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- "Format:" label ---
        HWND hFmtLabel = CreateWindowW(L"STATIC", L"Format:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 44, 70, 16, hDlg, nullptr, hInst, nullptr);
        SendMessage(hFmtLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- PNG radio ---
        HWND hPng = CreateWindowW(L"BUTTON", L"PNG",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            80, 42, 60, 20, hDlg, (HMENU)(INT_PTR)IDC_RADIO_PNG, hInst, nullptr);
        SendMessage(hPng, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- JPG radio ---
        HWND hJpg = CreateWindowW(L"BUTTON", L"JPG",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            150, 42, 60, 20, hDlg, (HMENU)(INT_PTR)IDC_RADIO_JPG, hInst, nullptr);
        SendMessage(hJpg, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Set current selection
        CheckDlgButton(hDlg, (g_imageFormat == FMT_PNG) ? IDC_RADIO_PNG : IDC_RADIO_JPG, BST_CHECKED);

        // --- OK button ---
        HWND hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            240, 75, 80, 26, hDlg, (HMENU)(INT_PTR)IDC_BTN_OK, hInst, nullptr);
        SendMessage(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Cancel button ---
        HWND hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            324, 75, 80, 26, hDlg, (HMENU)(INT_PTR)IDC_BTN_CANCEL, hInst, nullptr);
        SendMessage(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Resize dialog to fit controls (dialog units → pixels are tricky,
        // so just SetWindowPos to a fixed pixel size)
        SetWindowPos(hDlg, nullptr, 0, 0, 420, 145,
            SWP_NOMOVE | SWP_NOZORDER);

        return (INT_PTR)TRUE;
    }

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        switch (wmId)
        {
        case IDC_BTN_BROWSE:
        {
            // Open folder picker
            BROWSEINFOW bi = {};
            bi.hwndOwner = hDlg;
            bi.lpszTitle = L"Select screenshot save folder";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl)
            {
                WCHAR folderPath[MAX_PATH] = {};
                if (SHGetPathFromIDListW(pidl, folderPath))
                {
                    SetDlgItemTextW(hDlg, IDC_EDIT_FOLDER, folderPath);
                }
                CoTaskMemFree(pidl);
            }
        }
        break;

        case IDC_BTN_OK:
        {
            // Read folder path
            GetDlgItemTextW(hDlg, IDC_EDIT_FOLDER, g_saveFolderPath, MAX_PATH);

            // Read format
            g_imageFormat = IsDlgButtonChecked(hDlg, IDC_RADIO_JPG) ? FMT_JPG : FMT_PNG;

            // Persist and ensure folder exists
            SaveSettings();
            EnsureSaveFolderExists();

            EndDialog(hDlg, IDOK);
        }
        break;

        case IDC_BTN_CANCEL:
            EndDialog(hDlg, IDCANCEL);
            break;
        }
    }
    break;

    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return (INT_PTR)TRUE;
    }

    return (INT_PTR)FALSE;
}

// ============================================================================
// Toast notification — custom popup at bottom-right, click to open folder
// ============================================================================
void DismissToast()
{
    if (g_hToast)
    {
        KillTimer(g_hToast, IDT_TOAST_FADEIN);
        KillTimer(g_hToast, IDT_TOAST_HOLD);
        KillTimer(g_hToast, IDT_TOAST_FADEOUT);
        DestroyWindow(g_hToast);
        g_hToast = nullptr;
    }
}

void ShowToast(const WCHAR* filePath)
{
    DismissToast();

    StringCchCopyW(g_toastText, MAX_PATH, filePath);

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int x = workArea.right - TOAST_WIDTH - 12;
    int y = workArea.bottom - TOAST_HEIGHT - 12;

    g_hToast = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        TOAST_CLASS, L"", WS_POPUP,
        x, y, TOAST_WIDTH, TOAST_HEIGHT,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hToast) return;

    g_toastAlpha = 0;
    SetLayeredWindowAttributes(g_hToast, 0, 0, LWA_ALPHA);
    ShowWindow(g_hToast, SW_SHOWNOACTIVATE);
    InvalidateRect(g_hToast, nullptr, TRUE);
    UpdateWindow(g_hToast);

    SetTimer(g_hToast, IDT_TOAST_FADEIN, TOAST_FADE_STEP_MS, nullptr);
}

LRESULT CALLBACK ToastWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);

        // Dark background
        HBRUSH hBg = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);

        // Green left accent bar
        RECT rcAccent = { 0, 0, 4, rc.bottom };
        HBRUSH hAccent = CreateSolidBrush(RGB(0, 180, 80));
        FillRect(hdc, &rcAccent, hAccent);
        DeleteObject(hAccent);

        SetBkMode(hdc, TRANSPARENT);

        // Title line
        HFONT hBold = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hBold);
        SetTextColor(hdc, RGB(255, 255, 255));
        RECT rcTitle = { 14, 6, rc.right - 8, 24 };
        DrawTextW(hdc, L"\x2714  Screenshot Saved", -1, &rcTitle, DT_LEFT | DT_SINGLELINE);
        DeleteObject(hBold);

        // File path
        HFONT hSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(hdc, hSmall);
        SetTextColor(hdc, RGB(180, 180, 180));
        RECT rcPath = { 14, 26, rc.right - 8, 40 };
        DrawTextW(hdc, g_toastText, -1, &rcPath, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Click hint
        SetTextColor(hdc, RGB(100, 160, 255));
        RECT rcHint = { 14, 42, rc.right - 8, rc.bottom - 2 };
        DrawTextW(hdc, L"Click to open folder", -1, &rcHint, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, hOldFont);
        DeleteObject(hSmall);

        EndPaint(hWnd, &ps);
    }
    break;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;

    case WM_LBUTTONUP:
    {
        if (g_lastSavedPath[0] != L'\0')
        {
            PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(g_lastSavedPath);
            if (pidl)
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        }
        DismissToast();
    }
    break;

    case WM_TIMER:
        if (wParam == IDT_TOAST_FADEIN)
        {
            g_toastAlpha = min(g_toastAlpha + 25, TOAST_MAX_ALPHA);
            SetLayeredWindowAttributes(hWnd, 0, (BYTE)g_toastAlpha, LWA_ALPHA);
            if (g_toastAlpha >= TOAST_MAX_ALPHA)
            {
                KillTimer(hWnd, IDT_TOAST_FADEIN);
                SetTimer(hWnd, IDT_TOAST_HOLD, TOAST_HOLD_MS, nullptr);
            }
        }
        else if (wParam == IDT_TOAST_HOLD)
        {
            KillTimer(hWnd, IDT_TOAST_HOLD);
            SetTimer(hWnd, IDT_TOAST_FADEOUT, TOAST_FADE_STEP_MS, nullptr);
        }
        else if (wParam == IDT_TOAST_FADEOUT)
        {
            g_toastAlpha = max(g_toastAlpha - 20, 0);
            SetLayeredWindowAttributes(hWnd, 0, (BYTE)g_toastAlpha, LWA_ALPHA);
            if (g_toastAlpha <= 0)
            {
                DismissToast();
            }
        }
        break;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_TOAST_FADEIN);
        KillTimer(hWnd, IDT_TOAST_HOLD);
        KillTimer(hWnd, IDT_TOAST_FADEOUT);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ============================================================================
// Entry point
// ============================================================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    // Load user settings from registry
    LoadSettings();

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_SketchSnap, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Register overlay window class
    WNDCLASSEXW wcOverlay = {};
    wcOverlay.cbSize = sizeof(WNDCLASSEX);
    wcOverlay.style = CS_HREDRAW | CS_VREDRAW;
    wcOverlay.lpfnWndProc = OverlayWndProc;
    wcOverlay.hInstance = hInstance;
    wcOverlay.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wcOverlay.hbrBackground = nullptr;
    wcOverlay.lpszClassName = OVERLAY_CLASS;
    RegisterClassExW(&wcOverlay);

    // Register toast window class
    WNDCLASSEXW wcToast = {};
    wcToast.cbSize = sizeof(WNDCLASSEX);
    wcToast.style = CS_HREDRAW | CS_VREDRAW;
    wcToast.lpfnWndProc = ToastWndProc;
    wcToast.hInstance = hInstance;
    wcToast.hCursor = LoadCursor(nullptr, IDC_HAND);
    wcToast.hbrBackground = nullptr;
    wcToast.lpszClassName = TOAST_CLASS;
    RegisterClassExW(&wcToast);

    // Perform application initialization:
    if (!InitInstance(hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_SketchSnap));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        // WM_HOTKEY is a thread message (hwnd == NULL).
        if (msg.message == WM_HOTKEY)
        {
            if (msg.wParam == ID_HOTKEY_PRINTSCR || msg.wParam == ID_HOTKEY_CTRLSHIFT)
            {
                ShowOverlay();
            }
            continue;
        }

        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    // Cleanup
    if (hKeyboardHook)
        UnhookWindowsHookEx(hKeyboardHook);
    Gdiplus::GdiplusShutdown(gdiplusToken);

    return (int)msg.wParam;
}

// ============================================================================
// Register main (tray) window class
// ============================================================================
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SketchSnap));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_SketchSnap);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

// ============================================================================
// Low-level keyboard hook
// ============================================================================
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN)
    {
        KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;
        if (pKey->vkCode == VK_SNAPSHOT)
        {
            PostMessageW(g_hWnd, WM_TRIGGER_OVERLAY, 0, 0);
        }
    }
    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

// ============================================================================
// InitInstance — tray icon + hotkeys
// ============================================================================
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    UNREFERENCED_PARAMETER(nCmdShow);

    hInst = hInstance;

    HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd)
    {
        return FALSE;
    }

    g_hWnd = hWnd;

    // Set up the system tray icon
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SketchSnap));
    StringCchCopyW(nid.szTip, ARRAYSIZE(nid.szTip), L"SketchSnap - PrtScr or Ctrl+Shift+S");
    Shell_NotifyIconW(NIM_ADD, &nid);

    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);

    RegisterHotKey(hWnd, ID_HOTKEY_PRINTSCR, 0, VK_SNAPSHOT);

    if (!RegisterHotKey(hWnd, ID_HOTKEY_CTRLSHIFT, MOD_CONTROL | MOD_SHIFT, 'S'))
    {
        MessageBoxW(hWnd, L"Failed to register Ctrl+Shift+S hotkey.",
            L"SketchSnap", MB_ICONWARNING);
    }

    hKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

    return TRUE;
}

// ============================================================================
// Helper: normalize two points into a RECT
// ============================================================================
RECT NormalizeRect(POINT p1, POINT p2)
{
    RECT rc;
    rc.left   = min(p1.x, p2.x);
    rc.top    = min(p1.y, p2.y);
    rc.right  = max(p1.x, p2.x);
    rc.bottom = max(p1.y, p2.y);
    return rc;
}

// ============================================================================
// Show the fullscreen overlay with a frozen screenshot
// ============================================================================
void ShowOverlay()
{
    if (g_hOverlay)
        return;

    g_screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hScreenDC = GetDC(nullptr);
    HDC hMemDC = CreateCompatibleDC(hScreenDC);
    g_hScreenBitmap = CreateCompatibleBitmap(hScreenDC, g_screenW, g_screenH);
    HBITMAP hOld = (HBITMAP)SelectObject(hMemDC, g_hScreenBitmap);
    BitBlt(hMemDC, 0, 0, g_screenW, g_screenH, hScreenDC, g_screenX, g_screenY, SRCCOPY);
    SelectObject(hMemDC, hOld);
    DeleteDC(hMemDC);
    ReleaseDC(nullptr, hScreenDC);

    // Clear previous annotations
    g_lines.clear();
    g_rects.clear();
    g_isDrawingFreehand = false;
    g_isDrawingRect = false;
    g_isCropping = false;
    g_currentRect = {};
    g_cropRect = {};
    g_currentColorIndex = 0;  // reset to Red
    g_penThickness = 3;       // reset to default thickness

    // Create a topmost popup window covering the entire virtual screen
    g_hOverlay = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS,
        L"ScreenShotOverlay",
        WS_POPUP,
        g_screenX, g_screenY, g_screenW, g_screenH,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hOverlay, SW_SHOW);
    SetForegroundWindow(g_hOverlay);
    SetCapture(g_hOverlay);
}

// ============================================================================
// Close overlay without saving
// ============================================================================
void CloseOverlay()
{
    if (!g_hOverlay)
        return;

    ReleaseCapture();
    DestroyWindow(g_hOverlay);
    g_hOverlay = nullptr;

    if (g_hScreenBitmap)
    {
        DeleteObject(g_hScreenBitmap);
        g_hScreenBitmap = nullptr;
    }
}

// ============================================================================
// Paint the overlay
// ============================================================================
void PaintOverlay(HWND hWnd, HDC hdc)
{
    HDC hMemDC = CreateCompatibleDC(hdc);
    HBITMAP hOld = (HBITMAP)SelectObject(hMemDC, g_hScreenBitmap);
    BitBlt(hdc, 0, 0, g_screenW, g_screenH, hMemDC, 0, 0, SRCCOPY);
    SelectObject(hMemDC, hOld);
    DeleteDC(hMemDC);

    // Blueish tint
    {
        HDC hDimDC = CreateCompatibleDC(hdc);
        HBITMAP hDimBmp = CreateCompatibleBitmap(hdc, g_screenW, g_screenH);
        HBITMAP hDimOld = (HBITMAP)SelectObject(hDimDC, hDimBmp);

        RECT rcFull = { 0, 0, g_screenW, g_screenH };
        HBRUSH hDimBrush = CreateSolidBrush(RGB(0, 30, 80));
        FillRect(hDimDC, &rcFull, hDimBrush);
        DeleteObject(hDimBrush);

        BLENDFUNCTION bf = {};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 60;
        AlphaBlend(hdc, 0, 0, g_screenW, g_screenH, hDimDC, 0, 0, g_screenW, g_screenH, bf);

        SelectObject(hDimDC, hDimOld);
        DeleteObject(hDimBmp);
        DeleteDC(hDimDC);
    }

    // Freehand lines (each with its own color and thickness)
    for (const auto& line : g_lines)
    {
        HPEN hPen = CreatePen(PS_SOLID, line.thickness, line.color);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, line.pt1.x, line.pt1.y, nullptr);
        LineTo(hdc, line.pt2.x, line.pt2.y);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    // Rectangles (each with its own color and thickness)
    HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hNullBrush);

    for (const auto& r : g_rects)
    {
        HPEN hPen = CreatePen(PS_SOLID, r.thickness, r.color);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        Rectangle(hdc, r.rc.left, r.rc.top, r.rc.right, r.rc.bottom);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
    }

    if (g_isDrawingRect)
    {
        HPEN hDashPen = CreatePen(PS_DASH, g_penThickness, g_colorPalette[g_currentColorIndex]);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hDashPen);
        Rectangle(hdc, g_currentRect.left, g_currentRect.top,
            g_currentRect.right, g_currentRect.bottom);
        SelectObject(hdc, hOldPen);
        DeleteObject(hDashPen);
    }

    if (g_isCropping)
    {
        HPEN hCropPen = CreatePen(PS_DASH, 2, RGB(0, 200, 255));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hCropPen);
        Rectangle(hdc, g_cropRect.left, g_cropRect.top,
            g_cropRect.right, g_cropRect.bottom);
        SelectObject(hdc, hOldPen);
        DeleteObject(hCropPen);
    }

    SelectObject(hdc, hOldBrush);

    // Help text
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    const WCHAR* helpText =
        L"RMB: Draw  |  Ctrl+RMB: Rect  |  LMB: Save  |  Ctrl+LMB: Crop  |  "
        L"Ctrl+Wheel: Size  |  Shift+Wheel: Color  |  Esc: Cancel";

    RECT rcText = { 0, 10, g_screenW, 40 };
    DrawTextW(hdc, helpText, -1, &rcText, DT_CENTER | DT_SINGLELINE);

    // --- Cursor color/thickness indicator ---
    {
        COLORREF curColor = g_colorPalette[g_currentColorIndex];
        int cx = g_lastMousePos.x;
        int cy = g_lastMousePos.y;

        // Draw a filled circle at cursor position showing current color and size
        int radius = max(g_penThickness / 2, 2);
        HBRUSH hFillBrush = CreateSolidBrush(curColor);
        HPEN hOutlinePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HPEN hPrevPen = (HPEN)SelectObject(hdc, hOutlinePen);
        HBRUSH hPrevBrush = (HBRUSH)SelectObject(hdc, hFillBrush);
        Ellipse(hdc, cx - radius, cy - radius, cx + radius, cy + radius);
        SelectObject(hdc, hPrevBrush);
        SelectObject(hdc, hPrevPen);
        DeleteObject(hFillBrush);
        DeleteObject(hOutlinePen);

        // Draw color name + thickness label near cursor
        WCHAR label[64] = {};
        StringCchPrintfW(label, 64, L"%s  [%d]", g_colorNames[g_currentColorIndex], g_penThickness);
        HFONT hLabelFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT hPrevFont = (HFONT)SelectObject(hdc, hLabelFont);
        SetTextColor(hdc, curColor);
        RECT rcLabel = { cx + radius + 6, cy - 8, cx + radius + 160, cy + 12 };
        DrawTextW(hdc, label, -1, &rcLabel, DT_LEFT | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(hdc, hPrevFont);
        DeleteObject(hLabelFont);
    }
}

// ============================================================================
// Save the annotated screenshot (full or cropped)
// ============================================================================
void SaveAnnotatedScreenshot(const RECT* cropRect)
{
    HDC hScreenDC = GetDC(nullptr);
    HDC hCompDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hCompBmp = CreateCompatibleBitmap(hScreenDC, g_screenW, g_screenH);
    HBITMAP hCompOld = (HBITMAP)SelectObject(hCompDC, hCompBmp);

    // Original screenshot
    HDC hSrcDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hSrcOld = (HBITMAP)SelectObject(hSrcDC, g_hScreenBitmap);
    BitBlt(hCompDC, 0, 0, g_screenW, g_screenH, hSrcDC, 0, 0, SRCCOPY);
    SelectObject(hSrcDC, hSrcOld);
    DeleteDC(hSrcDC);

    // Annotations
    for (const auto& line : g_lines)
    {
        HPEN hPen = CreatePen(PS_SOLID, line.thickness, line.color);
        HPEN hOldPen = (HPEN)SelectObject(hCompDC, hPen);
        MoveToEx(hCompDC, line.pt1.x, line.pt1.y, nullptr);
        LineTo(hCompDC, line.pt2.x, line.pt2.y);
        SelectObject(hCompDC, hOldPen);
        DeleteObject(hPen);
    }

    HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hCompDC, hNullBrush);

    for (const auto& r : g_rects)
    {
        HPEN hPen = CreatePen(PS_SOLID, r.thickness, r.color);
        HPEN hOldPen = (HPEN)SelectObject(hCompDC, hPen);
        Rectangle(hCompDC, r.rc.left, r.rc.top, r.rc.right, r.rc.bottom);
        SelectObject(hCompDC, hOldPen);
        DeleteObject(hPen);
    }

    SelectObject(hCompDC, hOldBrush);

    // Determine save region
    HBITMAP hSaveBmp = nullptr;
    int saveW = g_screenW;
    int saveH = g_screenH;

    if (cropRect && (cropRect->right - cropRect->left > 5) &&
        (cropRect->bottom - cropRect->top > 5))
    {
        saveW = cropRect->right - cropRect->left;
        saveH = cropRect->bottom - cropRect->top;
        HDC hCropDC = CreateCompatibleDC(hScreenDC);
        hSaveBmp = CreateCompatibleBitmap(hScreenDC, saveW, saveH);
        HBITMAP hCropOld = (HBITMAP)SelectObject(hCropDC, hSaveBmp);
        BitBlt(hCropDC, 0, 0, saveW, saveH, hCompDC, cropRect->left, cropRect->top, SRCCOPY);
        SelectObject(hCropDC, hCropOld);
        DeleteDC(hCropDC);
    }
    else
    {
        hSaveBmp = hCompBmp;
        hCompBmp = nullptr;
    }

    SelectObject(hCompDC, hCompOld);
    if (hCompBmp)
        DeleteObject(hCompBmp);
    DeleteDC(hCompDC);
    ReleaseDC(nullptr, hScreenDC);

    // Build file path using settings
    EnsureSaveFolderExists();

    SYSTEMTIME st;
    GetLocalTime(&st);

    const WCHAR* ext = (g_imageFormat == FMT_JPG) ? L"jpg" : L"png";
    const WCHAR* mime = (g_imageFormat == FMT_JPG) ? L"image/jpeg" : L"image/png";

    WCHAR filePath[MAX_PATH] = {};
    StringCchPrintfW(filePath, MAX_PATH,
        L"%s\\Screenshot_%04d%02d%02d_%02d%02d%02d.%s",
        g_saveFolderPath, st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, ext);

    // Save using GDI+
    {
        Gdiplus::Bitmap bitmap(hSaveBmp, nullptr);
        CLSID clsid;
        if (GetEncoderClsid(mime, &clsid) >= 0)
        {
            if (g_imageFormat == FMT_JPG)
            {
                // Set JPEG quality to 95
                Gdiplus::EncoderParameters encoderParams;
                encoderParams.Count = 1;
                encoderParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
                encoderParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
                encoderParams.Parameter[0].NumberOfValues = 1;
                ULONG quality = 95;
                encoderParams.Parameter[0].Value = &quality;
                bitmap.Save(filePath, &clsid, &encoderParams);
            }
            else
            {
                bitmap.Save(filePath, &clsid, nullptr);
            }
        }
    }

    // Copy to clipboard
    {
        // Get bitmap dimensions
        BITMAP bm = {};
        GetObject(hSaveBmp, sizeof(bm), &bm);

        BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = bm.bmWidth;
        bi.biHeight = bm.bmHeight;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        DWORD dwBmpSize = bm.bmWidth * bm.bmHeight * 4;

        HDC hDC = GetDC(nullptr);
        HGLOBAL hDIB = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + dwBmpSize);
        if (hDIB)
        {
            void* pDIB = GlobalLock(hDIB);
            if (pDIB)
            {
                memcpy(pDIB, &bi, sizeof(BITMAPINFOHEADER));
                GetDIBits(hDC, hSaveBmp, 0, bm.bmHeight,
                    (BYTE*)pDIB + sizeof(BITMAPINFOHEADER),
                    (BITMAPINFO*)pDIB, DIB_RGB_COLORS);
                GlobalUnlock(hDIB);

                if (OpenClipboard(nullptr))
                {
                    EmptyClipboard();
                    SetClipboardData(CF_DIB, hDIB);
                    CloseClipboard();
                    hDIB = nullptr; // clipboard owns it now
                }
            }
            if (hDIB)
                GlobalFree(hDIB);
        }
        ReleaseDC(nullptr, hDC);
    }

    DeleteObject(hSaveBmp);

    // Remember path for toast click
    StringCchCopyW(g_lastSavedPath, MAX_PATH, filePath);

    // Show custom toast notification
    ShowToast(filePath);
}

// ============================================================================
// GDI+ encoder CLSID helper
// ============================================================================
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    Gdiplus::ImageCodecInfo* pImageCodecInfo =
        (Gdiplus::ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == nullptr) return -1;

    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j)
    {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
        {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }

    free(pImageCodecInfo);
    return -1;
}

// ============================================================================
// Overlay window procedure
// ============================================================================
LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        PaintOverlay(hWnd, hdc);
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_ERASEBKGND:
        return 1;

    case WM_RBUTTONDOWN:
    {
        POINT pt = { (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam) };
        bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrlHeld)
        {
            g_isDrawingRect = true;
            g_rectStartPt = pt;
            g_currentRect = { pt.x, pt.y, pt.x, pt.y };
        }
        else
        {
            g_isDrawingFreehand = true;
            g_lastFreehandPt = pt;
        }
    }
    break;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam) };

        if (g_isDrawingFreehand && (wParam & MK_RBUTTON))
        {
            DrawLine line = { g_lastFreehandPt, pt, g_colorPalette[g_currentColorIndex], g_penThickness };
            g_lines.push_back(line);
            g_lastFreehandPt = pt;
        }

        if (g_isDrawingRect && (wParam & MK_RBUTTON))
        {
            g_currentRect = NormalizeRect(g_rectStartPt, pt);
        }

        if (g_isCropping && (wParam & MK_LBUTTON))
        {
            g_cropRect = NormalizeRect(g_cropStart, pt);
        }

        // Update mouse position for cursor indicator
        g_lastMousePos = pt;
        InvalidateRect(hWnd, nullptr, FALSE);
    }
    break;

    case WM_RBUTTONUP:
    {
        POINT pt = { (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam) };

        if (g_isDrawingRect)
        {
            g_currentRect = NormalizeRect(g_rectStartPt, pt);
            DrawRect dr = { g_currentRect, g_colorPalette[g_currentColorIndex], g_penThickness };
            g_rects.push_back(dr);
            g_isDrawingRect = false;
            g_currentRect = {};
            InvalidateRect(hWnd, nullptr, FALSE);
        }

        g_isDrawingFreehand = false;
    }
    break;

    case WM_LBUTTONDOWN:
    {
        POINT pt = { (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam) };
        bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrlHeld)
        {
            g_isCropping = true;
            g_cropStart = pt;
            g_cropRect = { pt.x, pt.y, pt.x, pt.y };
        }
    }
    break;

    case WM_LBUTTONUP:
    {
        POINT pt = { (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam) };
        bool ctrlHeld = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (g_isCropping && ctrlHeld)
        {
            g_cropRect = NormalizeRect(g_cropStart, pt);
            g_isCropping = false;
            SaveAnnotatedScreenshot(&g_cropRect);
            CloseOverlay();
        }
        else if (!g_isCropping)
        {
            SaveAnnotatedScreenshot(nullptr);
            CloseOverlay();
        }
        else
        {
            g_isCropping = false;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
    }
    break;

    case WM_MOUSEWHEEL:
    {
        short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        bool ctrlHeld = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
        bool shiftHeld = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;

        if (ctrlHeld)
        {
            // Ctrl + mouse wheel: change pen thickness
            if (zDelta > 0)
                g_penThickness = min(g_penThickness + 1, PEN_THICKNESS_MAX);
            else
                g_penThickness = max(g_penThickness - 1, PEN_THICKNESS_MIN);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        else if (shiftHeld)
        {
            // Shift + mouse wheel: cycle annotation color
            if (zDelta > 0)
                g_currentColorIndex = (g_currentColorIndex + 1) % g_colorCount;
            else
                g_currentColorIndex = (g_currentColorIndex - 1 + g_colorCount) % g_colorCount;
            InvalidateRect(hWnd, nullptr, FALSE);
        }
    }
    break;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        return TRUE;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            CloseOverlay();
        }
        break;

    case WM_DESTROY:
        ReleaseCapture();
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ============================================================================
// Main (tray) window procedure
// ============================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TRIGGER_OVERLAY:
        ShowOverlay();
        break;

    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP)
        {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_SETTINGS, L"Settings...");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_ABOUT, L"About");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            PostMessage(hWnd, WM_NULL, 0, 0);
            DestroyMenu(hMenu);
        }
        else if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
        {
            ShowOverlay();
        }
        break;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        switch (wmId)
        {
        case ID_TRAY_SETTINGS:
            ShowSettingsDialog(hWnd);
            break;
        case ID_TRAY_ABOUT:
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case ID_TRAY_EXIT:
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        UNREFERENCED_PARAMETER(hdc);
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_DESTROY:
        UnregisterHotKey(hWnd, ID_HOTKEY_PRINTSCR);
        UnregisterHotKey(hWnd, ID_HOTKEY_CTRLSHIFT);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ============================================================================
// About dialog
// ============================================================================
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
