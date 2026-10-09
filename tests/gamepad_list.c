/* neutron gamepad test: lists the game controllers a Windows game can see through
 * the four APIs games use, with names, VID/PID and current state:
 *   XInput (xinput1_4), DirectInput 8 game controllers, raw input HID devices
 *   (what SDL's and Unity's HID drivers open) and Windows.Gaming.Input gamepads.
 * Console only, no window.
 *
 * Usage:
 *   gamepad_list.exe                 list once
 *   gamepad_list.exe watch [sec]     print every state change and hotplug for sec
 *                                    seconds (default 30): press buttons, move sticks,
 *                                    unplug and plug pads
 *   gamepad_list.exe rumble          XInput rumble on each pad (left motor, right motor),
 *                                    then the trigger motors through Windows.Gaming.Input
 *   gamepad_list.exe selftest        with NEUTRON_PAD_VIRTUAL=1 (or =<vid>:<pid>): checks the
 *                                    virtual pad winebus attaches through SDL (scripted
 *                                    buttons and axes, rumble loopback, hotplug) in all
 *                                    four APIs
 *   gamepad_list.exe selftest absent the virtual pad must not show up (a PlayStation
 *                                    ID with NEUTRON_PAD_HIDRAW=1: SDL devices of
 *                                    raw HID pads are dropped)
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o gamepad_list.exe gamepad_list.c
 *        -ldinput8 -ldxguid -lhid -lruntimeobject -lole32
 * Exit code: 0 = ok (list, watch, rumble always; selftest when all checks passed). */
#define DIRECTINPUT_VERSION 0x0800
#define COBJMACROS
#define WIDL_using_Windows_Gaming_Input
#include <initguid.h>
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <hidsdi.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.gaming.input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- XInput, loaded at runtime for the hidden ordinals ---- */

static DWORD (WINAPI *pXInputGetStateEx)(DWORD, XINPUT_STATE *);     /* ordinal 100, has the guide button */
static DWORD (WINAPI *pXInputSetState)(DWORD, XINPUT_VIBRATION *);
static DWORD (WINAPI *pXInputGetCapabilities)(DWORD, DWORD, XINPUT_CAPABILITIES *);
static DWORD (WINAPI *pXInputGetCapabilitiesEx)(DWORD, DWORD, DWORD, XINPUT_CAPABILITIES_EX *); /* ordinal 108 */
static DWORD (WINAPI *pXInputGetBatteryInformation)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);

static BOOL load_xinput(void)
{
    HMODULE xi = LoadLibraryA("xinput1_4.dll");
    if (!xi) return FALSE;
    pXInputGetStateEx = (void *)GetProcAddress(xi, (const char *)100);
    if (!pXInputGetStateEx) pXInputGetStateEx = (void *)GetProcAddress(xi, "XInputGetState");
    pXInputSetState = (void *)GetProcAddress(xi, "XInputSetState");
    pXInputGetCapabilities = (void *)GetProcAddress(xi, "XInputGetCapabilities");
    pXInputGetCapabilitiesEx = (void *)GetProcAddress(xi, (const char *)108);
    pXInputGetBatteryInformation = (void *)GetProcAddress(xi, "XInputGetBatteryInformation");
    return pXInputGetStateEx && pXInputSetState && pXInputGetCapabilities;
}

static void format_xinput(const XINPUT_GAMEPAD *g, char *buf, size_t size)
{
    snprintf(buf, size, "buttons=%04x LT=%3u RT=%3u LX=%6d LY=%6d RX=%6d RY=%6d",
             g->wButtons, g->bLeftTrigger, g->bRightTrigger, g->sThumbLX, g->sThumbLY, g->sThumbRX, g->sThumbRY);
}

static void list_xinput(void)
{
    printf("XInput:\n");
    if (!pXInputGetStateEx) { printf("  xinput1_4.dll not loaded\n"); return; }
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
    {
        XINPUT_STATE st;
        XINPUT_CAPABILITIES caps;
        XINPUT_CAPABILITIES_EX capsex;
        XINPUT_BATTERY_INFORMATION bat;
        char state[160];
        DWORD rc = pXInputGetStateEx(i, &st);

        if (rc == ERROR_DEVICE_NOT_CONNECTED) { printf("  slot %lu: not connected\n", i); continue; }
        if (rc) { printf("  slot %lu: error %lu\n", i, rc); continue; }
        memset(&caps, 0, sizeof(caps));
        pXInputGetCapabilities(i, 0, &caps);
        printf("  slot %lu: connected, type %u subtype %u flags %04x", i, caps.Type, caps.SubType, caps.Flags);
        memset(&capsex, 0, sizeof(capsex));
        if (pXInputGetCapabilitiesEx && !pXInputGetCapabilitiesEx(1, i, 0, &capsex))
            printf(", vid:pid %04x:%04x", capsex.VendorId, capsex.ProductId);
        if (pXInputGetBatteryInformation)
        {
            rc = pXInputGetBatteryInformation(i, BATTERY_DEVTYPE_GAMEPAD, &bat);
            if (rc) printf(", battery error %lu", rc);
            else printf(", battery type %u level %u", bat.BatteryType, bat.BatteryLevel);
        }
        format_xinput(&st.Gamepad, state, sizeof(state));
        printf("\n    %s\n", state);
    }
}

/* ---- DirectInput 8 ---- */

#define MAX_DI 16
static IDirectInput8W *dinput;
static struct di_pad
{
    IDirectInputDevice8W *dev;
    GUID guid;
    WCHAR name[MAX_PATH];
    DWORD vidpid, type;
    DIJOYSTATE2 last;
    BOOL seen;
} di_pads[MAX_DI];
static int di_count;

static BOOL CALLBACK di_enum_cb(const DIDEVICEINSTANCEW *inst, void *ctx)
{
    struct di_pad *pad;
    DIPROPDWORD prop = {{sizeof(prop), sizeof(prop.diph), 0, DIPH_DEVICE}};

    if (di_count >= MAX_DI) return DIENUM_STOP;
    for (int i = 0; i < di_count; i++)
        if (IsEqualGUID(&di_pads[i].guid, &inst->guidInstance)) { di_pads[i].seen = TRUE; return DIENUM_CONTINUE; }

    pad = &di_pads[di_count];
    memset(pad, 0, sizeof(*pad));
    pad->guid = inst->guidInstance;
    pad->type = inst->dwDevType;
    lstrcpynW(pad->name, inst->tszProductName, MAX_PATH);
    if (FAILED(IDirectInput8_CreateDevice(dinput, &inst->guidInstance, &pad->dev, NULL))) return DIENUM_CONTINUE;
    if (SUCCEEDED(IDirectInputDevice8_GetProperty(pad->dev, DIPROP_VIDPID, &prop.diph))) pad->vidpid = prop.dwData;
    IDirectInputDevice8_SetDataFormat(pad->dev, &c_dfDIJoystick2);
    IDirectInputDevice8_Acquire(pad->dev);
    pad->seen = TRUE;
    di_count++;
    if (*(BOOL *)ctx) printf("dinput: added %ls (%04lx:%04lx)\n", pad->name, pad->vidpid & 0xffff, pad->vidpid >> 16);
    return DIENUM_CONTINUE;
}

/* Enumerates again, adds new devices, drops devices that are gone. */
static void update_dinput(BOOL announce)
{
    if (!dinput) return;
    for (int i = 0; i < di_count; i++) di_pads[i].seen = FALSE;
    IDirectInput8_EnumDevices(dinput, DI8DEVCLASS_GAMECTRL, di_enum_cb, &announce, DIEDFL_ATTACHEDONLY);
    for (int i = 0; i < di_count; i++)
    {
        if (di_pads[i].seen) continue;
        if (announce) printf("dinput: removed %ls\n", di_pads[i].name);
        IDirectInputDevice8_Release(di_pads[i].dev);
        di_pads[i--] = di_pads[--di_count];
    }
}

static HRESULT read_dinput(struct di_pad *pad, DIJOYSTATE2 *st)
{
    HRESULT hr;
    memset(st, 0, sizeof(*st));
    IDirectInputDevice8_Poll(pad->dev);
    hr = IDirectInputDevice8_GetDeviceState(pad->dev, sizeof(*st), st);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
    {
        IDirectInputDevice8_Acquire(pad->dev);
        hr = IDirectInputDevice8_GetDeviceState(pad->dev, sizeof(*st), st);
    }
    return hr;
}

static void format_dinput(const DIJOYSTATE2 *st, char *buf, size_t size)
{
    unsigned int buttons = 0;
    for (int b = 0; b < 32; b++) if (st->rgbButtons[b] & 0x80) buttons |= 1u << b;
    snprintf(buf, size, "buttons=%08x pov=%ld x=%ld y=%ld z=%ld rx=%ld ry=%ld rz=%ld",
             buttons, (long)(st->rgdwPOV[0] == 0xffffffff ? -1 : (LONG)st->rgdwPOV[0]),
             st->lX, st->lY, st->lZ, st->lRx, st->lRy, st->lRz);
}

static void list_dinput(void)
{
    printf("DirectInput 8 game controllers:\n");
    if (!dinput) { printf("  DirectInput8Create failed\n"); return; }
    if (!di_count) printf("  none\n");
    for (int i = 0; i < di_count; i++)
    {
        struct di_pad *pad = &di_pads[i];
        DIJOYSTATE2 st;
        char state[200];
        HRESULT hr = read_dinput(pad, &st);
        printf("  %ls, vid:pid %04lx:%04lx, type %02x subtype %02x\n", pad->name,
               pad->vidpid & 0xffff, pad->vidpid >> 16, GET_DIDEVICE_TYPE(pad->type), GET_DIDEVICE_SUBTYPE(pad->type));
        if (FAILED(hr)) printf("    GetDeviceState hr=%08lx\n", (unsigned long)hr);
        else { format_dinput(&st, state, sizeof(state)); printf("    %s\n", state); }
    }
}

/* ---- raw input HID devices ---- */

#define MAX_HID 32
static struct hid_dev
{
    HANDLE raw;
    WCHAR path[MAX_PATH];
    WCHAR product[128];
    DWORD vid, pid;
    USHORT page, usage;
    HANDLE file, event;
    OVERLAPPED ov;
    BYTE report[256], last[256];
    DWORD len, last_len;
    BOOL reading, seen;
} hids[MAX_HID];
static int hid_count;

static void hid_start_read(struct hid_dev *h)
{
    DWORD got;
    if (h->file == INVALID_HANDLE_VALUE || h->reading) return;
    ResetEvent(h->event);
    memset(&h->ov, 0, sizeof(h->ov));
    h->ov.hEvent = h->event;
    if (ReadFile(h->file, h->report, sizeof(h->report), &got, &h->ov)) { h->len = got; SetEvent(h->event); }
    else if (GetLastError() != ERROR_IO_PENDING) { CloseHandle(h->file); h->file = INVALID_HANDLE_VALUE; return; }
    h->reading = TRUE;
}

/* Returns TRUE when a new input report arrived. */
static BOOL hid_poll(struct hid_dev *h, DWORD timeout)
{
    DWORD got;
    hid_start_read(h);
    if (!h->reading || WaitForSingleObject(h->event, timeout)) return FALSE;
    h->reading = FALSE;
    if (!GetOverlappedResult(h->file, &h->ov, &got, FALSE)) return FALSE;
    h->len = got;
    return TRUE;
}

static void format_hid(const BYTE *report, DWORD len, char *buf, size_t size)
{
    size_t pos = snprintf(buf, size, "report (%lu bytes):", len);
    for (DWORD i = 0; i < len && i < 24 && pos + 4 < size; i++) pos += snprintf(buf + pos, size - pos, " %02x", report[i]);
    if (len > 24 && pos + 4 < size) snprintf(buf + pos, size - pos, " ...");
}

static void update_hid(BOOL announce)
{
    RAWINPUTDEVICELIST list[128];
    UINT count = ARRAYSIZE(list);

    for (int i = 0; i < hid_count; i++) hids[i].seen = FALSE;
    count = GetRawInputDeviceList(list, &count, sizeof(list[0]));
    if (count == (UINT)-1) count = 0;
    for (UINT i = 0; i < count; i++)
    {
        RID_DEVICE_INFO info = {sizeof(info)};
        UINT size = sizeof(info);
        struct hid_dev *h = NULL;

        if (list[i].dwType != RIM_TYPEHID) continue;
        for (int j = 0; j < hid_count; j++) if (hids[j].raw == list[i].hDevice) h = &hids[j];
        if (h) { h->seen = TRUE; continue; }
        if (hid_count >= MAX_HID) break;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &size) == (UINT)-1) continue;

        h = &hids[hid_count++];
        memset(h, 0, sizeof(*h));
        h->raw = list[i].hDevice;
        h->seen = TRUE;
        h->vid = info.hid.dwVendorId;
        h->pid = info.hid.dwProductId;
        h->page = info.hid.usUsagePage;
        h->usage = info.hid.usUsage;
        size = MAX_PATH;
        GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, h->path, &size);
        h->file = CreateFileW(h->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h->file == INVALID_HANDLE_VALUE)
            h->file = CreateFileW(h->path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h->file != INVALID_HANDLE_VALUE) HidD_GetProductString(h->file, h->product, sizeof(h->product));
        h->event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (announce) printf("rawinput: added %ls (%04lx:%04lx usage %02x:%02x) %ls\n", h->product, h->vid, h->pid,
                             h->page, h->usage, h->path);
    }
    for (int i = 0; i < hid_count; i++)
    {
        struct hid_dev *h = &hids[i];
        if (h->seen) continue;
        if (announce) printf("rawinput: removed %ls (%04lx:%04lx)\n", h->product, h->vid, h->pid);
        if (h->file != INVALID_HANDLE_VALUE) { CancelIo(h->file); CloseHandle(h->file); }
        CloseHandle(h->event);
        hids[i--] = hids[--hid_count];
    }
}

static void list_hid(void)
{
    printf("Raw input HID devices:\n");
    if (!hid_count) printf("  none\n");
    for (int i = 0; i < hid_count; i++)
    {
        struct hid_dev *h = &hids[i];
        char state[160];
        printf("  %ls, vid:pid %04lx:%04lx, usage %02x:%02x%s\n    %ls\n", h->product, h->vid, h->pid, h->page, h->usage,
               h->file == INVALID_HANDLE_VALUE ? ", cannot open" : "", h->path);
        if (hid_poll(h, 200)) { format_hid(h->report, h->len, state, sizeof(state)); printf("    %s\n", state); }
        else if (h->file != INVALID_HANDLE_VALUE) printf("    no input report within 200 ms\n");
    }
}

/* ---- Windows.Gaming.Input ---- */

static IGamepadStatics *wgi_statics;
static IRawGameControllerStatics *wgi_raw_statics;

static BOOL init_wgi(void)
{
    HSTRING name;
    HRESULT hr;
    static const WCHAR gamepad[] = L"Windows.Gaming.Input.Gamepad";
    static const WCHAR raw[] = L"Windows.Gaming.Input.RawGameController";

    if (FAILED(RoInitialize(RO_INIT_MULTITHREADED))) return FALSE;
    WindowsCreateString(gamepad, wcslen(gamepad), &name);
    hr = RoGetActivationFactory(name, &IID_IGamepadStatics, (void **)&wgi_statics);
    WindowsDeleteString(name);
    if (FAILED(hr)) { wgi_statics = NULL; return FALSE; }
    WindowsCreateString(raw, wcslen(raw), &name);
    if (FAILED(RoGetActivationFactory(name, &IID_IRawGameControllerStatics, (void **)&wgi_raw_statics)))
        wgi_raw_statics = NULL;
    WindowsDeleteString(name);
    return TRUE;
}

/* Name and VID/PID of a gamepad through its RawGameController. */
static void wgi_describe(IGamepad *pad, char *buf, size_t size)
{
    IGameController *controller = NULL;
    IRawGameController *raw = NULL;
    IRawGameController2 *raw2 = NULL;
    UINT16 vid = 0, pid = 0;
    HSTRING name = NULL;
    boolean wireless = 0;

    snprintf(buf, size, "?");
    if (FAILED(IGamepad_QueryInterface(pad, &IID_IGameController, (void **)&controller))) return;
    IGameController_get_IsWireless(controller, &wireless);
    if (wgi_raw_statics && SUCCEEDED(IRawGameControllerStatics_FromGameController(wgi_raw_statics, controller, &raw)) && raw)
    {
        IRawGameController_get_HardwareVendorId(raw, &vid);
        IRawGameController_get_HardwareProductId(raw, &pid);
        if (SUCCEEDED(IRawGameController_QueryInterface(raw, &IID_IRawGameController2, (void **)&raw2)))
        {
            IRawGameController2_get_DisplayName(raw2, &name);
            IRawGameController2_Release(raw2);
        }
        IRawGameController_Release(raw);
    }
    snprintf(buf, size, "%ls, vid:pid %04x:%04x%s", name ? WindowsGetStringRawBuffer(name, NULL) : L"(no name)",
             vid, pid, wireless ? ", wireless" : "");
    if (name) WindowsDeleteString(name);
    IGameController_Release(controller);
}

static void format_wgi(const GamepadReading *r, char *buf, size_t size)
{
    snprintf(buf, size, "buttons=%05x LT=%.2f RT=%.2f LX=%+.2f LY=%+.2f RX=%+.2f RY=%+.2f", (unsigned int)r->Buttons,
             r->LeftTrigger, r->RightTrigger, r->LeftThumbstickX, r->LeftThumbstickY, r->RightThumbstickX, r->RightThumbstickY);
}

/* Gamepad n of the current list (AddRef'd), NULL past the end. */
static IGamepad *wgi_gamepad(UINT32 n, UINT32 *count)
{
    __FIVectorView_1_Windows__CGaming__CInput__CGamepad *pads = NULL;
    IGamepad *pad = NULL;
    UINT32 size = 0;

    if (count) *count = 0;
    if (!wgi_statics || FAILED(IGamepadStatics_get_Gamepads(wgi_statics, &pads)) || !pads) return NULL;
    __FIVectorView_1_Windows__CGaming__CInput__CGamepad_get_Size(pads, &size);
    if (count) *count = size;
    if (n < size) __FIVectorView_1_Windows__CGaming__CInput__CGamepad_GetAt(pads, n, &pad);
    __FIVectorView_1_Windows__CGaming__CInput__CGamepad_Release(pads);
    return pad;
}

static void list_wgi(void)
{
    UINT32 count = 0;
    IGamepad *pad;

    printf("Windows.Gaming.Input gamepads:\n");
    if (!wgi_statics) { printf("  activation factory failed\n"); return; }
    pad = wgi_gamepad(0, &count);
    if (pad) IGamepad_Release(pad);
    if (!count) printf("  none\n");
    for (UINT32 i = 0; i < count; i++)
    {
        GamepadReading r;
        char desc[200], state[200];
        if (!(pad = wgi_gamepad(i, NULL))) break;
        wgi_describe(pad, desc, sizeof(desc));
        memset(&r, 0, sizeof(r));
        IGamepad_GetCurrentReading(pad, &r);
        format_wgi(&r, state, sizeof(state));
        printf("  %u: %s\n    %s\n", i, desc, state);
        IGamepad_Release(pad);
    }
}

/* ---- watch ---- */

static void watch(int seconds)
{
    XINPUT_STATE xlast[XUSER_MAX_COUNT];
    DWORD xrc[XUSER_MAX_COUNT];
    GamepadReading wlast[8];
    UINT32 wcount = 0;
    DWORD end = GetTickCount() + seconds * 1000, next_enum = 0;
    char buf[200];

    printf("watching for %d s: press buttons, move sticks and triggers, plug and unplug pads\n", seconds);
    for (int i = 0; i < XUSER_MAX_COUNT; i++) xrc[i] = pXInputGetStateEx ? pXInputGetStateEx(i, &xlast[i]) : ~0u;
    memset(wlast, 0, sizeof(wlast));
    wgi_gamepad(~0u, &wcount);
    for (int i = 0; i < di_count; i++) read_dinput(&di_pads[i], &di_pads[i].last);
    for (int i = 0; i < hid_count; i++) { hids[i].last_len = 0; }

    while ((LONG)(end - GetTickCount()) > 0)
    {
        DWORD now = GetTickCount();
        double t = (seconds * 1000 - (LONG)(end - now)) / 1000.0;

        for (DWORD i = 0; pXInputGetStateEx && i < XUSER_MAX_COUNT; i++)
        {
            XINPUT_STATE st;
            DWORD rc = pXInputGetStateEx(i, &st);
            if (rc != xrc[i]) printf("%6.2f xinput slot %lu: %s\n", t, i, rc ? "disconnected" : "connected");
            else if (!rc && memcmp(&st.Gamepad, &xlast[i].Gamepad, sizeof(st.Gamepad)))
            {
                format_xinput(&st.Gamepad, buf, sizeof(buf));
                printf("%6.2f xinput slot %lu: %s\n", t, i, buf);
            }
            xrc[i] = rc;
            xlast[i] = st;
        }

        if ((LONG)(now - next_enum) >= 0)
        {
            int before = di_count;
            update_dinput(TRUE);
            for (int i = before; i < di_count; i++) read_dinput(&di_pads[i], &di_pads[i].last);
            update_hid(TRUE);
            next_enum = now + 1000;
        }
        for (int i = 0; i < di_count; i++)
        {
            DIJOYSTATE2 st;
            if (FAILED(read_dinput(&di_pads[i], &st))) continue;
            if (!memcmp(&st, &di_pads[i].last, sizeof(st))) continue;
            format_dinput(&st, buf, sizeof(buf));
            printf("%6.2f dinput %ls: %s\n", t, di_pads[i].name, buf);
            di_pads[i].last = st;
        }
        for (int i = 0; i < hid_count; i++)
        {
            struct hid_dev *h = &hids[i];
            if (h->page != 1 || (h->usage != 4 && h->usage != 5 && h->usage != 8)) continue;
            while (hid_poll(h, 0))
            {
                if (h->len == h->last_len && !memcmp(h->report, h->last, h->len)) continue;
                memcpy(h->last, h->report, h->len);
                h->last_len = h->len;
                format_hid(h->report, h->len, buf, sizeof(buf));
                printf("%6.2f rawinput %04lx:%04lx %s\n", t, h->vid, h->pid, buf);
            }
        }

        {
            UINT32 count = 0;
            IGamepad *pad = wgi_gamepad(~0u, &count);
            if (pad) IGamepad_Release(pad);
            if (count != wcount) printf("%6.2f wgi: %u gamepad(s)\n", t, count);
            wcount = count;
            for (UINT32 i = 0; i < count && i < ARRAYSIZE(wlast); i++)
            {
                GamepadReading r;
                if (!(pad = wgi_gamepad(i, NULL))) break;
                memset(&r, 0, sizeof(r));
                IGamepad_GetCurrentReading(pad, &r);
                IGamepad_Release(pad);
                r.Timestamp = 0;
                if (!memcmp(&r, &wlast[i], sizeof(r))) continue;
                format_wgi(&r, buf, sizeof(buf));
                printf("%6.2f wgi gamepad %u: %s\n", t, i, buf);
                wlast[i] = r;
            }
        }
        fflush(stdout);
        Sleep(16);
    }
}

/* ---- rumble ---- */

static void rumble(void)
{
    for (DWORD i = 0; pXInputSetState && i < XUSER_MAX_COUNT; i++)
    {
        XINPUT_STATE st;
        XINPUT_VIBRATION v = {0};
        if (pXInputGetStateEx(i, &st)) continue;
        printf("xinput slot %lu: left (low frequency) motor 1 s\n", i); fflush(stdout);
        v.wLeftMotorSpeed = 0xc000; pXInputSetState(i, &v); Sleep(1000);
        printf("xinput slot %lu: right (high frequency) motor 1 s\n", i); fflush(stdout);
        v.wLeftMotorSpeed = 0; v.wRightMotorSpeed = 0xc000; pXInputSetState(i, &v); Sleep(1000);
        v.wRightMotorSpeed = 0; pXInputSetState(i, &v); Sleep(300);
    }
    for (UINT32 i = 0;; i++)
    {
        GamepadVibration v = {0};
        IGamepad *pad = wgi_gamepad(i, NULL);
        if (!pad) break;
        printf("wgi gamepad %u: left trigger motor 1 s\n", i); fflush(stdout);
        v.LeftTrigger = 0.8; IGamepad_put_Vibration(pad, v); Sleep(1000);
        printf("wgi gamepad %u: right trigger motor 1 s\n", i); fflush(stdout);
        v.LeftTrigger = 0; v.RightTrigger = 0.8; IGamepad_put_Vibration(pad, v); Sleep(1000);
        v.RightTrigger = 0; IGamepad_put_Vibration(pad, v);
        IGamepad_Release(pad);
    }
}

/* ---- selftest against winebus' virtual pad (NEUTRON_PAD_VIRTUAL=1) ----
 * The pad (045e:028e unless NEUTRON_PAD_VIRTUAL=<vid>:<pid>) cycles every 400 ms through: nothing, A, B + dpad up,
 * left stick right/up + left trigger full, nothing. Rumble comes back as input:
 * right stick X = low frequency motor / 2, right trigger = high frequency motor / 2.
 * Rumble 0xffff/0x0001 unplugs it for one second. */

static int passed, failed;
static unsigned int vpad_vid = 0x045e, vpad_pid = 0x028e;

static void check(BOOL ok, const char *name, const char *detail)
{
    if (ok) { passed++; printf("ok %s\n", name); }
    else { failed++; printf("FAIL %s: %s\n", name, detail); }
    fflush(stdout);
}

static int find_virtual_slot(DWORD timeout)
{
    DWORD end = GetTickCount() + timeout;
    do
    {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
        {
            XINPUT_STATE st;
            XINPUT_CAPABILITIES_EX caps;
            if (pXInputGetStateEx(i, &st)) continue;
            memset(&caps, 0, sizeof(caps));
            if (!pXInputGetCapabilitiesEx || pXInputGetCapabilitiesEx(1, i, 0, &caps)) return i;
            if (caps.VendorId == vpad_vid && caps.ProductId == vpad_pid) return i;
        }
        Sleep(50);
    } while ((LONG)(end - GetTickCount()) > 0);
    return -1;
}

/* Waits until the pad shows the given state. */
static BOOL wait_xinput(int slot, BOOL (*match)(const XINPUT_GAMEPAD *), DWORD timeout, XINPUT_GAMEPAD *seen)
{
    DWORD end = GetTickCount() + timeout;
    do
    {
        XINPUT_STATE st;
        if (!pXInputGetStateEx(slot, &st))
        {
            *seen = st.Gamepad;
            if (match(&st.Gamepad)) return TRUE;
        }
        Sleep(5);
    } while ((LONG)(end - GetTickCount()) > 0);
    return FALSE;
}

static BOOL is_a(const XINPUT_GAMEPAD *g) { return (g->wButtons & 0x7fff) == XINPUT_GAMEPAD_A; }
static BOOL is_b_up(const XINPUT_GAMEPAD *g) { return (g->wButtons & 0x7fff) == (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_DPAD_UP); }
static BOOL is_stick(const XINPUT_GAMEPAD *g)
{
    return !(g->wButtons & 0x7fff) && g->sThumbLX == 32767 && g->sThumbLY == 32767 && g->bLeftTrigger == 255;
}
static BOOL is_neutral(const XINPUT_GAMEPAD *g)
{
    return !(g->wButtons & 0x7fff) && !g->bLeftTrigger && abs(g->sThumbLX) < 2 && abs(g->sThumbLY) < 2;
}
static BOOL is_rumble_echo(const XINPUT_GAMEPAD *g) { return abs(g->sThumbRX - 0x4000) <= 2 && abs(g->bRightTrigger - 64) <= 2; }
static BOOL is_rumble_off(const XINPUT_GAMEPAD *g) { return abs(g->sThumbRX) <= 2 && g->bRightTrigger == 0; }

static int selftest(BOOL absent)
{
    char detail[200], state[160];
    XINPUT_GAMEPAD seen = {0};
    XINPUT_VIBRATION v;
    const char *env = getenv("NEUTRON_PAD_VIRTUAL");
    int slot;

    if (env && strcmp(env, "1")) sscanf(env, "%x:%x", &vpad_vid, &vpad_pid);
    if (absent)
    {
        slot = find_virtual_slot(8000);
        snprintf(detail, sizeof(detail), "XInput pad %04x:%04x in slot %d", vpad_vid, vpad_pid, slot);
        check(slot < 0, "xinput_absent", detail);
        update_dinput(FALSE);
        check(!di_count, "dinput_absent", "a DirectInput game controller is there");
        printf("gamepad selftest: %d/%d passed\n", passed, passed + failed);
        return failed ? 1 : 0;
    }

    slot = find_virtual_slot(15000);
    snprintf(detail, sizeof(detail), "no XInput pad %04x:%04x within 15 s (is NEUTRON_PAD_VIRTUAL set?)", vpad_vid, vpad_pid);
    check(slot >= 0, "xinput_connected", detail);
    if (slot < 0) return 1;

    format_xinput(&seen, state, sizeof(state));
    check(wait_xinput(slot, is_a, 3000, &seen), "xinput_button_a", "A alone never seen");
    check(wait_xinput(slot, is_b_up, 3000, &seen), "xinput_button_b_dpad_up", "B + dpad up never seen");
    format_xinput(&seen, state, sizeof(state));
    snprintf(detail, sizeof(detail), "last %s", state);
    check(wait_xinput(slot, is_stick, 3000, &seen), "xinput_left_stick_trigger", detail);
    check(wait_xinput(slot, is_neutral, 3000, &seen), "xinput_neutral", "neutral state never seen");

    memset(&v, 0, sizeof(v));
    v.wLeftMotorSpeed = 0x8000;
    v.wRightMotorSpeed = 0x4000;
    check(pXInputSetState(slot, &v) == ERROR_SUCCESS, "xinput_set_state", "XInputSetState failed");
    format_xinput(&seen, state, sizeof(state));
    {
        BOOL ok = wait_xinput(slot, is_rumble_echo, 3000, &seen);
        format_xinput(&seen, state, sizeof(state));
        snprintf(detail, sizeof(detail), "rumble 0x8000/0x4000 not echoed, last %s", state);
        check(ok, "xinput_rumble_loopback", detail);
    }
    /* XInput rumble lasts until the game changes it (Wine's HID haptics have a 1 s cutoff) */
    Sleep(1500);
    {
        BOOL ok = wait_xinput(slot, is_rumble_echo, 0, &seen);
        format_xinput(&seen, state, sizeof(state));
        snprintf(detail, sizeof(detail), "rumble gone after 1.5 s, last %s", state);
        check(ok, "xinput_rumble_held", detail);
    }
    memset(&v, 0, sizeof(v));
    pXInputSetState(slot, &v);
    {
        BOOL ok = wait_xinput(slot, is_rumble_off, 3000, &seen);
        format_xinput(&seen, state, sizeof(state));
        snprintf(detail, sizeof(detail), "rumble stop not echoed, last %s", state);
        check(ok, "xinput_rumble_stop", detail);
    }

    {
        XINPUT_BATTERY_INFORMATION bat = {0};
        DWORD rc = pXInputGetBatteryInformation ? pXInputGetBatteryInformation(slot, BATTERY_DEVTYPE_GAMEPAD, &bat) : ~0u;
        snprintf(detail, sizeof(detail), "rc %lu type %u level %u", rc, bat.BatteryType, bat.BatteryLevel);
        check(rc == ERROR_SUCCESS && bat.BatteryType == BATTERY_TYPE_WIRED && bat.BatteryLevel == BATTERY_LEVEL_FULL,
              "xinput_battery_wired", detail);
    }

    /* DirectInput: the same pad, and its buttons move */
    {
        struct di_pad *pad = NULL;
        DWORD end;
        BOOL pressed = FALSE;
        update_dinput(FALSE);
        for (int i = 0; i < di_count; i++) if (di_pads[i].vidpid == MAKELONG(vpad_vid, vpad_pid)) pad = &di_pads[i];
        check(pad != NULL, "dinput_device", "no DirectInput game controller with the pad's VID/PID");
        end = GetTickCount() + 3000;
        while (pad && !pressed && (LONG)(end - GetTickCount()) > 0)
        {
            DIJOYSTATE2 st;
            if (SUCCEEDED(read_dinput(pad, &st)) && (st.rgbButtons[0] & 0x80)) pressed = TRUE;
            Sleep(5);
        }
        check(pressed, "dinput_button", "button 1 never pressed");
    }

    /* raw input: a HID game controller with the pad's VID/PID */
    {
        BOOL found = FALSE;
        update_hid(FALSE);
        for (int i = 0; i < hid_count; i++)
            if (hids[i].vid == vpad_vid && hids[i].pid == vpad_pid && hids[i].page == 1 && hids[i].usage == 5) found = TRUE;
        check(found, "rawinput_hid_gamepad", "no HID gamepad with the pad's VID/PID in GetRawInputDeviceList");
    }

    /* Windows.Gaming.Input: one gamepad, its A button */
    {
        DWORD end = GetTickCount() + 5000;
        BOOL pressed = FALSE;
        UINT32 count = 0;
        while (!pressed && (LONG)(end - GetTickCount()) > 0)
        {
            IGamepad *pad = wgi_gamepad(0, &count);
            if (pad)
            {
                GamepadReading r;
                IGamepad_GetCurrentReading(pad, &r);
                if (r.Buttons & GamepadButtons_A) pressed = TRUE;
                IGamepad_Release(pad);
            }
            Sleep(5);
        }
        snprintf(detail, sizeof(detail), "%u gamepads", count);
        check(count >= 1, "wgi_gamepad", detail);
        check(pressed, "wgi_button_a", "A never seen in GetCurrentReading");
    }

    /* hotplug: the pad unplugs itself for one second */
    {
        DWORD end, rc = 0, t0 = GetTickCount();
        XINPUT_STATE st;
        v.wLeftMotorSpeed = 0xffff;
        v.wRightMotorSpeed = 0x0001;
        pXInputSetState(slot, &v);
        end = GetTickCount() + 3000;
        while ((LONG)(end - GetTickCount()) > 0 && (rc = pXInputGetStateEx(slot, &st)) == ERROR_SUCCESS) Sleep(5);
        snprintf(detail, sizeof(detail), "XInputGetState rc %lu", rc);
        check(rc == ERROR_DEVICE_NOT_CONNECTED, "hotplug_removed", detail);
        slot = find_virtual_slot(5000);
        snprintf(detail, sizeof(detail), "no pad back after %lu ms", GetTickCount() - t0);
        check(slot >= 0, "hotplug_added", detail);
        if (slot >= 0) check(wait_xinput(slot, is_a, 3000, &seen), "hotplug_input", "no input after replug");
    }

    printf("gamepad selftest: %d/%d passed\n", passed, passed + failed);
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "list";

    setvbuf(stdout, NULL, _IONBF, 0);
    load_xinput();
    if (FAILED(DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&dinput, NULL)))
        dinput = NULL;
    init_wgi();
    /* XInput, WGI and winebus pick devices up in the background: give them a moment */
    Sleep(500);
    update_dinput(FALSE);
    update_hid(FALSE);

    if (!strcmp(mode, "selftest")) return selftest(argc > 2 && !strcmp(argv[2], "absent"));

    list_xinput();
    list_dinput();
    list_hid();
    list_wgi();

    if (!strcmp(mode, "watch")) watch(argc > 2 ? atoi(argv[2]) : 30);
    else if (!strcmp(mode, "rumble")) rumble();
    return 0;
}
