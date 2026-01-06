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
CHANNELS old_channel = STEREO;
std::shared_mutex audioMutex;
std::chrono::steady_clock::time_point now_time;

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
    UINT meterChannels = 0;
    CHANNELS channel_id = INVALID;

    bool CanRun = true;
    while (CanRun)
    {
        Sleep(50);
        std::shared_lock lock(audioMutex);

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
        IAudioClient* pAudioClient = pClient.GetAudioClient();
        if (!pAudioClient)
        {
            UpdateIcon(INVALID);
            continue;
        }

        float volume{};
        HRESULT hr = pMeterInfo->GetPeakValue(&volume);
        if (hr != S_OK)
        {
            UpdateIcon(INVALID);
            continue;
        }

        if (volume <= CUST_FLT_EPS)
            continue;

        DWORD mixFormatChannels;
        const std::vector<DWORD>* channelMap{};
        pClient.GetMixInfo(mixFormatChannels, channelMap);

        now_time = std::chrono::high_resolution_clock::now();

        switch (mixFormatChannels)
        {
        case 1:
            channel_id = MONO;
            break;

        case 2:
            channel_id = STEREO;
            break;

        default:
            meterChannels = pClient.GetMeteringChannelCount();

            if (peaks.size() != meterChannels)
                peaks.resize(meterChannels);
            hr = pMeterInfo->GetChannelsPeakValues(meterChannels, peaks.data());
            if (hr != S_OK)
            {
                UpdateIcon(INVALID);
                continue;
            }


            if (mixFormatChannels == meterChannels)
                channel_id = CheckChannelsMix(meterChannels, peaks, channelMap);
            else
				channel_id = CheckChannels(meterChannels, peaks);

            break;
        }

        bool force = pClient.ShouldForce();
        StoreChannels(channel_id, force);
        UpdateIcon(channel_id, force);
    }
}

CHANNELS CheckChannels(UINT channels, const std::vector<float>& peaks)
{
    if (channels <= PADDING || channels >= INVALID || channels != peaks.size())
        return INVALID;

    constexpr CHANNELS valid_channels[] = {
        INVALID,
        STEREO,
        STEREO,
        THREEOH,
        TWOONE,
        FIVEONE,
        FIVEONE,
        SEVENONE,
        SEVENONE,
        INVALID
	};

    CHANNELS max_channel = PADDING;
    for (int idx = max_channel; idx < peaks.size(); idx++)
    {
        if (peaks[idx] > CUST_FLT_EPS)
            max_channel = valid_channels[idx + 1];
    }

    return max_channel;
}

CHANNELS CheckChannelsMix(UINT meterChannels, const std::vector<float>& meterPeaks, const std::vector<DWORD>* channelMap)
{
    if (meterChannels <= PADDING || meterChannels >= INVALID || meterChannels != meterPeaks.size())
        return INVALID;

    CHANNELS playing_channels = INVALID;

    constexpr float HOLD_TIME = 1.0f;
    static std::vector<std::chrono::time_point<std::chrono::high_resolution_clock>> lastActiveTime(9);

    enum SPEAKER_IDX
    {
        FL = 0,
        FR,
        FC,
        LFE,
        BL,
        BR,
        BC,
        SL,
        SR
    };

    static const std::unordered_map<DWORD, SPEAKER_IDX> SpeakerToIndex =
    {
        { SPEAKER_FRONT_LEFT,              FL },
        { SPEAKER_FRONT_LEFT_OF_CENTER,    FL },

        { SPEAKER_FRONT_RIGHT,             FR },
        { SPEAKER_FRONT_RIGHT_OF_CENTER,   FR },

        { SPEAKER_FRONT_CENTER,            FC },
        { SPEAKER_LOW_FREQUENCY,           LFE },

        { SPEAKER_BACK_LEFT,               BL },
        { SPEAKER_BACK_RIGHT,              BR },
        { SPEAKER_BACK_CENTER,             BC },

        { SPEAKER_SIDE_LEFT,               SL },
        { SPEAKER_SIDE_RIGHT,              SR }
    };

    auto updateActivity = [](size_t index) {
        lastActiveTime[index] = now_time;
        };

    auto isActive = [](size_t index) {
        auto duration = std::chrono::duration<float>(now_time - lastActiveTime[index]).count();
        return duration <= HOLD_TIME;
        };

    for (UINT i = 0; i < meterChannels; ++i)
    {
        if (meterPeaks[i] <= CUST_FLT_EPS)
            continue;

        auto it = SpeakerToIndex.find(channelMap->at(i));
        if (it != SpeakerToIndex.end())
            updateActivity(it->second);
    }

    if (isActive(SL) || isActive(SR))
    {
        if (meterChannels >= 8)
            playing_channels = SEVENONE;
        else if (meterChannels >= 6)
            playing_channels = FIVEONE;
    }
    else if (isActive(BL) || isActive(BR))
    {
        if (meterChannels >= 6)
            playing_channels = FIVEONE;
        else if (meterChannels >= 4)
            playing_channels = (isActive(LFE) ? FOURONE : FOUROH);
    }
    else if (isActive(BC))
        playing_channels = (isActive(LFE) ? FOURONE : FOUROH);
    else if (isActive(FL) || isActive(FR))
    {
        if (isActive(FC))
            playing_channels = (isActive(LFE) ? THREEONE : THREEOH);
        else
            playing_channels = (isActive(LFE) ? TWOONE : STEREO);
    }
    else if (isActive(FC))
        playing_channels = MONO;
    else if (isActive(LFE))
		playing_channels = MONO;

    return playing_channels;
}

void StoreChannels(CHANNELS& channel_id, bool force)
{
    static CHANNELS saved_channels = PADDING;
    static auto start_time = now_time;

    if (channel_id >= saved_channels)
    {
        saved_channels = channel_id;
        start_time = now_time;
    }
    else
    {
        std::chrono::duration<float, std::milli> delta_time_ms = now_time - start_time;

        if (delta_time_ms.count() > 10000 || force)
        {
            saved_channels = channel_id;
            start_time = now_time;
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
    if (ret == FALSE)
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
    if (ret == FALSE)
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
