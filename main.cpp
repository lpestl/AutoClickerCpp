// ============================================================================
//  Auto Clicker (WinAPI)
//  - Показывает точные координаты курсора (GetCursorPos) в реальном времени
//  - ПРОБЕЛ включает/выключает имитацию кликов мыши (хук WH_KEYBOARD_LL)
//  - Интервал кликов настраивается в config.ini (фикс. или случайный min-max)
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
// Конфигурация автокликера (читается из config.ini рядом с исполняемым файлом)
// ---------------------------------------------------------------------------
struct ClickConfig
{
    bool useRandom     = false; // случайный интервал (true) или фиксированный
    int  fixedInterval = 100;   // фиксированный интервал, мс
    int  minInterval   = 50;    // минимальный интервал (случайный режим), мс
    int  maxInterval   = 200;   // максимальный интервал (случайный режим), мс
    int  pressDelayMin = 10;    // минимальная задержка между DOWN и UP, мс
    int  pressDelayMax = 50;    // максимальная задержка между DOWN и UP, мс
};

static ClickConfig g_config;

// ---------------------------------------------------------------------------
// Глобальное состояние приложения
// ---------------------------------------------------------------------------
static POINT    g_mousePos         = { 0, 0 }; // текущие координаты курсора

static bool     g_autoClickEnabled = false;    // включён ли автоклик
static long     g_clickCount       = 0;        // сколько кликов отправлено

static HHOOK    g_keyboardHook     = NULL;     // дескриптор хука клавиатуры
static HWND     g_hwnd             = NULL;     // окно приложения

static const UINT_PTR TIMER_ID       = 1;      // таймер координат
static const UINT     TIMER_MS       = 15;     // период опроса координат (мс)
static const UINT_PTR TIMER_CLICK_ID = 2;      // таймер кликов


// ---------------------------------------------------------------------------
// Парсинг config.ini
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

// Путь к config.ini — в папке рядом с exe
static std::filesystem::path GetConfigPath()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path() / L"config.ini";
}

// Читает config.ini; при его отсутствии/ошибках остаются значения по умолчанию
static void LoadConfig()
{
    std::ifstream file(GetConfigPath());
    if (!file.is_open())
        return;

    std::string line;
    while (std::getline(file, line))
    {
        // отрезаем комментарий (; или #)
        size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos)
            line = line.substr(0, comment);

        // ищем разделитель '='
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = ToLower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));

        if (key == "mode")
            g_config.useRandom = (ToLower(val) == "random");
        else if (key == "interval")
        {
            int v = atoi(val.c_str());
            if (v > 0) g_config.fixedInterval = v;
        }
        else if (key == "min")
        {
            int v = atoi(val.c_str());
            if (v > 0) g_config.minInterval = v;
        }
        else if (key == "max")
        {
            int v = atoi(val.c_str());
            if (v > 0) g_config.maxInterval = v;
        }
        else if (key == "down_delay_min")
        {
            int v = atoi(val.c_str());
            if (v >= 0) g_config.pressDelayMin = v; // 0 тоже допустим (без задержки)
        }
        else if (key == "down_delay_max")
        {
            int v = atoi(val.c_str());
            if (v >= 0) g_config.pressDelayMax = v;
        }
    }

    // Гарантируем корректные диапазоны min <= max
    if (g_config.minInterval > g_config.maxInterval)
    {
        int tmp = g_config.minInterval;
        g_config.minInterval = g_config.maxInterval;
        g_config.maxInterval = tmp;
    }
    if (g_config.pressDelayMin > g_config.pressDelayMax)
    {
        int tmp = g_config.pressDelayMin;
        g_config.pressDelayMin = g_config.pressDelayMax;
        g_config.pressDelayMax = tmp;
    }
}

// Возвращает следующий интервал между кликами (мс): фиксированный либо
// случайное значение в диапазоне [min, max].
static int GetNextInterval()
{
    if (g_config.useRandom)
    {
        int range = g_config.maxInterval - g_config.minInterval + 1;
        return g_config.minInterval + (rand() % range);
    }
    return g_config.fixedInterval;
}

// Возвращает случайную задержку между нажатием и отпусканием (мс)
// в диапазоне [pressDelayMin, pressDelayMax].
static int GetNextPressDelay()
{
    int range = g_config.pressDelayMax - g_config.pressDelayMin + 1;
    return g_config.pressDelayMin + (rand() % range);
}


// ---------------------------------------------------------------------------
// Низкоуровневый хук клавиатуры: фиксирует нажатия ПРОБЕЛА (VK_SPACE).
// Работает глобально, даже когда окно не в фокусе.
// ---------------------------------------------------------------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* p =
            reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

        // Реагируем только на "клавиша нажата", игнорируем отпускание
        if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
            p->vkCode == VK_SPACE)
        {
            // Пробел переключает имитацию кликов мыши: вкл <-> выкл
            g_autoClickEnabled = !g_autoClickEnabled;

            if (g_autoClickEnabled)
            {
                // Запускаем таймер кликов с первым интервалом
                SetTimer(g_hwnd, TIMER_CLICK_ID, GetNextInterval(), NULL);
            }
            else
            {
                // Останавливаем таймер кликов
                KillTimer(g_hwnd, TIMER_CLICK_ID);
            }
        }
    }

    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}


// ---------------------------------------------------------------------------
// Обновление координат курсора
// ---------------------------------------------------------------------------
static void UpdateMousePosition()
{
    GetCursorPos(&g_mousePos);
}


// ---------------------------------------------------------------------------
// Имитация клика левой кнопкой мыши в текущей позиции курсора:
// отправляет события MOUSE_BUTTON_DOWN (LEFTDOWN) и MOUSE_BUTTON_UP (LEFTUP)
// через SendInput.
// ---------------------------------------------------------------------------
static void SimulateMouseClick()
{
    INPUT input = {};
    input.type = INPUT_MOUSE;

    // MOUSE_BUTTON_DOWN — кнопка нажата
    input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &input, sizeof(INPUT));

    // Случайная задержка между нажатием и отпусканием (как у реального клика)
    int pressDelay = GetNextPressDelay();
    if (pressDelay > 0)
        Sleep(pressDelay);

    // MOUSE_BUTTON_UP — кнопка отпущена
    input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));

    InterlockedIncrement(&g_clickCount);
}


// ---------------------------------------------------------------------------
// Обработчик окна
// ---------------------------------------------------------------------------
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_CREATE:
        {
            g_hwnd = hwnd;

            // Устанавливаем низкоуровневый хук клавиатуры (глобальный)
            g_keyboardHook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                LowLevelKeyboardProc,
                GetModuleHandleW(NULL),
                0
            );

            if (!g_keyboardHook)
            {
                MessageBoxW(hwnd, L"Не удалось установить хук клавиатуры.",
                            L"Ошибка", MB_ICONERROR);
            }

            // Запускаем таймер для периодического опроса координат
            SetTimer(hwnd, TIMER_ID, TIMER_MS, NULL);
            UpdateMousePosition();
            return 0;
        }

        case WM_TIMER:
        {
            if (wParam == TIMER_ID)
            {
                UpdateMousePosition();
                InvalidateRect(hwnd, NULL, FALSE); // запросить перерисовку
            }
            else if (wParam == TIMER_CLICK_ID)
            {
                // Если автоклик включён — отправляем клик и перепланируем таймер
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

            // Тёмный фон
            HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
            FillRect(hdc, &ps.rcPaint, bg);
            DeleteObject(bg);

            // Цвет и шрифт текста
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
            wsprintfW(title, L"..::-Автокликер-::..");
            TextOutW(hdc, 20, y, title, lstrlenW(title));
            y += lineHeight + 10;

            wchar_t coords[128];
            wsprintfW(coords, L"Координаты мыши:  X = %ld   Y = %ld",
                      g_mousePos.x, g_mousePos.y);
            TextOutW(hdc, 20, y, coords, lstrlenW(coords));
            y += lineHeight;

            // Состояние автоклика
            const wchar_t* autoOn  = L"Автоклик (клавиша Space):  ВКЛ";
            const wchar_t* autoOff = L"Автоклик (клавиша Space):  ВЫКЛ";
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

            // Текущий режим
            wchar_t modeText[128];
            if (g_config.useRandom)
                wsprintfW(modeText, L"Mode:  random");
            else
                wsprintfW(modeText, L"Mode:  fixed");
            TextOutW(hdc, 20, y, modeText, lstrlenW(modeText));
            y += lineHeight;

            // Текущий режим и интервал
            wchar_t intervalText[128];
            if (g_config.useRandom)
            {
                wsprintfW(intervalText, L"Интервал:  %d-%d мс (случайный)",
                          g_config.minInterval, g_config.maxInterval);
            }
            else
            {
                wsprintfW(intervalText, L"Интервал:  %d мс (фиксированный)",
                          g_config.fixedInterval);
            }
            TextOutW(hdc, 20, y, intervalText, lstrlenW(intervalText));
            y += lineHeight;

            wchar_t clicks[128];
            wsprintfW(clicks, L"Отправлено кликов: %ld", g_clickCount);
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
// Точка входа
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    // Читаем настройки из config.ini и инициализируем ГПСЧ
    LoadConfig();
    srand((unsigned)GetTickCount());

    const wchar_t CLASS_NAME[] = L"MouseSpaceTrackerWindow";

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
        L"Автокликер (WinAPI)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 320,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd)
    {
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Цикл сообщений (необходим и для работы хука клавиатуры)
    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
