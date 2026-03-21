// SketchSnap.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "SketchSnap.h"

#include <shellapi.h>
#include <shlobj.h>
#include <gdiplus.h>
#include <strsafe.h>
#include <vector>
#include <mmsystem.h>
#include <commdlg.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

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
#define IDC_CHK_SOUND           3007
#define IDC_EDIT_SOUND          3008
#define IDC_BTN_SOUND_BROWSE    3009
#define IDC_COMBO_COLOR         3010
#define IDC_EDIT_SIZE           3011
#define IDC_CHK_EDITOR          3012
#define IDC_EDIT_EDITOR         3013
#define IDC_BTN_EDITOR_BROWSE   3014
#define IDC_COMBO_HOTKEY        3015
#define IDC_CHK_MOD_CTRL        3016
#define IDC_CHK_MOD_ALT         3017
#define IDC_CHK_MOD_SHIFT       3018
#define IDC_CHK_STARTUP         3019

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

// --- Sound settings ---
bool g_soundEnabled = true;
WCHAR g_soundFilePath[MAX_PATH] = L"C:\\Windows\\Media\\Windows Ding.wav";

// --- Default annotation settings ---
int g_defaultColorIndex = 0;    // index into g_colorPalette (0 = Red)
int g_defaultPenThickness = 3;  // default pen thickness

// --- Image editor settings ---
bool g_openEditorEnabled = false;
WCHAR g_editorPath[MAX_PATH] = L"mspaint.exe";

// --- Hotkey settings ---
struct HotkeyDef { UINT vk; const WCHAR* name; };
static const HotkeyDef g_availableKeys[] =
{
    { VK_SNAPSHOT,  L"Print Screen" },
    { VK_F1,        L"F1" },
    { VK_F2,        L"F2" },
    { VK_F3,        L"F3" },
    { VK_F4,        L"F4" },
    { VK_F5,        L"F5" },
    { VK_F6,        L"F6" },
    { VK_F7,        L"F7" },
    { VK_F8,        L"F8" },
    { VK_F9,        L"F9" },
    { VK_F10,       L"F10" },
    { VK_F11,       L"F11" },
    { VK_F12,       L"F12" },
    { VK_PAUSE,     L"Pause" },
    { VK_SCROLL,    L"Scroll Lock" },
    { VK_INSERT,    L"Insert" },
};
static const int g_availableKeyCount = sizeof(g_availableKeys) / sizeof(g_availableKeys[0]);

UINT g_hotkeyVk = VK_SNAPSHOT;
UINT g_hotkeyModifiers = 0;

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

struct DrawTextAnnotation
{
    POINT pos;
    WCHAR text[256];
    COLORREF color;
    int fontSize;
};

// Annotation color palette
static const COLORREF g_colorPalette[] =
{
    RGB(255, 0, 0),      // Red (default)
    RGB(255, 200, 0),    // Yellow
	RGB(0, 0, 255),      // Blue
    RGB(0, 200, 0),      // Green
    RGB(255, 128, 0),    // Orange
    RGB(160, 32, 240),   // Purple
    RGB(139, 69, 19),    // Brown
};
static const WCHAR* g_colorNames[] =
{
    L"Red", L"Yellow", L"Blue", L"Green", L"Orange", L"Purple", L"Brown"
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
std::vector<DrawTextAnnotation> g_texts;

// Undo history — tracks each drawing action in order
enum ActionType { ACTION_FREEHAND, ACTION_RECT, ACTION_TEXT };
struct UndoEntry
{
    ActionType type;
    size_t startIndex; // for freehand: first index in g_lines
    size_t count;      // for freehand: number of line segments
};
std::vector<UndoEntry> g_undoHistory;
size_t g_freehandStrokeStart = 0; // g_lines.size() when freehand stroke began

// Redo history — stores removed drawing data for restoration
struct RedoEntry
{
    UndoEntry undoInfo;
    std::vector<DrawLine> lines;   // stored lines (for ACTION_FREEHAND)
    std::vector<DrawRect> rects;   // stored rects (for ACTION_RECT)
    std::vector<DrawTextAnnotation> texts; // stored texts (for ACTION_TEXT)
};
std::vector<RedoEntry> g_redoHistory;

// Right mouse drawing state
bool g_isDrawingFreehand = false;
POINT g_lastFreehandPt = {};

bool g_isDrawingRect = false;
POINT g_rectStartPt = {};
RECT g_currentRect = {};

// Text annotation state
bool g_isAnnotatingText = false;
POINT g_textAnnotStartPt = {};
WCHAR g_currentText[256] = L"";

// Left mouse crop state
bool g_isCropping = false;
POINT g_cropStart = {};
RECT g_cropRect = {};

// Text typing state
bool g_isTypingText = false;
POINT g_textPos = {};
WCHAR g_textBuffer[256] = {};
int g_textLen = 0;

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
void                PlayNotificationSound();
void                CommitTextBlock(HWND hWnd);
void                OpenInEditor(const WCHAR* filePath);
void                RegisterScreenshotHotkey();
void                UnregisterScreenshotHotkey();
bool                IsStartupEnabled();
void                SetStartupEnabled(bool enable);

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
    g_soundEnabled = true;
    StringCchCopyW(g_soundFilePath, MAX_PATH, L"C:\\Windows\\Media\\Windows Ding.wav");
    g_defaultColorIndex = 0;
    g_defaultPenThickness = 3;
    g_openEditorEnabled = false;
    StringCchCopyW(g_editorPath, MAX_PATH, L"mspaint.exe");
    g_hotkeyVk = VK_SNAPSHOT;
    g_hotkeyModifiers = 0;

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

        DWORD sndEnabled = 1;
        DWORD cbSnd = sizeof(sndEnabled);
        if (RegQueryValueExW(hKey, L"SoundEnabled", nullptr, nullptr,
            (LPBYTE)&sndEnabled, &cbSnd) == ERROR_SUCCESS)
        {
            g_soundEnabled = (sndEnabled != 0);
        }

        DWORD cbSndPath = sizeof(g_soundFilePath);
        RegQueryValueExW(hKey, L"SoundFile", nullptr, nullptr,
            (LPBYTE)g_soundFilePath, &cbSndPath);

        DWORD colorIdx = 0;
        DWORD cbColor = sizeof(colorIdx);
        if (RegQueryValueExW(hKey, L"DefaultColor", nullptr, nullptr,
            (LPBYTE)&colorIdx, &cbColor) == ERROR_SUCCESS)
        {
            if ((int)colorIdx >= 0 && (int)colorIdx < g_colorCount)
                g_defaultColorIndex = (int)colorIdx;
        }

        DWORD penSize = 3;
        DWORD cbPen = sizeof(penSize);
        if (RegQueryValueExW(hKey, L"DefaultPenSize", nullptr, nullptr,
            (LPBYTE)&penSize, &cbPen) == ERROR_SUCCESS)
        {
            if ((int)penSize >= PEN_THICKNESS_MIN && (int)penSize <= PEN_THICKNESS_MAX)
                g_defaultPenThickness = (int)penSize;
        }

        DWORD editorEnabled = 0;
        DWORD cbEditor = sizeof(editorEnabled);
        if (RegQueryValueExW(hKey, L"OpenEditorEnabled", nullptr, nullptr,
            (LPBYTE)&editorEnabled, &cbEditor) == ERROR_SUCCESS)
        {
            g_openEditorEnabled = (editorEnabled != 0);
        }

        DWORD cbEditorPath = sizeof(g_editorPath);
        RegQueryValueExW(hKey, L"EditorPath", nullptr, nullptr,
            (LPBYTE)g_editorPath, &cbEditorPath);

        DWORD hotkeyVk = VK_SNAPSHOT;
        DWORD cbHkVk = sizeof(hotkeyVk);
        if (RegQueryValueExW(hKey, L"HotkeyVk", nullptr, nullptr,
            (LPBYTE)&hotkeyVk, &cbHkVk) == ERROR_SUCCESS)
        {
            g_hotkeyVk = (UINT)hotkeyVk;
        }

        DWORD hotkeyMod = 0;
        DWORD cbHkMod = sizeof(hotkeyMod);
        if (RegQueryValueExW(hKey, L"HotkeyModifiers", nullptr, nullptr,
            (LPBYTE)&hotkeyMod, &cbHkMod) == ERROR_SUCCESS)
        {
            g_hotkeyModifiers = (UINT)hotkeyMod;
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

        DWORD sndEnabled = g_soundEnabled ? 1 : 0;
        RegSetValueExW(hKey, L"SoundEnabled", 0, REG_DWORD,
            (const BYTE*)&sndEnabled, sizeof(sndEnabled));

        RegSetValueExW(hKey, L"SoundFile", 0, REG_SZ,
            (const BYTE*)g_soundFilePath,
            (DWORD)((wcslen(g_soundFilePath) + 1) * sizeof(WCHAR)));

        DWORD colorIdx = (DWORD)g_defaultColorIndex;
        RegSetValueExW(hKey, L"DefaultColor", 0, REG_DWORD,
            (const BYTE*)&colorIdx, sizeof(colorIdx));

        DWORD penSize = (DWORD)g_defaultPenThickness;
        RegSetValueExW(hKey, L"DefaultPenSize", 0, REG_DWORD,
            (const BYTE*)&penSize, sizeof(penSize));

        DWORD editorEnabled = g_openEditorEnabled ? 1 : 0;
        RegSetValueExW(hKey, L"OpenEditorEnabled", 0, REG_DWORD,
            (const BYTE*)&editorEnabled, sizeof(editorEnabled));

        RegSetValueExW(hKey, L"EditorPath", 0, REG_SZ,
            (const BYTE*)g_editorPath,
            (DWORD)((wcslen(g_editorPath) + 1) * sizeof(WCHAR)));

        DWORD hotkeyVk = (DWORD)g_hotkeyVk;
        RegSetValueExW(hKey, L"HotkeyVk", 0, REG_DWORD,
            (const BYTE*)&hotkeyVk, sizeof(hotkeyVk));

        DWORD hotkeyMod = (DWORD)g_hotkeyModifiers;
        RegSetValueExW(hKey, L"HotkeyModifiers", 0, REG_DWORD,
            (const BYTE*)&hotkeyMod, sizeof(hotkeyMod));

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

        // --- Sound checkbox ---
        HWND hChkSound = CreateWindowW(L"BUTTON", L"Play sound",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 75, 100, 20, hDlg, (HMENU)(INT_PTR)IDC_CHK_SOUND, hInst, nullptr);
        SendMessage(hChkSound, WM_SETFONT, (WPARAM)hFont, TRUE);
        CheckDlgButton(hDlg, IDC_CHK_SOUND, g_soundEnabled ? BST_CHECKED : BST_UNCHECKED);

        // --- Sound file path edit ---
        HWND hSoundEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_soundFilePath,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            120, 75, 180, 22, hDlg, (HMENU)(INT_PTR)IDC_EDIT_SOUND, hInst, nullptr);
        SendMessage(hSoundEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Sound browse button ---
        HWND hSoundBrowse = CreateWindowW(L"BUTTON", L"...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            308, 75, 30, 22, hDlg, (HMENU)(INT_PTR)IDC_BTN_SOUND_BROWSE, hInst, nullptr);
        SendMessage(hSoundBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- "Default color:" label ---
        HWND hColorLabel = CreateWindowW(L"STATIC", L"Default color:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 108, 80, 16, hDlg, nullptr, hInst, nullptr);
        SendMessage(hColorLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Color combo box ---
        HWND hColorCombo = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            100, 105, 100, 200, hDlg, (HMENU)(INT_PTR)IDC_COMBO_COLOR, hInst, nullptr);
        SendMessage(hColorCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        for (int i = 0; i < g_colorCount; i++)
        {
            SendMessageW(hColorCombo, CB_ADDSTRING, 0, (LPARAM)g_colorNames[i]);
        }
        SendMessageW(hColorCombo, CB_SETCURSEL, (WPARAM)g_defaultColorIndex, 0);

        // --- "Default size:" label ---
        HWND hSizeLabel = CreateWindowW(L"STATIC", L"Default size:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            220, 108, 80, 16, hDlg, nullptr, hInst, nullptr);
        SendMessage(hSizeLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Size edit ---
        WCHAR sizeBuf[8] = {};
        StringCchPrintfW(sizeBuf, 8, L"%d", g_defaultPenThickness);
        HWND hSizeEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", sizeBuf,
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_LEFT,
            305, 105, 40, 22, hDlg, (HMENU)(INT_PTR)IDC_EDIT_SIZE, hInst, nullptr);
        SendMessage(hSizeEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- "1-20" hint ---
        HWND hSizeHint = CreateWindowW(L"STATIC", L"(1-20)",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            350, 108, 40, 16, hDlg, nullptr, hInst, nullptr);
        SendMessage(hSizeHint, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Open in editor checkbox ---
        HWND hChkEditor = CreateWindowW(L"BUTTON", L"Open in editor",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 140, 110, 20, hDlg, (HMENU)(INT_PTR)IDC_CHK_EDITOR, hInst, nullptr);
        SendMessage(hChkEditor, WM_SETFONT, (WPARAM)hFont, TRUE);
        CheckDlgButton(hDlg, IDC_CHK_EDITOR, g_openEditorEnabled ? BST_CHECKED : BST_UNCHECKED);

        // --- Editor path edit ---
        HWND hEditorEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_editorPath,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
            120, 138, 250, 22, hDlg, (HMENU)(INT_PTR)IDC_EDIT_EDITOR, hInst, nullptr);
        SendMessage(hEditorEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Editor browse button ---
        HWND hEditorBrowse = CreateWindowW(L"BUTTON", L"...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            374, 138, 30, 22, hDlg, (HMENU)(INT_PTR)IDC_BTN_EDITOR_BROWSE, hInst, nullptr);
        SendMessage(hEditorBrowse, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- "Hotkey:" label ---
        HWND hHkLabel = CreateWindowW(L"STATIC", L"Hotkey:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 173, 50, 16, hDlg, nullptr, hInst, nullptr);
        SendMessage(hHkLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Key combo box ---
        HWND hKeyCombo = CreateWindowW(L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            60, 170, 110, 200, hDlg, (HMENU)(INT_PTR)IDC_COMBO_HOTKEY, hInst, nullptr);
        SendMessage(hKeyCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
        {
            int curKeySel = 0;
            for (int i = 0; i < g_availableKeyCount; i++)
            {
                SendMessageW(hKeyCombo, CB_ADDSTRING, 0, (LPARAM)g_availableKeys[i].name);
                if (g_availableKeys[i].vk == g_hotkeyVk)
                    curKeySel = i;
            }
            SendMessageW(hKeyCombo, CB_SETCURSEL, (WPARAM)curKeySel, 0);
        }

        // --- Modifier checkboxes ---
        HWND hChkCtrl = CreateWindowW(L"BUTTON", L"Ctrl",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            180, 173, 45, 20, hDlg, (HMENU)(INT_PTR)IDC_CHK_MOD_CTRL, hInst, nullptr);
        SendMessage(hChkCtrl, WM_SETFONT, (WPARAM)hFont, TRUE);
        CheckDlgButton(hDlg, IDC_CHK_MOD_CTRL, (g_hotkeyModifiers & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED);

        HWND hChkAlt = CreateWindowW(L"BUTTON", L"Alt",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            230, 173, 40, 20, hDlg, (HMENU)(INT_PTR)IDC_CHK_MOD_ALT, hInst, nullptr);
        SendMessage(hChkAlt, WM_SETFONT, (WPARAM)hFont, TRUE);
        CheckDlgButton(hDlg, IDC_CHK_MOD_ALT, (g_hotkeyModifiers & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED);

        HWND hChkShift = CreateWindowW(L"BUTTON", L"Shift",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            275, 173, 50, 20, hDlg, (HMENU)(INT_PTR)IDC_CHK_MOD_SHIFT, hInst, nullptr);
        SendMessage(hChkShift, WM_SETFONT, (WPARAM)hFont, TRUE);
        CheckDlgButton(hDlg, IDC_CHK_MOD_SHIFT, (g_hotkeyModifiers & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED);

        // --- Run at startup checkbox ---
        HWND hChkStartup = CreateWindowW(L"BUTTON", L"Run at Windows startup",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            10, 205, 160, 20, hDlg, (HMENU)(INT_PTR)IDC_CHK_STARTUP, hInst, nullptr);
        SendMessage(hChkStartup, WM_SETFONT, (WPARAM)hFont, TRUE);
        CheckDlgButton(hDlg, IDC_CHK_STARTUP, IsStartupEnabled() ? BST_CHECKED : BST_UNCHECKED);

        // --- OK button ---
        HWND hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            240, 235, 80, 26, hDlg, (HMENU)(INT_PTR)IDC_BTN_OK, hInst, nullptr);
        SendMessage(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);

        // --- Cancel button ---
        HWND hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            324, 235, 80, 26, hDlg, (HMENU)(INT_PTR)IDC_BTN_CANCEL, hInst, nullptr);
        SendMessage(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Resize dialog to fit controls
        SetWindowPos(hDlg, nullptr, 0, 0, 420, 310,
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

        case IDC_BTN_SOUND_BROWSE:
        {
            // Open file picker for sound file
            WCHAR soundPath[MAX_PATH] = {};
            GetDlgItemTextW(hDlg, IDC_EDIT_SOUND, soundPath, MAX_PATH);

            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hDlg;
            ofn.lpstrFilter = L"Wave Files (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = soundPath;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Select Sound File";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

            if (GetOpenFileNameW(&ofn))
            {
                SetDlgItemTextW(hDlg, IDC_EDIT_SOUND, soundPath);
            }
        }
        break;

        case IDC_BTN_EDITOR_BROWSE:
        {
            WCHAR editorPath[MAX_PATH] = {};
            GetDlgItemTextW(hDlg, IDC_EDIT_EDITOR, editorPath, MAX_PATH);

            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hDlg;
            ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = editorPath;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Select Image Editor";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

            if (GetOpenFileNameW(&ofn))
            {
                SetDlgItemTextW(hDlg, IDC_EDIT_EDITOR, editorPath);
            }
        }
        break;

        case IDC_BTN_OK:
        {
            // Read folder path
            GetDlgItemTextW(hDlg, IDC_EDIT_FOLDER, g_saveFolderPath, MAX_PATH);

            // Read format
            g_imageFormat = IsDlgButtonChecked(hDlg, IDC_RADIO_JPG) ? FMT_JPG : FMT_PNG;

            // Read sound settings
            g_soundEnabled = IsDlgButtonChecked(hDlg, IDC_CHK_SOUND) == BST_CHECKED;
            GetDlgItemTextW(hDlg, IDC_EDIT_SOUND, g_soundFilePath, MAX_PATH);

            // Read default color
            int sel = (int)SendDlgItemMessageW(hDlg, IDC_COMBO_COLOR, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_colorCount)
                g_defaultColorIndex = sel;

            // Read default pen size
            WCHAR sizeBuf[8] = {};
            GetDlgItemTextW(hDlg, IDC_EDIT_SIZE, sizeBuf, 8);
            int sz = _wtoi(sizeBuf);
            if (sz >= PEN_THICKNESS_MIN && sz <= PEN_THICKNESS_MAX)
                g_defaultPenThickness = sz;

            // Read editor settings
            g_openEditorEnabled = IsDlgButtonChecked(hDlg, IDC_CHK_EDITOR) == BST_CHECKED;
            GetDlgItemTextW(hDlg, IDC_EDIT_EDITOR, g_editorPath, MAX_PATH);

            // Read hotkey settings
            {
                int keySel = (int)SendDlgItemMessageW(hDlg, IDC_COMBO_HOTKEY, CB_GETCURSEL, 0, 0);
                if (keySel >= 0 && keySel < g_availableKeyCount)
                    g_hotkeyVk = g_availableKeys[keySel].vk;

                UINT mod = 0;
                if (IsDlgButtonChecked(hDlg, IDC_CHK_MOD_CTRL) == BST_CHECKED)
                    mod |= MOD_CONTROL;
                if (IsDlgButtonChecked(hDlg, IDC_CHK_MOD_ALT) == BST_CHECKED)
                    mod |= MOD_ALT;
                if (IsDlgButtonChecked(hDlg, IDC_CHK_MOD_SHIFT) == BST_CHECKED)
                    mod |= MOD_SHIFT;
                g_hotkeyModifiers = mod;

                RegisterScreenshotHotkey();
            }

            // Apply startup setting
            SetStartupEnabled(IsDlgButtonChecked(hDlg, IDC_CHK_STARTUP) == BST_CHECKED);

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
// Startup registry helpers (Run key)
// ============================================================================
static const WCHAR* STARTUP_REG_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const WCHAR* STARTUP_VALUE_NAME = L"SketchSnap";

bool IsStartupEnabled()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    WCHAR path[MAX_PATH] = {};
    DWORD cbPath = sizeof(path);
    bool exists = (RegQueryValueExW(hKey, STARTUP_VALUE_NAME, nullptr, nullptr,
        (LPBYTE)path, &cbPath) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

void SetStartupEnabled(bool enable)
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REG_KEY, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return;

    if (enable)
    {
        WCHAR exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        RegSetValueExW(hKey, STARTUP_VALUE_NAME, 0, REG_SZ,
            (const BYTE*)exePath,
            (DWORD)((wcslen(exePath) + 1) * sizeof(WCHAR)));
    }
    else
    {
        RegDeleteValueW(hKey, STARTUP_VALUE_NAME);
    }
    RegCloseKey(hKey);
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

void PlayNotificationSound()
{
    if (g_soundEnabled && g_soundFilePath[0] != L'\0')
    {
        // Play the notification sound using the Windows multimedia API
        PlaySoundW(g_soundFilePath, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    }
}

// ============================================================================
// Open the saved screenshot in the configured image editor
// ============================================================================
void OpenInEditor(const WCHAR* filePath)
{
    if (g_openEditorEnabled && g_editorPath[0] != L'\0' && filePath && filePath[0] != L'\0')
    {
        ShellExecuteW(nullptr, L"open", g_editorPath, filePath, nullptr, SW_SHOWNORMAL);
    }
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
// Hotkey registration helpers
// ============================================================================
void UnregisterScreenshotHotkey()
{
    if (g_hWnd)
        UnregisterHotKey(g_hWnd, ID_HOTKEY_PRINTSCR);
}

void RegisterScreenshotHotkey()
{
    if (g_hWnd)
    {
        UnregisterHotKey(g_hWnd, ID_HOTKEY_PRINTSCR);
        RegisterHotKey(g_hWnd, ID_HOTKEY_PRINTSCR, g_hotkeyModifiers, g_hotkeyVk);
    }
}

// ============================================================================
// Low-level keyboard hook
// ============================================================================
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN)
    {
        KBDLLHOOKSTRUCT* pKey = (KBDLLHOOKSTRUCT*)lParam;
        if (pKey->vkCode == g_hotkeyVk)
        {
            bool ctrlRequired  = (g_hotkeyModifiers & MOD_CONTROL) != 0;
            bool altRequired   = (g_hotkeyModifiers & MOD_ALT) != 0;
            bool shiftRequired = (g_hotkeyModifiers & MOD_SHIFT) != 0;

            bool ctrlHeld  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool altHeld   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            if (ctrlHeld == ctrlRequired && altHeld == altRequired && shiftHeld == shiftRequired)
            {
                PostMessageW(g_hWnd, WM_TRIGGER_OVERLAY, 0, 0);
            }
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

    RegisterScreenshotHotkey();

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

    PlayNotificationSound();

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
    g_texts.clear();
    g_undoHistory.clear();
    g_redoHistory.clear();
    g_isDrawingFreehand = false;
    g_isDrawingRect = false;
    g_isCropping = false;
    g_isTypingText = false;
    g_textLen = 0;
    g_textBuffer[0] = L'\0';
    g_currentRect = {};
    g_cropRect = {};
    g_currentColorIndex = g_defaultColorIndex;
    g_penThickness = g_defaultPenThickness;

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
// Commit the current text block as a text annotation
// ============================================================================
void CommitTextBlock(HWND hWnd)
{
    if (g_isTypingText && g_textLen > 0)
    {
        DrawTextAnnotation ta = {};
        ta.pos = g_textPos;
        StringCchCopyW(ta.text, 256, g_textBuffer);
        ta.color = g_colorPalette[g_currentColorIndex];
        ta.fontSize = g_penThickness * 4 + 8;
        g_texts.push_back(ta);

        UndoEntry entry = {};
        entry.type = ACTION_TEXT;
        entry.startIndex = g_texts.size() - 1;
        entry.count = 1;
        g_undoHistory.push_back(entry);
        g_redoHistory.clear();
    }
    g_isTypingText = false;
    g_textLen = 0;
    g_textBuffer[0] = L'\0';
    if (hWnd) InvalidateRect(hWnd, nullptr, FALSE);
}

// ============================================================================
// Paint the overlay
// ============================================================================
void PaintOverlay(HWND hWnd, HDC hdc)
{
    // --- Double-buffer: create offscreen DC and bitmap ---
    HDC hBufferDC = CreateCompatibleDC(hdc);
    HBITMAP hBufferBmp = CreateCompatibleBitmap(hdc, g_screenW, g_screenH);
    HBITMAP hBufferOld = (HBITMAP)SelectObject(hBufferDC, hBufferBmp);

    // Draw screenshot into the buffer
    HDC hMemDC = CreateCompatibleDC(hBufferDC);
    HBITMAP hOld = (HBITMAP)SelectObject(hMemDC, g_hScreenBitmap);
    BitBlt(hBufferDC, 0, 0, g_screenW, g_screenH, hMemDC, 0, 0, SRCCOPY);
    SelectObject(hMemDC, hOld);
    DeleteDC(hMemDC);

    // Blueish tint
    {
        HDC hDimDC = CreateCompatibleDC(hBufferDC);
        HBITMAP hDimBmp = CreateCompatibleBitmap(hBufferDC, g_screenW, g_screenH);
        HBITMAP hDimOld = (HBITMAP)SelectObject(hDimDC, hDimBmp);

        RECT rcFull = { 0, 0, g_screenW, g_screenH };
        HBRUSH hDimBrush = CreateSolidBrush(RGB(0, 30, 80));
        FillRect(hDimDC, &rcFull, hDimBrush);
        DeleteObject(hDimBrush);

        BLENDFUNCTION bf = {};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 60;
        AlphaBlend(hBufferDC, 0, 0, g_screenW, g_screenH, hDimDC, 0, 0, g_screenW, g_screenH, bf);

        SelectObject(hDimDC, hDimOld);
        DeleteObject(hDimBmp);
        DeleteDC(hDimDC);
    }

    // Freehand lines (each with its own color and thickness)
    for (const auto& line : g_lines)
    {
        HPEN hPen = CreatePen(PS_SOLID, line.thickness, line.color);
        HPEN hOldPen = (HPEN)SelectObject(hBufferDC, hPen);
        MoveToEx(hBufferDC, line.pt1.x, line.pt1.y, nullptr);
        LineTo(hBufferDC, line.pt2.x, line.pt2.y);
        SelectObject(hBufferDC, hOldPen);
        DeleteObject(hPen);
    }

    // Rectangles (each with its own color and thickness)
    HBRUSH hNullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hBufferDC, hNullBrush);

    for (const auto& r : g_rects)
    {
        HPEN hPen = CreatePen(PS_SOLID, r.thickness, r.color);
        HPEN hOldPen = (HPEN)SelectObject(hBufferDC, hPen);
        Rectangle(hBufferDC, r.rc.left, r.rc.top, r.rc.right, r.rc.bottom);
        SelectObject(hBufferDC, hOldPen);
        DeleteObject(hPen);
    }

    if (g_isDrawingRect)
    {
        HPEN hDashPen = CreatePen(PS_DASH, g_penThickness, g_colorPalette[g_currentColorIndex]);
        HPEN hOldPen = (HPEN)SelectObject(hBufferDC, hDashPen);
        Rectangle(hBufferDC, g_currentRect.left, g_currentRect.top,
            g_currentRect.right, g_currentRect.bottom);
        SelectObject(hBufferDC, hOldPen);
        DeleteObject(hDashPen);
    }

    if (g_isCropping)
    {
        HPEN hCropPen = CreatePen(PS_DASH, 2, RGB(0, 200, 255));
        HPEN hOldPen = (HPEN)SelectObject(hBufferDC, hCropPen);
        Rectangle(hBufferDC, g_cropRect.left, g_cropRect.top,
            g_cropRect.right, g_cropRect.bottom);
        SelectObject(hBufferDC, hOldPen);
        DeleteObject(hCropPen);
    }

    SelectObject(hBufferDC, hOldBrush);

    // Text annotations
    SetBkMode(hBufferDC, TRANSPARENT);
    for (const auto& ta : g_texts)
    {
        SetTextColor(hBufferDC, ta.color);
        HFONT hFont = CreateFontW(ta.fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hBufferDC, hFont);
        RECT rcText = { ta.pos.x, ta.pos.y, g_screenW, g_screenH };
        DrawTextW(hBufferDC, ta.text, -1, &rcText, DT_NOCLIP | DT_SINGLELINE);
        SelectObject(hBufferDC, hOldFont);
        DeleteObject(hFont);
    }

    // In-progress text being typed
    if (g_isTypingText && g_textLen > 0)
    {
        int fontSize = g_penThickness * 4 + 8;
        COLORREF curColor = g_colorPalette[g_currentColorIndex];
        SetTextColor(hBufferDC, curColor);
        HFONT hFont = CreateFontW(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hBufferDC, hFont);

        // Draw the text with a cursor bar
        WCHAR displayBuf[260] = {};
        StringCchCopyW(displayBuf, 258, g_textBuffer);
        StringCchCatW(displayBuf, 260, L"|");

        RECT rcText = { g_textPos.x, g_textPos.y, g_screenW, g_screenH };
        DrawTextW(hBufferDC, displayBuf, -1, &rcText, DT_NOCLIP | DT_SINGLELINE);
        SelectObject(hBufferDC, hOldFont);
        DeleteObject(hFont);
    }

    // Help text
    SetBkMode(hBufferDC, TRANSPARENT);
    SetTextColor(hBufferDC, RGB(255, 255, 255));

    const WCHAR* helpText =
        L"RMB: Draw  |  Ctrl+RMB: Rect  |  Type: Text  |  LMB: Save  |  Ctrl+LMB: Crop  |  "
        L"Ctrl+Z/Y: Undo/Redo  |  Ctrl+Wheel: Size  |  Shift+Wheel: Color  |  Esc: Cancel";

    RECT rcText = { 0, 10, g_screenW, 40 };
    DrawTextW(hBufferDC, helpText, -1, &rcText, DT_CENTER | DT_SINGLELINE);

    // --- Cursor color/thickness indicator ---
    {
        COLORREF curColor = g_colorPalette[g_currentColorIndex];
        int cx = g_lastMousePos.x;
        int cy = g_lastMousePos.y;

        // Draw a filled circle at cursor position showing current color and size
        int radius = max(g_penThickness / 2, 2);
        HBRUSH hFillBrush = CreateSolidBrush(curColor);
        HPEN hOutlinePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HPEN hPrevPen = (HPEN)SelectObject(hBufferDC, hOutlinePen);
        HBRUSH hPrevBrush = (HBRUSH)SelectObject(hBufferDC, hFillBrush);
        Ellipse(hBufferDC, cx - radius, cy - radius, cx + radius, cy + radius);
        SelectObject(hBufferDC, hPrevBrush);
        SelectObject(hBufferDC, hPrevPen);
        DeleteObject(hFillBrush);
        DeleteObject(hOutlinePen);

        // Draw color name + thickness label near cursor
        WCHAR label[64] = {};
        StringCchPrintfW(label, 64, L"%s  [%d]", g_colorNames[g_currentColorIndex], g_penThickness);
        HFONT hLabelFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT hPrevFont = (HFONT)SelectObject(hBufferDC, hLabelFont);
        SetTextColor(hBufferDC, curColor);
        RECT rcLabel = { cx + radius + 6, cy - 8, cx + radius + 160, cy + 12 };
        DrawTextW(hBufferDC, label, -1, &rcLabel, DT_LEFT | DT_SINGLELINE | DT_NOCLIP);
        SelectObject(hBufferDC, hPrevFont);
        DeleteObject(hLabelFont);
    }

    // --- Single blit from buffer to screen ---
    BitBlt(hdc, 0, 0, g_screenW, g_screenH, hBufferDC, 0, 0, SRCCOPY);

    SelectObject(hBufferDC, hBufferOld);
    DeleteObject(hBufferBmp);
    DeleteDC(hBufferDC);
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

    // Text annotations
    SetBkMode(hCompDC, TRANSPARENT);
    for (const auto& ta : g_texts)
    {
        // Draw text using GDI
        SetTextColor(hCompDC, ta.color);
        HFONT hFont = CreateFontW(ta.fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hCompDC, hFont);
        RECT rcText = { ta.pos.x, ta.pos.y, g_screenW, g_screenH };
        DrawTextW(hCompDC, ta.text, -1, &rcText, DT_NOCLIP | DT_SINGLELINE);
        SelectObject(hCompDC, hOldFont);
        DeleteObject(hFont);
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

    // Play sound on screenshot save
    PlayNotificationSound();

    // Open in image editor if enabled
    OpenInEditor(filePath);
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
        CommitTextBlock(hWnd);
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
            g_freehandStrokeStart = g_lines.size();
        }
    }
    break;

    case WM_MOUSEMOVE:
    {
        POINT pt = { (SHORT)LOWORD(lParam), (SHORT)HIWORD(lParam) };

        // If typing text, commit it when the mouse moves
        if (g_isTypingText)
        {
            int dx = pt.x - g_lastMousePos.x;
            int dy = pt.y - g_lastMousePos.y;
            if (dx * dx + dy * dy > 16)
            {
                CommitTextBlock(hWnd);
            }
        }

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

            UndoEntry entry = {};
            entry.type = ACTION_RECT;
            entry.startIndex = g_rects.size() - 1;
            entry.count = 1;
            g_undoHistory.push_back(entry);
            g_redoHistory.clear();

            InvalidateRect(hWnd, nullptr, FALSE);
        }

        if (g_isDrawingFreehand)
        {
            size_t strokeCount = g_lines.size() - g_freehandStrokeStart;
            if (strokeCount > 0)
            {
                UndoEntry entry = {};
                entry.type = ACTION_FREEHAND;
                entry.startIndex = g_freehandStrokeStart;
                entry.count = strokeCount;
                g_undoHistory.push_back(entry);
                g_redoHistory.clear();
            }
            g_isDrawingFreehand = false;
        }
    }
    break;

    case WM_LBUTTONDOWN:
    {
        CommitTextBlock(hWnd);
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
            CommitTextBlock(hWnd);
            SaveAnnotatedScreenshot(&g_cropRect);
            CloseOverlay();
        }
        else if (!g_isCropping)
        {
            CommitTextBlock(hWnd);
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
            if (zDelta > 0)
                g_penThickness = min(g_penThickness + 1, PEN_THICKNESS_MAX);
            else
                g_penThickness = max(g_penThickness - 1, PEN_THICKNESS_MIN);
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        else if (shiftHeld)
        {
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

    case WM_CHAR:
    {
        WCHAR ch = (WCHAR)wParam;
        // Ignore control characters (below space), allow printable chars and space
        if (ch < L' ')
            break;

        // Start typing if not already
        if (!g_isTypingText)
        {
            g_isTypingText = true;
            g_textPos = g_lastMousePos;
            g_textLen = 0;
            g_textBuffer[0] = L'\0';
        }

        // Append character
        if (g_textLen < 255)
        {
            g_textBuffer[g_textLen++] = ch;
            g_textBuffer[g_textLen] = L'\0';
            InvalidateRect(hWnd, nullptr, FALSE);
        }
    }
    break;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            if (g_isTypingText)
            {
                // Cancel current text without committing
                g_isTypingText = false;
                g_textLen = 0;
                g_textBuffer[0] = L'\0';
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            else
            {
                CloseOverlay();
            }
        }
        else if (wParam == VK_RETURN && g_isTypingText)
        {
            CommitTextBlock(hWnd);
        }
        else if (wParam == VK_BACK && g_isTypingText)
        {
            if (g_textLen > 0)
            {
                g_textBuffer[--g_textLen] = L'\0';
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        else if (wParam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            CommitTextBlock(hWnd);
            if (!g_undoHistory.empty())
            {
                UndoEntry last = g_undoHistory.back();
                g_undoHistory.pop_back();

                RedoEntry redo = {};
                redo.undoInfo = last;

                if (last.type == ACTION_FREEHAND)
                {
                    if (last.startIndex < g_lines.size())
                    {
                        redo.lines.assign(g_lines.begin() + last.startIndex, g_lines.end());
                        g_lines.erase(g_lines.begin() + last.startIndex, g_lines.end());
                    }
                }
                else if (last.type == ACTION_RECT)
                {
                    if (last.startIndex < g_rects.size())
                    {
                        redo.rects.assign(g_rects.begin() + last.startIndex, g_rects.end());
                        g_rects.erase(g_rects.begin() + last.startIndex, g_rects.end());
                    }
                }
                else if (last.type == ACTION_TEXT)
                {
                    if (last.startIndex < g_texts.size())
                    {
                        redo.texts.assign(g_texts.begin() + last.startIndex, g_texts.end());
                        g_texts.erase(g_texts.begin() + last.startIndex, g_texts.end());
                    }
                }

                g_redoHistory.push_back(std::move(redo));
                InvalidateRect(hWnd, nullptr, FALSE);
            }
        }
        else if (wParam == 'Y' && (GetKeyState(VK_CONTROL) & 0x8000))
        {
            CommitTextBlock(hWnd);
            if (!g_redoHistory.empty())
            {
                RedoEntry redo = std::move(g_redoHistory.back());
                g_redoHistory.pop_back();

                if (redo.undoInfo.type == ACTION_FREEHAND)
                {
                    redo.undoInfo.startIndex = g_lines.size();
                    redo.undoInfo.count = redo.lines.size();
                    g_lines.insert(g_lines.end(), redo.lines.begin(), redo.lines.end());
                }
                else if (redo.undoInfo.type == ACTION_RECT)
                {
                    redo.undoInfo.startIndex = g_rects.size();
                    redo.undoInfo.count = redo.rects.size();
                    g_rects.insert(g_rects.end(), redo.rects.begin(), redo.rects.end());
                }
                else if (redo.undoInfo.type == ACTION_TEXT)
                {
                    redo.undoInfo.startIndex = g_texts.size();
                    redo.undoInfo.count = redo.texts.size();
                    g_texts.insert(g_texts.end(), redo.texts.begin(), redo.texts.end());
                }

                g_undoHistory.push_back(redo.undoInfo);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
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
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y,  0, hWnd, nullptr);
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
        UnregisterScreenshotHotkey();
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
