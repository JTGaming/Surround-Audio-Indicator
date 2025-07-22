// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: https://pvs-studio.com
#include "Audio Meter.h"
#include "resource.h"

// we need commctrl v6 for LoadIconMetric()
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comsuppw.lib")

HINSTANCE g_hInst = NULL;
UINT const WMAPP_NOTIFYCALLBACK = WM_APP + 1;
wchar_t const szWindowClass[] = L"Audio Meter";
// Use a guid to uniquely identify our icon
class __declspec(uuid("3a8a77d4-1d6e-434b-8a88-11a5dd4aeca2")) NotifIcon;
HWND main_hwnd = NULL;
CHANNELS old_channel = PADDING;

int APIENTRY wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int
)
{
    // Get class id as string
    LPOLESTR className;
    HRESULT hr = StringFromCLSID(__uuidof(NotifIcon), &className);
    if (hr != S_OK)
        return -1;

    // convert to CString
    CString c = (char*)(_bstr_t)className;
    // then release the memory used by the class name
    CoTaskMemFree(className);

    CreateMutex(0, FALSE, c); // try to create a named mutex
    if (GetLastError() == ERROR_ALREADY_EXISTS) // did the mutex already exist?
        return -1; // quit; mutex is released automatically

    SetProcessDPIAware();
    g_hInst = hInstance;
    RegisterWindowClass(szWindowClass, WndProc);

    // Create the main window. This could be a hidden window if you don't need
    // any UI other than the notification icon.
    main_hwnd = CreateWindow(szWindowClass, szWindowClass, 0, 0, 0, 0, 0, 0, 0, g_hInst, 0);
    if (main_hwnd)
        MainLoop();

    return 0;
}

void MainLoop()
{
    NotificationClient pClient;

    std::vector<float> peaks{};
    UINT channels = 0;
    CHANNELS channel_id = INVALID;

    bool CanRun = true;
    while (CanRun)
    {
        Sleep(500);

        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                CanRun = false;

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        IAudioMeterInformation* pMeterInfo = pClient.GetMeter();
        if (!pMeterInfo)
        {
            UpdateIcon(INVALID);
            continue;
        }

        HRESULT hr = pMeterInfo->GetMeteringChannelCount(&channels);
        if (hr != S_OK)
        {
            UpdateIcon(INVALID);
            continue;
        }

        float volume{};
        hr = pMeterInfo->GetPeakValue(&volume);
        if (hr != S_OK)
        {
            UpdateIcon(INVALID);
            continue;
        }

        if (volume <= CUST_FLT_EPS)
            continue;

        switch (channels)
        {
        case 1:
            channel_id = MONO;
            break;

        case 2:
            channel_id = STEREO;
            break;

        default:
            peaks.resize(channels);
            hr = pMeterInfo->GetChannelsPeakValues(channels, peaks.data());
            if (hr != S_OK)
            {
                UpdateIcon(INVALID);
                continue;
            }

            channel_id = CheckChannels(channels, peaks);
            break;
        }

        bool force = pClient.ShouldForce();
        StoreChannels(channel_id, force);
        UpdateIcon(channel_id, force);
    }
}

CHANNELS CheckChannels(UINT channels, const std::vector<float>& peaks)
{
    if (channels <= STEREO || channels >= INVALID || channels != peaks.size())
        return INVALID;

    CHANNELS max_channel = STEREO;
    for (int idx = STEREO; idx < peaks.size(); idx++)
    {
        if (peaks[idx] > CUST_FLT_EPS)
            max_channel = (CHANNELS)(idx + 1);
    }

    return max_channel;
}

void StoreChannels(CHANNELS& channel_id, bool force)
{
    static CHANNELS saved_channels = PADDING;
    static auto start_time = std::chrono::high_resolution_clock::now();

    if (channel_id >= saved_channels)
    {
        saved_channels = channel_id;
        start_time = std::chrono::high_resolution_clock::now();
    }
    else
    {
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> delta_time_ms = end_time - start_time;

        if (delta_time_ms.count() > 10000 || force)
        {
            saved_channels = channel_id;
            start_time = end_time;
        }
        else
            channel_id = saved_channels;
    }
}

void RegisterWindowClass(PCWSTR pszClassName, WNDPROC lpfnWndProc)
{
    WNDCLASSEX wcex = { sizeof(wcex) };
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = lpfnWndProc;
    wcex.hInstance = g_hInst;
    wcex.lpszClassName = pszClassName;
    RegisterClassEx(&wcex);
}

BOOL AddIcon(HWND hwnd)
{
    NOTIFYICONDATA nid = { sizeof(nid) };
    nid.hWnd = hwnd;
    // add the icon, setting the icon, tooltip, and callback message.
    // the icon will be identified with the GUID
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_GUID;
    nid.guidItem = __uuidof(NotifIcon);
    nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
    LoadIconMetric(g_hInst, MAKEINTRESOURCE(IDI_NOTIFICATIONICONIDX + (int)old_channel), LIM_SMALL, &nid.hIcon);
    BOOL ret = Shell_NotifyIcon(NIM_ADD, &nid);
    if (ret != TRUE)
        return FALSE;

    // NOTIFYICON_VERSION_4 is prefered
    nid.uVersion = NOTIFYICON_VERSION_4;
    return Shell_NotifyIcon(NIM_SETVERSION, &nid);
}

void UpdateIcon(CHANNELS channel, bool force)
{
    if (channel == old_channel && !force)
        return;

    int IDX = (int)channel + IDI_NOTIFICATIONICONIDX;
    NOTIFYICONDATA nid = { sizeof(nid) };
    nid.uFlags = NIF_ICON | NIF_GUID;
    nid.guidItem = __uuidof(NotifIcon);

    HRESULT hr = LoadIconMetric(g_hInst, MAKEINTRESOURCE(IDX), LIM_SMALL, &nid.hIcon);
    if (hr != S_OK)
        return;
    BOOL ret = Shell_NotifyIcon(NIM_MODIFY, &nid);
    if (ret != TRUE)
    {
        DeleteIcon();
        AddIcon(main_hwnd);
        return;
    }

    old_channel = channel;
}

BOOL DeleteIcon()
{
    NOTIFYICONDATA nid = { sizeof(nid) };
    nid.uFlags = NIF_GUID;
    nid.guidItem = __uuidof(NotifIcon);
    return Shell_NotifyIcon(NIM_DELETE, &nid);
}

void ShowContextMenu(HWND hwnd)
{
    HMENU hMenu = LoadMenu(g_hInst, MAKEINTRESOURCE(IDC_CONTEXTMENU));
    if (hMenu)
    {
        HMENU hSubMenu = GetSubMenu(hMenu, 0);
        if (hSubMenu)
        {
            // our window must be foreground before calling TrackPopupMenu or the menu will not disappear when the user clicks away
            SetForegroundWindow(hwnd);

            // respect menu drop alignment
            UINT uFlags = TPM_RIGHTBUTTON;
            if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0)
                uFlags |= TPM_RIGHTALIGN;
            else
                uFlags |= TPM_LEFTALIGN;
            POINT pt;
            GetCursorPos(&pt);
            TrackPopupMenuEx(hSubMenu, uFlags, pt.x, pt.y, hwnd, NULL);
        }
        DestroyMenu(hMenu);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
        // add the notification icon
        if (!AddIcon(hwnd))
            return -1;
        break;
    case WM_COMMAND:
    {
        // Parse the menu selections:
        switch (LOWORD(wParam))
        {
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        default:
            return DefWindowProc(hwnd, message, wParam, lParam);
        }
    }
    break;

    case WMAPP_NOTIFYCALLBACK:
        switch (LOWORD(lParam))
        {
        case WM_CONTEXTMENU:
            ShowContextMenu(hwnd);
            break;
        }
        break;

    case WM_DESTROY:
        DeleteIcon();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, message, wParam, lParam);
    }
    return 0;
}
