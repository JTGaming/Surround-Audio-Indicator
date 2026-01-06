#pragma once
#include <windows.h>
#include <initguid.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <vector>
#include <chrono>
#include <atlstr.h>
#include <comutil.h>
#include <audioclient.h>
#include <shared_mutex>
#include <unordered_map>

extern std::shared_mutex audioMutex;

#define SAFE_RELEASE(punk)  \
              if ((punk) != NULL)  \
                { (punk)->Release(); (punk) = NULL; }
#define CUST_FLT_EPS (0.0001f)

//L R C LFE BL BR
enum CHANNELS : UINT
{
    PADDING = 0,
    MONO,       //1.0
    STEREO,     //2.0
    TWOONE,     //2.1
    THREEOH,    //3.0
    THREEONE,   //3.1
    FOUROH,     //4.0
    FOURONE,    //4.1 or 5.0
    FIVEONE,    //5.1
    SEVENONE,   //7.1
    INVALID     //
};

// Forward declarations of functions included in this code module:
void                RegisterWindowClass(PCWSTR pszClassName, WNDPROC lpfnWndProc);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
void                ShowContextMenu(HWND hwnd);
BOOL                AddIcon(HWND hwnd);
void                UpdateIcon(CHANNELS channel, bool force = false);
BOOL                DeleteIcon();
void                MainLoop();
CHANNELS            CheckChannels(UINT channels, const std::vector<float>& peaks);
CHANNELS            CheckChannelsMix(UINT channels, const std::vector<float>& peaks, const std::vector<DWORD>* channelMap);
void                StoreChannels(CHANNELS& channel_id, bool force = false);

// The notification client class
class NotificationClient : public IMMNotificationClient {
public:
    NotificationClient() {
        Start();
    }

    ~NotificationClient() {
        Close();
    }

    bool Start() {
        // Initialize the COM library for the current thread
        HRESULT hr = CoInitialize(NULL);

        if (SUCCEEDED(hr)) {
            // Create the device enumerator
            IMMDeviceEnumerator* pEnumerator;
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
            if (SUCCEEDED(hr)) {
                // Register for device change notifications
                hr = pEnumerator->RegisterEndpointNotificationCallback(this);
                m_pEnumerator = pEnumerator;

                return true;
            }

            CoUninitialize();
        }

        return false;
    }

    void Close() {
        // Unregister the device enumerator
        if (m_pEnumerator) {
            m_pEnumerator->UnregisterEndpointNotificationCallback(this);
            SAFE_RELEASE(m_pEnumerator);
        }
        SAFE_RELEASE(pDevice);
        SAFE_RELEASE(pMeterInfo);
        SAFE_RELEASE(pAudioClient);

        // Uninitialize the COM library for the current thread
        CoUninitialize();
    }

    IAudioMeterInformation* GetMeter()
    {
        if (!pMeterInfo && m_pEnumerator) {
            HRESULT hr = CoInitialize(NULL);

            if (SUCCEEDED(hr)) {
                // Get peak meter for default audio-rendering device.
                hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
                if (SUCCEEDED(hr))
                {
                    hr = pDevice->Activate(__uuidof(IAudioMeterInformation),
                        CLSCTX_ALL, NULL, (void**)&pMeterInfo);

                    LPWSTR id = nullptr;
                    pDevice->GetId(&id);
                    currentDeviceId = id;
                    CoTaskMemFree(id);
                }
                UpdateMeteringChannelCount();
                CoUninitialize();
            }
        }

        return pMeterInfo;
    }

    IAudioClient* GetAudioClient()
    {
        if (!pAudioClient && m_pEnumerator) {
            HRESULT hr = CoInitialize(NULL);

            if (SUCCEEDED(hr)) {
                // Get peak meter for default audio-rendering device.
                hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
                if (SUCCEEDED(hr))
                {
                    hr = pDevice->Activate(__uuidof(IAudioClient),
                        CLSCTX_ALL, NULL, (void**)&pAudioClient);
                 
                    LPWSTR id = nullptr;
                    pDevice->GetId(&id);
                    currentDeviceId = id;
                    CoTaskMemFree(id);
                }
                UpdateMixInfo();
                CoUninitialize();
            }
        }

        return pAudioClient;
    }

    bool ShouldForce()
    {
        bool force = bForceUpdate;
        bForceUpdate = false;

        return force;
    }

    HRESULT UpdateMeteringChannelCount()
    {
        if (!pMeterInfo)
            return E_POINTER;

        HRESULT hr = pMeterInfo->GetMeteringChannelCount(&meterChannels);
        return hr;
    }

    UINT GetMeteringChannelCount()
    {
        return meterChannels;
    }

    HRESULT UpdateMixInfo()
    {
        if (!pAudioClient)
            return E_POINTER;

        WAVEFORMATEX* pwfx = nullptr;
        HRESULT hr = pAudioClient->GetMixFormat(&pwfx);
        if (hr != S_OK)
            return hr;

        auto* wfex = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
        mixFormatChannels = wfex->Format.nChannels;
        DWORD channelMask = wfex->dwChannelMask;
        CoTaskMemFree(pwfx);

        channelMap.clear();
        channelMap.reserve(meterChannels);
        for (DWORD bit = 0; bit < 32; ++bit)
        {
            DWORD speaker = 1u << bit;
            if (channelMask & speaker)
                channelMap.push_back(speaker);
        }

        return S_OK;
    }

    void GetMixInfo(DWORD& pmixFormatChannels, const std::vector<DWORD>*& pchannelMap)
    {
        pmixFormatChannels = mixFormatChannels;
		pchannelMap = &channelMap;
    }

    // IUnknown methods
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) { //-V835
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppvObject = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() {
        return InterlockedIncrement(&m_cRef);
    }

    ULONG STDMETHODCALLTYPE Release() {
        ULONG ulRef = InterlockedDecrement(&m_cRef);
        if (0 == ulRef) {
            delete this;
        }
        return ulRef;
    }

    // IMMNotificationClient methods
    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow, ERole, LPCWSTR pwstrDefaultDeviceId) {
        // Default audio device has been changed.
        Sleep(500);
        std::unique_lock lock(audioMutex);

        if (m_pEnumerator) {
            SAFE_RELEASE(pDevice);
            SAFE_RELEASE(pMeterInfo);
            SAFE_RELEASE(pAudioClient);

            HRESULT hr = CoInitialize(NULL);
            if (SUCCEEDED(hr)) {
                // Get peak meter for default audio-rendering device.
                hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
                if (SUCCEEDED(hr))
                {
                    hr = pDevice->Activate(__uuidof(IAudioMeterInformation),
                        CLSCTX_ALL, NULL, (void**)&pMeterInfo) &
                        pDevice->Activate(__uuidof(IAudioClient),
                            CLSCTX_ALL, NULL, (void**)&pAudioClient);
                    if (SUCCEEDED(hr))
                        bForceUpdate = true;
                }

                UpdateMeteringChannelCount();
                UpdateMixInfo();
                CoUninitialize();
				currentDeviceId = pwstrDefaultDeviceId;
            }
        }

        return S_OK;
    }

    STDMETHOD(OnDeviceAdded)(LPCWSTR) {
        // A new audio device has been added.
        return S_OK;
    }

    STDMETHOD(OnDeviceRemoved)(LPCWSTR) {
        // An audio device has been removed.
        return S_OK;
    }

    STDMETHOD(OnDeviceStateChanged)(LPCWSTR pwstrDeviceId, DWORD newState) {
        // The state of an audio device has changed.

        if (currentDeviceId == pwstrDeviceId)
        {
            Sleep(500);
            std::unique_lock lock(audioMutex);

            if (newState == DEVICE_STATE_ACTIVE)
            {
                // Device came back
                if (m_pEnumerator) {
                    SAFE_RELEASE(pDevice);
                    SAFE_RELEASE(pMeterInfo);
                    SAFE_RELEASE(pAudioClient);

                    HRESULT hr = CoInitialize(NULL);
                    if (SUCCEEDED(hr)) {
                        // Get peak meter for default audio-rendering device.
                        hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
                        if (SUCCEEDED(hr))
                        {
                            hr = pDevice->Activate(__uuidof(IAudioMeterInformation),
                                CLSCTX_ALL, NULL, (void**)&pMeterInfo) &
                                pDevice->Activate(__uuidof(IAudioClient),
                                    CLSCTX_ALL, NULL, (void**)&pAudioClient);
                            if (SUCCEEDED(hr))
                                bForceUpdate = true;
                        }

                        UpdateMeteringChannelCount();
                        UpdateMixInfo();
                        CoUninitialize();
                        currentDeviceId = pwstrDeviceId;
                    }
                }
            }
            else
            {
                // Device is gone or unavailable
                SAFE_RELEASE(pDevice);
                SAFE_RELEASE(pMeterInfo);
                SAFE_RELEASE(pAudioClient);
            }
        }

        return S_OK;
    }

    STDMETHOD(OnPropertyValueChanged)(LPCWSTR pwstrDeviceId, const PROPERTYKEY propertyKey) { //-V801
        // A property value of an audio device has changed.

        if (currentDeviceId == pwstrDeviceId)
            if (propertyKey == PKEY_AudioEndpoint_PhysicalSpeakers ||
                propertyKey == PKEY_AudioEngine_DeviceFormat)
            {
                Sleep(500);
                std::unique_lock lock(audioMutex);

                SAFE_RELEASE(pDevice);
                SAFE_RELEASE(pMeterInfo);
                SAFE_RELEASE(pAudioClient);

                GetMeter();
				GetAudioClient();
            }

        return S_OK;
    }

private:
    LONG m_cRef;
    IMMDeviceEnumerator* m_pEnumerator;
    IMMDevice* pDevice;
    IAudioMeterInformation* pMeterInfo;
    IAudioClient* pAudioClient;

    bool bForceUpdate = true;
    UINT meterChannels = 0;
    DWORD mixFormatChannels = 0;
    std::vector<DWORD> channelMap{};
    std::wstring currentDeviceId{};
};
