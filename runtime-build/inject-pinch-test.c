/* Opt-in, bounded native acceptance helper; never part of the launcher.
 * Injects two synthetic touchpad contacts only into the specified foreground
 * QEMU window. Build: gcc inject-pinch-test.c -o inject-pinch-test.exe -luser32
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _WIN32_WINNT 0x0a00
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

typedef struct {
    POINTER_INPUT_TYPE type;
    ULONG count;
    POINTER_FEEDBACK_MODE feedback;
    HMONITOR monitor;
    ULONG width, height, options;
} TestDeviceParams;
static DWORD target_pid;
static HWND target;
static BOOL CALLBACK find_qemu(HWND window, LPARAM unused)
{
    DWORD pid;
    WCHAR cls[64];
    (void)unused;
    GetWindowThreadProcessId(window, &pid);
    GetClassNameW(window, cls, 64);
    if (pid == target_pid && IsWindowVisible(window) && !wcscmp(cls, L"SDL_app")) {
        if (target) { target = NULL; return FALSE; }
        target = window;
    }
    return TRUE;
}
int main(int argc, char **argv)
{
    char *end;
    const char *mode = argc == 3 ? argv[2] : "out";
    if ((argc != 2 && argc != 3) || !(target_pid = strtoul(argv[1], &end, 10)) || *end) return 2;
    if (strcmp(mode, "out") && strcmp(mode, "in") && strcmp(mode, "pan") &&
        strcmp(mode, "cancel") && strcmp(mode, "focus")) return 2;
    EnumWindows(find_qemu, 0);
    if (!target) { fprintf(stderr, "unique visible QEMU SDL window required\n"); return 2; }
    HMODULE user = GetModuleHandleW(L"user32.dll");
    HSYNTHETICPOINTERDEVICE (WINAPI *create)(TestDeviceParams *) =
        (void *)GetProcAddress(user, "CreateSyntheticPointerDevice2");
    BOOL (WINAPI *inject)(HSYNTHETICPOINTERDEVICE, const POINTER_TYPE_INFO *, UINT32) =
        (void *)GetProcAddress(user, "InjectSyntheticPointerInput");
    void (WINAPI *destroy)(HSYNTHETICPOINTERDEVICE) =
        (void *)GetProcAddress(user, "DestroySyntheticPointerDevice");
    if (!create || !inject || !destroy) { fprintf(stderr, "synthetic touchpad API unavailable\n"); return 3; }
    TestDeviceParams params = { (POINTER_INPUT_TYPE)5, 2, POINTER_FEEDBACK_NONE, NULL, 10000, 7000, 3 };
    HSYNTHETICPOINTERDEVICE device = create(&params);
    if (!device) { fprintf(stderr, "create failed: %lu\n", GetLastError()); return 3; }
    SetForegroundWindow(target);
    Sleep(200);
    RECT rect;
    GetClientRect(target, &rect);
    POINT center = {(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
    ClientToScreen(target, &center);
    POINT old_cursor;
    GetCursorPos(&old_cursor);
    if (GetForegroundWindow() != target || WindowFromPoint(center) != target) {
        fprintf(stderr, "target is not unobscured foreground; refusing injection\n");
        destroy(device); return 4;
    }
    SetCursorPos(center.x, center.y);
    int result = 0;
    HWND focus_window = NULL;
    for (int frame = 0; frame <= 61; frame++) {
        if (frame == 30 && !strcmp(mode, "focus")) {
            focus_window = CreateWindowExW(0, L"STATIC", L"Try Omarchy input cancellation test",
                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 20, 20, 300, 100, NULL, NULL, GetModuleHandleW(NULL), NULL);
            SetForegroundWindow(focus_window);
            Sleep(200);
            break;
        }
        if (GetForegroundWindow() != target) { result = 4; break; }
        POINTER_TYPE_INFO contacts[2] = {0};
        for (int slot = 0; slot < 2; slot++) {
            contacts[slot].type = (POINTER_INPUT_TYPE)5;
            POINTER_TOUCH_INFO *touch = &contacts[slot].touchInfo;
            touch->pointerInfo.pointerType = (POINTER_INPUT_TYPE)5;
            touch->pointerInfo.pointerId = slot + 1;
            touch->pointerInfo.pointerFlags = frame == 61 ? POINTER_FLAG_UP :
                POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_CONFIDENCE |
                (frame ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN);
            if (frame == 30 && !strcmp(mode, "cancel"))
                touch->pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
            int step = frame > 60 ? 60 : frame;
            int radius = !strcmp(mode, "in") ? 2700 - step * 25 :
                !strcmp(mode, "pan") ? 1200 : 1200 + step * 25;
            touch->pointerInfo.ptHimetricLocation.x = 5000 + (slot ? radius : -radius);
            touch->pointerInfo.ptHimetricLocation.y = 3500 + (!strcmp(mode, "pan") ? step * 25 : 0);
        }
        if (!inject(device, contacts, 2)) {
            fprintf(stderr, "frame %d failed: %lu\n", frame, GetLastError()); result = 5; break;
        }
        if (frame == 30 && !strcmp(mode, "cancel")) break;
        Sleep(20);
    }
    destroy(device);
    if (focus_window) { DestroyWindow(focus_window); SetForegroundWindow(target); }
    SetCursorPos(old_cursor.x, old_cursor.y);
    if (!result) printf("injected bounded two-contact %s gesture; verify guest events separately\n", mode);
    return result;
}
