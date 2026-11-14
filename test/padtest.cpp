// Test aid (not shipped): plays the game's calls against ./dinput8.dll the way the game does, prints what it gets.
// Build: make test   Run next to dinput8.dll:  SS_PAD_FAKE=1 padtest.exe   (fake: XInput slot 0 is a scripted pad)
#define DIRECTINPUT_VERSION 0x0800
#include <initguid.h>
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <cstdio>
#include <cstring>

static BOOL CALLBACK ObjCb(const DIDEVICEOBJECTINSTANCEA* o, LPVOID ref) {
  ++*static_cast<int*>(ref);
  printf("    object ofs=0x%02lx type=0x%08lx '%s'\n", o->dwOfs, o->dwType, o->tszName);
  return DIENUM_CONTINUE;
}
static GUID g_guid[8];
static int g_n = 0;
static BOOL CALLBACK DevCb(const DIDEVICEINSTANCEA* d, LPVOID) {
  printf("  device '%s' product %08lX-... type 0x%lx size %lu\n", d->tszInstanceName, d->guidProduct.Data1, d->dwDevType, d->dwSize);
  if (g_n < 8) g_guid[g_n++] = d->guidInstance;
  return DIENUM_CONTINUE;
}

int main() {
  HMODULE xi = LoadLibraryA("xinput1_3.dll");  // the game imports it, so it is mapped before the proxy runs
  HMODULE m = LoadLibraryA(".\\dinput8.dll");
  if (!m) { printf("no dinput8.dll here\n"); return 1; }
  auto create = reinterpret_cast<HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN)>(GetProcAddress(m, "DirectInput8Create"));
  IDirectInput8A* di = nullptr;
  HRESULT hr = create(GetModuleHandle(nullptr), 0x800, IID_IDirectInput8A, reinterpret_cast<LPVOID*>(&di), nullptr);
  printf("DirectInput8Create -> %08lx %p\n", hr, di);
  if (FAILED(hr)) return 1;
  hr = di->EnumDevices(4, DevCb, nullptr, 1);
  printf("EnumDevices -> %08lx, %d device(s)\n", hr, g_n);
  for (int i = 0; i < g_n; ++i) {
    IDirectInputDevice8A* d = nullptr;
    hr = di->CreateDevice(g_guid[i], &d, nullptr);
    printf("CreateDevice #%d -> %08lx\n", i, hr);
    if (FAILED(hr)) continue;
    // c_dfDIJoystick2 is what the game sets; this harness lets the device keep its own format (SetDataFormat is a no-op there)
    int n = 0;
    d->EnumObjects(ObjCb, &n, 0x1000003);
    printf("  FF axes: %d\n", n);
    n = 0;
    d->EnumObjects(ObjCb, &n, 0x1f);
    printf("  all: %d\n", n);
    DIPROPRANGE r;
    r.diph.dwSize = sizeof r; r.diph.dwHeaderSize = sizeof(DIPROPHEADER); r.diph.dwHow = DIPH_BYOFFSET;
    r.lMin = -32767; r.lMax = 32767;
    for (DWORD o = 0; o < 0x20; o += 4) { r.diph.dwObj = o; hr = d->SetProperty(DIPROP_RANGE, &r.diph); printf("  SetProperty range ofs %lu -> %08lx\n", o, hr); }
    hr = d->SetCooperativeLevel(nullptr, 5);
    hr = d->Acquire();
    printf("  Acquire -> %08lx, Poll -> %08lx\n", hr, d->Poll());
    BYTE st[0x110];
    memset(st, 0xCC, sizeof st);
    hr = d->GetDeviceState(0x110, st);
    LONG* ax = reinterpret_cast<LONG*>(st);
    DWORD* pov = reinterpret_cast<DWORD*>(st + 0x20);
    printf("  GetDeviceState -> %08lx  X=%ld Y=%ld Z=%ld Rx=%ld Ry=%ld Rz=%ld  POV0=%lu POV1=%lx  buttons:", hr, ax[0], ax[1], ax[2], ax[3], ax[4], ax[5], pov[0], pov[1]);
    for (int b = 0; b < 12; ++b) printf(" %02x", st[0x30 + b]);
    printf("  vel/accel/force zero: %d\n", !memchr(st + 0xB0, 0xCC, 0x60));
    d->Release();
  }
  // Rumble as the game sends it: XInputSetState(device list index, motors) through xinput1_3's export
  auto set = reinterpret_cast<DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*)>(GetProcAddress(xi, "XInputSetState"));
  XINPUT_VIBRATION v = {40000, 20000};
  for (DWORD idx = 0; idx < 3; ++idx) printf("XInputSetState(%lu) -> %lu\n", idx, set ? set(idx, &v) : 9999);
  di->Release();
  printf("done\n");
  return 0;
}
