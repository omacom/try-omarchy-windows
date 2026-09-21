/* Read-only Windows endpoint/session evidence for one QEMU process.
 * Build in MSYS2 UCRT64:
 *   gcc probe-audio-sessions.c -o probe-audio-sessions.exe -lole32 -luuid
 * Run while the guest is playing/recording: probe-audio-sessions.exe QEMU_PID
 * This does not open audio streams or change any route, volume or default.
 */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdlib.h>

static void print_name(const WCHAR *name)
{
    char text[4096];
    if (!name || !WideCharToMultiByte(CP_UTF8, 0, name, -1,
                                    text, sizeof(text), NULL, NULL)) {
        printf("(unnamed)");
        return;
    }
    for (char *p = text; *p; p++) {
        putchar(*p == '\r' || *p == '\n' ? ' ' : *p);
    }
}

int main(int argc, char **argv)
{
    char *end;
    unsigned long requested;
    IMMDeviceEnumerator *devices = NULL;
    int found = 0;
    if (argc != 2 || !(requested = strtoul(argv[1], &end, 10)) || *end) {
        fprintf(stderr, "usage: probe-audio-sessions.exe QEMU_PID\n");
        return 2;
    }
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 2;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&devices);
    if (FAILED(hr)) { CoUninitialize(); return 2; }
    for (int direction = 0; direction < 2; direction++) {
        IMMDeviceCollection *collection = NULL;
        UINT count = 0;
        hr = IMMDeviceEnumerator_EnumAudioEndpoints(devices,
                direction ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr)) continue;
        IMMDeviceCollection_GetCount(collection, &count);
        for (UINT index = 0; index < count; index++) {
            IMMDevice *device = NULL;
            IAudioSessionManager2 *manager = NULL;
            IAudioSessionEnumerator *sessions = NULL;
            IPropertyStore *properties = NULL;
            PROPVARIANT name;
            int session_count = 0;
            PropVariantInit(&name);
            if (FAILED(IMMDeviceCollection_Item(collection, index, &device))) continue;
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &properties))) {
                IPropertyStore_GetValue(properties, &PKEY_Device_FriendlyName, &name);
                IPropertyStore_Release(properties);
            }
            hr = IMMDevice_Activate(device, &IID_IAudioSessionManager2,
                                     CLSCTX_ALL, NULL, (void **)&manager);
            if (SUCCEEDED(hr) && SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(manager, &sessions))) {
                IAudioSessionEnumerator_GetCount(sessions, &session_count);
                for (int i = 0; i < session_count; i++) {
                    IAudioSessionControl *control = NULL;
                    IAudioSessionControl2 *control2 = NULL;
                    DWORD pid = 0;
                    AudioSessionState state;
                    if (FAILED(IAudioSessionEnumerator_GetSession(sessions, i, &control))) continue;
                    hr = IAudioSessionControl_QueryInterface(control,
                            &IID_IAudioSessionControl2, (void **)&control2);
                    if (SUCCEEDED(hr)) {
                        if (SUCCEEDED(IAudioSessionControl2_GetProcessId(control2, &pid)) &&
                            pid == requested && SUCCEEDED(IAudioSessionControl_GetState(control, &state))) {
                            printf("pid=%lu direction=%s state=%s device=", requested,
                                   direction ? "recording" : "playback",
                                   state == AudioSessionStateActive ? "active" :
                                   state == AudioSessionStateInactive ? "inactive" : "expired");
                            print_name(name.vt == VT_LPWSTR ? name.pwszVal : NULL);
                            putchar('\n');
                            found++;
                        }
                        IAudioSessionControl2_Release(control2);
                    }
                    IAudioSessionControl_Release(control);
                }
                IAudioSessionEnumerator_Release(sessions);
            }
            if (manager) IAudioSessionManager2_Release(manager);
            PropVariantClear(&name);
            IMMDevice_Release(device);
        }
        IMMDeviceCollection_Release(collection);
    }
    IMMDeviceEnumerator_Release(devices);
    CoUninitialize();
    return found ? 0 : 1;
}
