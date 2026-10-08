/* neutron sandbox I/O test: audio output (CoreAudio through winecoreaudio), game
 * controller enumeration (IOHID through winebus) and XInput, all without a window.
 * Plays 0.3 s of silence. Prints one line per check, exit code 0 when all worked.
 *
 * Build: x86_64-w64-mingw32-clang -O2 -o iotest.exe iotest.c -lwinmm -lxinput -ldinput8 -ldxguid -lole32
 */
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <mmsystem.h>
#include <dinput.h>
#include <xinput.h>
#include <stdio.h>

static int devices;

static BOOL CALLBACK enum_cb(const DIDEVICEINSTANCEA *inst, void *ctx)
{
    devices++;
    printf("  dinput device: %s\n", inst->tszProductName);
    return DIENUM_CONTINUE;
}

int main(void)
{
    int fail = 0;
    /* audio: open the default device, play silence */
    WAVEFORMATEX fmt = { WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 };
    static short buf[48000 * 2 * 3 / 10];
    WAVEHDR hdr = { (char *)buf, sizeof(buf) };
    HWAVEOUT wo;
    MMRESULT mr = waveOutOpen(&wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (mr == MMSYSERR_NOERROR) {
        waveOutPrepareHeader(wo, &hdr, sizeof(hdr));
        mr = waveOutWrite(wo, &hdr, sizeof(hdr));
        Sleep(400);
        waveOutReset(wo);
        waveOutUnprepareHeader(wo, &hdr, sizeof(hdr));
        waveOutClose(wo);
    }
    printf("audio_waveout          %s mm=%u devs=%u\n", mr == MMSYSERR_NOERROR ? "OK" : "FAIL", mr, waveOutGetNumDevs());
    fail |= mr != MMSYSERR_NOERROR;

    /* controllers: DirectInput enumeration (winebus, IOHID) */
    IDirectInput8A *di;
    HRESULT hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&di, NULL);
    if (SUCCEEDED(hr)) {
        hr = IDirectInput8_EnumDevices(di, DI8DEVCLASS_ALL, enum_cb, NULL, DIEDFL_ATTACHEDONLY);
        IDirectInput8_Release(di);
    }
    printf("dinput_enum            %s hr=0x%08lx devices=%d\n", SUCCEEDED(hr) ? "OK" : "FAIL", (unsigned long)hr, devices);
    fail |= FAILED(hr);

    /* XInput: no pad connected is fine (ERROR_DEVICE_NOT_CONNECTED) */
    XINPUT_STATE st;
    DWORD xr = XInputGetState(0, &st);
    printf("xinput_state           %s rc=%lu\n", xr == ERROR_SUCCESS || xr == ERROR_DEVICE_NOT_CONNECTED ? "OK" : "FAIL", (unsigned long)xr);
    fail |= !(xr == ERROR_SUCCESS || xr == ERROR_DEVICE_NOT_CONNECTED);
    return fail;
}
