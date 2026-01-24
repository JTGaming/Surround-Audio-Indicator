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

        now_time = std::chrono::steady_clock::now();

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
                channel_id = CheckChannelsMixV2(meterChannels, peaks, channelMap);
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
            max_channel = valid_channels[idx + 1]; //-V557
    }

    return max_channel;
}

bool isPowerOfTwo(uint32_t x)
{
    return (x & (x - 1)) == 0;
}

constexpr SPEAKER_IDX SpeakerToIndex(DWORD speaker)
{
    switch (speaker)
    {
    case SPEAKER_FRONT_LEFT:
    case SPEAKER_FRONT_LEFT_OF_CENTER:
        return FL;

    case SPEAKER_FRONT_RIGHT:
    case SPEAKER_FRONT_RIGHT_OF_CENTER:
        return FR;

    case SPEAKER_FRONT_CENTER:
        return FC;

    case SPEAKER_LOW_FREQUENCY:
        return LFE;

    case SPEAKER_BACK_LEFT:
        return BL;

    case SPEAKER_BACK_RIGHT:
        return BR;

    case SPEAKER_BACK_CENTER:
        return BC;

    case SPEAKER_SIDE_LEFT:
        return SL;

    case SPEAKER_SIDE_RIGHT:
        return SR;

    default:
        return SPEAKER_COUNT; // invalid
    }
}

CHANNELS CheckChannelsMixV2(UINT meterChannels, const std::vector<float>& meterPeaks, const std::vector<DWORD>* channelMap)
{
    if (meterChannels <= PADDING || meterChannels >= INVALID || meterChannels != meterPeaks.size())
        return INVALID;

    constexpr float HOLD_TIME = 5.0f;
    static std::array<std::chrono::time_point<std::chrono::steady_clock>, SPEAKER_COUNT> lastActiveTime{};

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

        auto idx = SpeakerToIndex(channelMap->at(i));
        if (idx != SPEAKER_COUNT)
            updateActivity(idx);
    }

    static const std::array<std::pair<CHANNELS, std::vector<std::vector<SPEAKER_IDX>>>, INVALID - 1> possible_channels({
        { MONO,     {{FC}, {LFE}}},
        { STEREO,   {{FL, FR}}},
        { TWOONE,   {{FL, FR, LFE}}},
        { THREEOH,  {{FL, FR, FC}}},
        { THREEONE, {{FL, FR, FC, LFE}}},

        { FOUROH,   {{FL, FR}, {BL, BR}, {SL, SR}, {FC, BC}} },
        { FOURONE,  {{FL, FR, LFE}, {BL, BR}, {SL, SR}, {FC, BC}} },
        { FIVEOH,  {{FL, FR, FC}, {BL, BR}, {SL, SR}} },
        { FIVEONE,  {{FL, FR, FC, LFE}, {BL, BR}, {SL, SR}} },
        { SIXOH,  {{FL, FR, FC, BC}, {BL, BR}, {SL, SR}} },
        { SIXONE,  {{FL, FR, FC, LFE, BC}, {BL, BR}, {SL, SR}} },

        { SEVENOH,  {{FL, FR, FC, BL, BR, SL, SR}}},
        { SEVENONE, {{FL, FR, FC, LFE, BL, BR, SL, SR}}}
    });

    std::array<uint32_t, INVALID - 1> found_groups({});
    std::array<bool, INVALID - 1> delete_channels({});

    for (int idx = SPEAKER_COUNT - 1; idx >= 0; --idx)
    {
        if (!isActive(idx))
			continue;

		uint32_t channel_idx = 0;
        for (const auto& [channel_type, speaker_groups] : possible_channels)
        {
            if (delete_channels[channel_idx])
            {
                channel_idx++;
                continue;
			}

            uint32_t group_idx = 0;
            bool found = false;
            for (const auto& group : speaker_groups)
            {
                if (std::find(group.begin(), group.end(), idx) != group.end())
                {
                    found = true;
                    if (group_idx != 0)
                        found_groups[channel_idx] |= (1u << group_idx);
                    break;
                }
                group_idx++;
            }

            if (!found || !isPowerOfTwo(found_groups[channel_idx]))
                delete_channels.at(channel_idx) = true;
            channel_idx++;
		}
    }

    for (int i = 0; i < INVALID - 1; ++i)
        if (!delete_channels[i])
			return possible_channels[i].first;
     return INVALID;
}

CHANNELS CheckChannelsMix(UINT meterChannels, const std::vector<float>& meterPeaks, const std::vector<DWORD>* channelMap)
{
    if (meterChannels <= PADDING || meterChannels >= INVALID || meterChannels != meterPeaks.size())
        return INVALID;

    CHANNELS playing_channels = INVALID;

    constexpr float HOLD_TIME = 5.0f;
    static std::vector<std::chrono::time_point<std::chrono::steady_clock>> lastActiveTime(SPEAKER_COUNT);

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
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_GUID | NIF_TIP;
    nid.guidItem = __uuidof(NotifIcon);
    nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
    LoadIconMetric(g_hInst, MAKEINTRESOURCE(IDI_NOTIFICATIONICONIDX + (int)old_channel), LIM_SMALL, &nid.hIcon);
    wcscpy_s(nid.szTip, szWindowClass);

    BOOL ret = Shell_NotifyIcon(NIM_ADD, &nid);
    if (ret != TRUE)
        return FALSE;

    nid.uVersion = NOTIFYICON_VERSION;
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
