#pragma once
#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <vector>
#include <chrono>
#include <atlstr.h>
#include <comutil.h>

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
    TWOONE,     //2.1 or 3.0
    THREEONE,   //3.1 or 4.0
    FOURONE,    //4.1 or 5.0
    FIVEONE,    //5.1
    UNUSED6,    //
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
                    hr = pDevice->Activate(__uuidof(IAudioMeterInformation),
                        CLSCTX_ALL, NULL, (void**)&pMeterInfo);

                CoUninitialize();
            }
        }

        return pMeterInfo;
    }

    bool ShouldForce()
    {
        bool force = bForceUpdate;
        bForceUpdate = false;

        return force;
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
    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow, ERole, LPCWSTR) {
        // Default audio device has been changed.
        if (pMeterInfo && m_pEnumerator) {
            SAFE_RELEASE(pMeterInfo);

            HRESULT hr = CoInitialize(NULL);
            if (SUCCEEDED(hr)) {
                // Get peak meter for default audio-rendering device.
                hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
                if (SUCCEEDED(hr))
                {
                    hr = pDevice->Activate(__uuidof(IAudioMeterInformation),
                        CLSCTX_ALL, NULL, (void**)&pMeterInfo);
                    if (SUCCEEDED(hr))
                        bForceUpdate = true;
                }

                CoUninitialize();
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

    STDMETHOD(OnDeviceStateChanged)(LPCWSTR, DWORD) {
        // The state of an audio device has changed.
        return S_OK;
    }

    STDMETHOD(OnPropertyValueChanged)(LPCWSTR, const PROPERTYKEY) { //-V801
        // A property value of an audio device has changed.
        return S_OK;
    }

private:
    LONG m_cRef;
    IMMDeviceEnumerator* m_pEnumerator;
    IMMDevice* pDevice;
    IAudioMeterInformation* pMeterInfo;

    bool bForceUpdate = true;
};
