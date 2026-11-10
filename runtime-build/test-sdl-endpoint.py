#!/usr/bin/env python3
"""Compile the Windows default-endpoint check against mocked COM calls."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched audio/sdlaudio.c')
source = parser.parse_args().source.read_text(encoding='utf-8')
start = source.index('static bool sdl_default_endpoint_missing(')
helper = source[start:source.index('#endif', start)]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef int32_t HRESULT;
typedef uint32_t DWORD;
typedef void *LPVOID, *LPUNKNOWN, *HMODULE;
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef const GUID *REFCLSID, *REFIID;
#define WINAPI
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define CLSCTX_INPROC_SERVER 1
#define ERROR_NOT_FOUND 1168
#define HRESULT_FROM_WIN32(x) ((HRESULT)(((x) & 0xffff) | 0x80070000))
#define E_FAIL ((HRESULT)0x80004005)
#define CO_E_NOTINITIALIZED ((HRESULT)0x800401f0)
enum { eRender, eCapture };
enum { eConsole, eMultimedia, eCommunications };
typedef struct IMMDevice IMMDevice;
typedef struct IMMDeviceEnumerator IMMDeviceEnumerator;
typedef struct { unsigned (*Release)(IMMDevice *); } IMMDeviceVtbl;
typedef struct {
    HRESULT (*GetDefaultAudioEndpoint)(IMMDeviceEnumerator *, int, int, IMMDevice **);
    unsigned (*Release)(IMMDeviceEnumerator *);
} IMMDeviceEnumeratorVtbl;
struct IMMDevice { const IMMDeviceVtbl *lpVtbl; };
struct IMMDeviceEnumerator { const IMMDeviceEnumeratorVtbl *lpVtbl; };
static bool ole32_loaded = true;
static HRESULT create_result, endpoint_result;
static int creates, device_refs, enumerator_refs, last_flow, last_role;
static unsigned device_release(IMMDevice *device) { return --device_refs; }
static const IMMDeviceVtbl device_vtbl = { device_release };
static IMMDevice device = { &device_vtbl };
static HRESULT get_default(IMMDeviceEnumerator *e, int flow, int role, IMMDevice **out) {
    last_flow = flow; last_role = role;
    if (endpoint_result < 0) { *out = NULL; return endpoint_result; }
    device_refs++; *out = &device; return endpoint_result;
}
static unsigned enumerator_release(IMMDeviceEnumerator *e) { return --enumerator_refs; }
static const IMMDeviceEnumeratorVtbl enumerator_vtbl = { get_default, enumerator_release };
static IMMDeviceEnumerator enumerator = { &enumerator_vtbl };
static const GUID clsid_expected = { 0xbcde0395, 0xe52f, 0x467c,
    { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } }; /* CLSID_MMDeviceEnumerator */
static const GUID iid_expected = { 0xa95664d2, 0x9614, 0x4f35,
    { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } }; /* IID_IMMDeviceEnumerator */
static HRESULT co_create_instance(REFCLSID clsid, LPUNKNOWN outer, DWORD context,
                                  REFIID iid, LPVOID *out) {
    creates++;
    assert(!memcmp(clsid, &clsid_expected, sizeof(GUID)));
    assert(!memcmp(iid, &iid_expected, sizeof(GUID)));
    assert(!outer && context == CLSCTX_INPROC_SERVER);
    if (create_result < 0) { *out = NULL; return create_result; }
    enumerator_refs++; *out = &enumerator; return create_result;
}
static HMODULE GetModuleHandleW(const wchar_t *name) {
    assert(!wcscmp(name, L"ole32.dll"));
    return ole32_loaded ? (HMODULE)1 : NULL;
}
static void *GetProcAddress(HMODULE module, const char *name) {
    assert(module && !strcmp(name, "CoCreateInstance"));
    return (void *)co_create_instance;
}
''' + helper + r'''
int main(void) {
    /* A present default endpoint is left to SDL; both objects are released. */
    assert(!sdl_default_endpoint_missing(0));
    assert(creates == 1 && last_flow == eRender && last_role == eConsole);
    assert(!device_refs && !enumerator_refs);
    assert(!sdl_default_endpoint_missing(1) && last_flow == eCapture);

    /* Only Windows' "not found" answer counts as missing. */
    endpoint_result = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    assert(sdl_default_endpoint_missing(0) && !enumerator_refs);
    endpoint_result = E_FAIL;
    assert(!sdl_default_endpoint_missing(0) && !enumerator_refs);
    endpoint_result = 0;

    /* COM or loader failures leave the decision to SDL. */
    create_result = CO_E_NOTINITIALIZED;
    assert(!sdl_default_endpoint_missing(0) && !enumerator_refs);
    create_result = 0;
    ole32_loaded = false;
    int before = creates;
    assert(!sdl_default_endpoint_missing(0) && creates == before);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-endpoint-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-Werror', str(test / 'test.c'),
                    '-o', str(test / 'test')], check=True)
    subprocess.run([str(test / 'test')], check=True)
print('ok - Windows default endpoint check: present, missing, other errors, COM failure and releases')
