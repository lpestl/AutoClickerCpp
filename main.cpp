// ============================================================================
//  AutoClickerCpp (WinAPI)
//  - Displays the exact cursor coordinates (GetCursorPos) in real time
//  - SPACE toggles the mouse click simulation (WH_KEYBOARD_LL hook)
//  - Modes: fixed / random / precision, configured via config.ini
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// ---------------------------------------------------------------------------
// Auto-clicker configuration (read from config.ini next to the executable)
// ---------------------------------------------------------------------------
enum class ClickMode
{
    Fixed,
    Random,
    Precision
};

struct ClickConfig
{
    ClickMode mode       = ClickMode::Fixed;
    int  fixedInterval   = 100;   // fixed_interval, ms
    int  intervalMin     = 50;    // interval_min, ms
    int  intervalMax     = 200;   // interval_max, ms
    int  pressDelayMin   = 10;    // down_delay_min, ms
    int  pressDelayMax   = 50;    // down_delay_max, ms

    // precision mode: rectangular area for clicks (screen coordinates)
    int  precisionMinX   = 0;
    int  precisionMinY   = 0;
    int  precisionMaxX   = 800;
    int  precisionMaxY   = 600;

    bool showDebugWindow = true;  // show the application window
};

static ClickConfig g_config;

// ---------------------------------------------------------------------------
// Global application state
// ---------------------------------------------------------------------------
static POINT    g_mousePos         = { 0, 0 }; // current cursor coordinates

static bool     g_autoClickEnabled = false;    // whether auto-click is enabled
static long     g_clickCount       = 0;        // how many clicks have been sent

static HHOOK    g_keyboardHook     = NULL;     // keyboard hook handle
static HWND     g_hwnd             = NULL;     // application window

static const UINT_PTR TIMER_ID       = 1;      // coordinate timer
static const UINT     TIMER_MS       = 15;     // coordinate polling period (ms)
static const UINT_PTR TIMER_CLICK_ID = 2;      // click timer


// ---------------------------------------------------------------------------
// config.ini parsing
// ---------------------------------------------------------------------------
static std::string Trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

static std::string ToLower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    return s;
}

// Path to config.ini — in the folder next to the exe
static std::filesystem::path GetConfigPath()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path() / L"config.ini";
}

// Reads config.ini; on missing file/errors the defaults remain
static void LoadConfig()
{
    std::ifstream file(GetConfigPath());
    if (!file.is_open())
        return;

    std::string line;
    while (std::getline(file, line))
    {
        // strip the comment (; or #)
        size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos)
            line = line.substr(0, comment);

        // find the '=' separator
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = ToLower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));

        if (key == "mode")
        {
            std::string m = ToLower(val);
            if (m == "random")        g_config.mode = ClickMode::Random;
            else if (m == "precision") g_config.mode = ClickMode::Precision;
            else                      g_config.mode = ClickMode::Fixed;
        }
        else if (key == "fixed_interval")
        {
            int v = atoi(val.c_str());
            if (v > 0) g_config.fixedInterval = v;
        }
        else if (key == "interval_min")
        {
            int v = atoi(val.c_str());
            if (v > 0) g_config.intervalMin = v;
        }
        else if (key == "interval_max")
        {
            int v = atoi(val.c_str());
            if (v > 0) g_config.intervalMax = v;
        }
        else if (key == "down_delay_min")
        {
            int v = atoi(val.c_str());
            if (v >= 0) g_config.pressDelayMin = v; // 0 is also allowed (no delay)
        }
        else if (key == "down_delay_max")
        {
            int v = atoi(val.c_str());
            if (v >= 0) g_config.pressDelayMax = v;
        }
        else if (key == "precision_min_x")
            g_config.precisionMinX = atoi(val.c_str());
        else if (key == "precision_min_y")
            g_config.precisionMinY = atoi(val.c_str());
        else if (key == "precision_max_x")
            g_config.precisionMaxX = atoi(val.c_str());
        else if (key == "precision_max_y")
            g_config.precisionMaxY = atoi(val.c_str());
        else if (key == "show_debug_window")
        {
            std::string v = ToLower(val);
            g_config.showDebugWindow = (v == "true" || v == "1" ||
                                        v == "yes" || v == "on");
        }
    }

    // Ensure correct min <= max ranges
    if (g_config.intervalMin > g_config.intervalMax)
    {
        int tmp = g_config.intervalMin;
        g_config.intervalMin = g_config.intervalMax;
        g_config.intervalMax = tmp;
    }
    if (g_config.pressDelayMin > g_config.pressDelayMax)
    {
        int tmp = g_config.pressDelayMin;
        g_config.pressDelayMin = g_config.pressDelayMax;
        g_config.pressDelayMax = tmp;
    }
    if (g_config.precisionMinX > g_config.precisionMaxX)
    {
        int tmp = g_config.precisionMinX;
        g_config.precisionMinX = g_config.precisionMaxX;
        g_config.precisionMaxX = tmp;
    }
    if (g_config.precisionMinY > g_config.precisionMaxY)
    {
        int tmp = g_config.precisionMinY;
        g_config.precisionMinY = g_config.precisionMaxY;
        g_config.precisionMaxY = tmp;
    }
}

// Returns the next interval between clicks (ms): either fixed or
// a random value in the [min, max] range.
static int GetNextInterval()
{
    if (g_config.mode == ClickMode::Random)
    {
        int range = g_config.intervalMax - g_config.intervalMin + 1;
        return g_config.intervalMin + (rand() % range);
    }
    // Fixed and Precision use a fixed interval
    return g_config.fixedInterval;
}

// Returns a random delay between press and release (ms)
// in the [pressDelayMin, pressDelayMax] range.
static int GetNextPressDelay()
{
    int range = g_config.pressDelayMax - g_config.pressDelayMin + 1;
    return g_config.pressDelayMin + (rand() % range);
}


// ---------------------------------------------------------------------------
// Low-level keyboard hook: detects SPACE presses (VK_SPACE).
// Works globally, even when the window is not focused.
// ---------------------------------------------------------------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* p =
            reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

        // React only to "key down", ignoring key up
        if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
            p->vkCode == VK_SPACE)
        {
            // Space toggles the mouse click simulation: on <-> off
            g_autoClickEnabled = !g_autoClickEnabled;

            if (g_autoClickEnabled)
            {
                // Start the click timer with the first interval
                SetTimer(g_hwnd, TIMER_CLICK_ID, GetNextInterval(), NULL);
            }
            else
            {
                // Stop the click timer
                KillTimer(g_hwnd, TIMER_CLICK_ID);
            }
        }
    }

    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}


// ---------------------------------------------------------------------------
// Update the cursor coordinates
// ---------------------------------------------------------------------------
static void UpdateMousePosition()
{
    GetCursorPos(&g_mousePos);
}


// ---------------------------------------------------------------------------
// Simulate a left mouse button click at the current cursor position:
// sends MOUSE_BUTTON_DOWN (LEFTDOWN) and MOUSE_BUTTON_UP (LEFTUP)
// events via SendInput.
// ---------------------------------------------------------------------------
static void SimulateMouseClick()
{
    // In precision mode, click at a random point of the given rectangle
    if (g_config.mode == ClickMode::Precision)
    {
        int rangeX = g_config.precisionMaxX - g_config.precisionMinX + 1;
        int rangeY = g_config.precisionMaxY - g_config.precisionMinY + 1;
        int x = g_config.precisionMinX + (rand() % rangeX);
        int y = g_config.precisionMinY + (rand() % rangeY);
        SetCursorPos(x, y); // move the cursor to a point inside the area
    }

    INPUT input = {};
    input.type = INPUT_MOUSE;

    // MOUSE_BUTTON_DOWN — button pressed
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));

    // Random delay between press and release (like a real click)
    int pressDelay = GetNextPressDelay();
    if (pressDelay > 0)
        Sleep(pressDelay);

    // MOUSE_BUTTON_UP — button released
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));

    InterlockedIncrement(&g_clickCount);
}


// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
        {
            g_hwnd = hwnd;

            // Install the low-level keyboard hook (global)
            g_keyboardHook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                LowLevelKeyboardProc,
                GetModuleHandleW(NULL),
                0
            );

            if (!g_keyboardHook)
            {
                MessageBoxW(hwnd, L"Failed to install the keyboard hook.",
                            L"Error", MB_ICONERROR);
            }

            // Start the timer for periodic coordinate polling
            SetTimer(hwnd, TIMER_ID, TIMER_MS, NULL);
            UpdateMousePosition();
            return 0;
        }

        case WM_TIMER:
        {
            if (wParam == TIMER_ID)
            {
                UpdateMousePosition();
                InvalidateRect(hwnd, NULL, FALSE); // request a redraw
            }
            else if (wParam == TIMER_CLICK_ID)
            {
                // If auto-click is enabled — send a click and reschedule the timer
                if (g_autoClickEnabled)
                {
                    SimulateMouseClick();
                    SetTimer(hwnd, TIMER_CLICK_ID, GetNextInterval(), NULL);
                }
            }
            return 0;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            // Dark background
            HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
            FillRect(hdc, &ps.rcPaint, bg);
            DeleteObject(bg);

            // Text color and font
            SetTextColor(hdc, RGB(0, 255, 120));
            SetBkMode(hdc, TRANSPARENT);

            HFONT hFont = CreateFontW(
                26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas"
            );
            HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

            int y = 20;
            const int lineHeight = 36;

            wchar_t title[128];
            wsprintfW(title, L"..::-AutoClicker-::..");
            TextOutW(hdc, 20, y, title, lstrlenW(title));
            y += lineHeight + 10;

            wchar_t coords[128];
            wsprintfW(coords, L"Mouse coordinates:  X = %ld   Y = %ld",
                      g_mousePos.x, g_mousePos.y);
            TextOutW(hdc, 20, y, coords, lstrlenW(coords));
            y += lineHeight;

            // Auto-click state
            const wchar_t* autoOn  = L"Auto-click (Space key):  ON";
            const wchar_t* autoOff = L"Auto-click (Space key):  OFF";
            if (g_autoClickEnabled)
            {
                SetTextColor(hdc, RGB(255, 90, 90));
                TextOutW(hdc, 20, y, autoOn, lstrlenW(autoOn));
                SetTextColor(hdc, RGB(0, 255, 120));
            }
            else
            {
                TextOutW(hdc, 20, y, autoOff, lstrlenW(autoOff));
            }
            y += lineHeight;

            // Current mode
            wchar_t modeText[128];
            if (g_config.mode == ClickMode::Random)
                wsprintfW(modeText, L"Mode:  random");
            else if (g_config.mode == ClickMode::Precision)
                wsprintfW(modeText, L"Mode:  precision");
            else
                wsprintfW(modeText, L"Mode:  fixed");
            TextOutW(hdc, 20, y, modeText, lstrlenW(modeText));
            y += lineHeight;

            // Current mode and interval
            wchar_t intervalText[128];
            if (g_config.mode == ClickMode::Random)
            {
                wsprintfW(intervalText, L"Interval:  %d-%d ms (random)",
                          g_config.intervalMin, g_config.intervalMax);
            }
            else
            {
                wsprintfW(intervalText, L"Interval:  %d ms (fixed)",
                          g_config.fixedInterval);
            }
            TextOutW(hdc, 20, y, intervalText, lstrlenW(intervalText));
            y += lineHeight;

            // In precision mode, show the click area
            if (g_config.mode == ClickMode::Precision)
            {
                wchar_t rectText[160];
                wsprintfW(rectText, L"Area:  X %d-%d   Y %d-%d",
                          g_config.precisionMinX, g_config.precisionMaxX,
                          g_config.precisionMinY, g_config.precisionMaxY);
                TextOutW(hdc, 20, y, rectText, lstrlenW(rectText));
                y += lineHeight;
            }

            wchar_t clicks[128];
            wsprintfW(clicks, L"Clicks sent: %ld", g_clickCount);
            TextOutW(hdc, 20, y, clicks, lstrlenW(clicks));
            y += lineHeight;

            SelectObject(hdc, hOldFont);
            DeleteObject(hFont);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
        {
            KillTimer(hwnd, TIMER_ID);
            KillTimer(hwnd, TIMER_CLICK_ID);

            if (g_keyboardHook)
            {
                UnhookWindowsHookEx(g_keyboardHook);
                g_keyboardHook = NULL;
            }

            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}


// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    // Read the settings from config.ini and seed the PRNG
    LoadConfig();
    srand((unsigned)GetTickCount());

    const wchar_t CLASS_NAME[] = L"AutoClickerCppWindow";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"AutoClickerCpp",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 680, 400,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd)
    {
        return 0;
    }

    // Show the window only if the show_debug_window flag is enabled.
    // The message loop is still needed — the keyboard hook runs on it.
    if (g_config.showDebugWindow)
    {
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
    }

    // Message loop (also required for the keyboard hook to work)
    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
