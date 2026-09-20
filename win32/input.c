/****************************************************************************
 *  Genesis Plus GX -- Win32 GUI frontend
 *
 *  input.c -- feeds the core's input.pad[] / input.analog[] once per frame.
 *
 *  Keyboard state is polled rather than driven from WM_KEYDOWN so that
 *  several buttons held at once behave correctly and key repeat never leaks
 *  into the emulated pad. Polling reads the global key state, so everything
 *  is gated on the window actually having focus.
 *
 *  XInput is resolved at runtime: linking against it directly would stop the
 *  executable from starting on systems where the DLL is missing, and a
 *  gamepad is optional.
 ****************************************************************************/

#include <windows.h>

#include "shared.h"
#include "gui.h"

/****************************************************************************
 * XInput, loaded on demand
 ****************************************************************************/

#define XPAD_DPAD_UP        0x0001
#define XPAD_DPAD_DOWN      0x0002
#define XPAD_DPAD_LEFT      0x0004
#define XPAD_DPAD_RIGHT     0x0008
#define XPAD_START          0x0010
#define XPAD_BACK           0x0020
#define XPAD_LEFT_SHOULDER  0x0100
#define XPAD_RIGHT_SHOULDER 0x0200
#define XPAD_A              0x1000
#define XPAD_B              0x2000
#define XPAD_X              0x4000
#define XPAD_Y              0x8000
#define XPAD_LEFT_TRIGGER   0x00010000
#define XPAD_RIGHT_TRIGGER  0x00020000

#define XPAD_TRIGGER_THRESHOLD 30   /* out of 0-255; comfortably past resting noise */

#define XPAD_MAX_DEVICES    4
#define XPAD_DEADZONE       10000

typedef struct
{
  WORD  wButtons;
  BYTE  bLeftTrigger;
  BYTE  bRightTrigger;
  SHORT sThumbLX;
  SHORT sThumbLY;
  SHORT sThumbRX;
  SHORT sThumbRY;
} XPAD_GAMEPAD;

typedef struct
{
  DWORD        dwPacketNumber;
  XPAD_GAMEPAD Gamepad;
} XPAD_STATE;

typedef DWORD (WINAPI *xinput_get_state_fn)(DWORD, XPAD_STATE *);

static HMODULE             xinput_dll;
static xinput_get_state_fn xinput_get_state;

/* Cached once per frame so four devices do not mean four polls each. */
static XPAD_STATE xpad_state[XPAD_MAX_DEVICES];
static int        xpad_connected[XPAD_MAX_DEVICES];

static int  has_focus = 1;
static int  frame_advance_armed;

/****************************************************************************
 * Setup
 ****************************************************************************/

void gui_input_init(void)
{
  static const char *candidates[] =
  {
    "xinput1_4.dll",     /* Windows 8 and later  */
    "xinput1_3.dll",     /* DirectX SDK redist   */
    "xinput9_1_0.dll",   /* Windows 7 in-box     */
    NULL
  };
  int i;

  for (i = 0; candidates[i]; i++)
  {
    xinput_dll = LoadLibraryA(candidates[i]);
    if (xinput_dll) break;
  }

  if (xinput_dll)
  {
    xinput_get_state =
      (xinput_get_state_fn)(void *)GetProcAddress(xinput_dll, "XInputGetState");
  }
}

void gui_input_shutdown(void)
{
  if (xinput_dll)
  {
    FreeLibrary(xinput_dll);
    xinput_dll = NULL;
    xinput_get_state = NULL;
  }
}

void gui_input_set_focus(int focused)
{
  has_focus = focused;
}

/****************************************************************************
 * Polling helpers
 ****************************************************************************/

/* Tracks the last time either Alt or Ctrl was seen held, so a brief
   grace period after release (below) can still suppress game input --
   not just while the modifier is physically down. Both are shortcut
   modifiers used throughout this app's own accelerator table (Ctrl+O,
   Ctrl+R, Ctrl+C, etc., alongside Alt+Enter), so a key that's also
   mapped to a Genesis button shouldn't leak through as gameplay input
   during or immediately around one of those combos. */
static DWORD last_modifier_seen;

static int key_down(int vk)
{
  if (!vk || !has_focus) return 0;

  if ((GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000))
  {
    last_modifier_seen = GetTickCount();
    return 0;
  }
  if (GetTickCount() - last_modifier_seen < 1000) return 0;

  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void poll_gamepads(void)
{
  int i;

  for (i = 0; i < XPAD_MAX_DEVICES; i++)
  {
    xpad_connected[i] = 0;
    ZeroMemory(&xpad_state[i], sizeof(XPAD_STATE));
  }

  if (!xinput_get_state || !has_focus) return;

  for (i = 0; i < XPAD_MAX_DEVICES; i++)
  {
    if (xinput_get_state((DWORD)i, &xpad_state[i]) == ERROR_SUCCESS)
    {
      xpad_connected[i] = 1;
    }
  }
}

/* Buttons plus the left stick folded into the d-pad bits, and the analog
   triggers folded in as digital past XPAD_TRIGGER_THRESHOLD -- same
   treatment as the thumbstick-as-dpad above, just for LT/RT instead. */
static DWORD gamepad_buttons(int device)
{
  DWORD b;

  if (device < 0 || device >= XPAD_MAX_DEVICES || !xpad_connected[device]) return 0;

  b = xpad_state[device].Gamepad.wButtons;

  if (xpad_state[device].Gamepad.sThumbLY >  XPAD_DEADZONE) b |= XPAD_DPAD_UP;
  if (xpad_state[device].Gamepad.sThumbLY < -XPAD_DEADZONE) b |= XPAD_DPAD_DOWN;
  if (xpad_state[device].Gamepad.sThumbLX < -XPAD_DEADZONE) b |= XPAD_DPAD_LEFT;
  if (xpad_state[device].Gamepad.sThumbLX >  XPAD_DEADZONE) b |= XPAD_DPAD_RIGHT;

  if (xpad_state[device].Gamepad.bLeftTrigger  > XPAD_TRIGGER_THRESHOLD) b |= XPAD_LEFT_TRIGGER;
  if (xpad_state[device].Gamepad.bRightTrigger > XPAD_TRIGGER_THRESHOLD) b |= XPAD_RIGHT_TRIGGER;

  return b;
}

/*
 * Returns the mapping to use for a given emulated device slot.
 *
 * Slots 0 and 1 are the two configurable players. Higher slots only exist
 * with a multitap; those get the default layout on the matching XInput pad so
 * a four-player game works without extra setup.
 */
static void resolve_map(int slot, t_pad_map *out)
{
  if (slot < 2)
  {
    *out = gui.pad[slot];
  }
  else
  {
    *out = gui.pad[0];
    memset(out->key, 0, sizeof(out->key));
    out->device = (slot < XPAD_MAX_DEVICES) ? slot : -1;
  }
}

/* Collects one pad's state into a set of INPUT_* bits. */
static uint16 read_pad(int slot)
{
  t_pad_map map;
  DWORD buttons;
  uint16 pad = 0;

  resolve_map(slot, &map);
  buttons = gamepad_buttons(map.device);

  #define PRESSED(idx) \
    (key_down(map.key[idx]) || (map.button[idx] && (buttons & map.button[idx])))

  if (PRESSED(PAD_A))     pad |= INPUT_A;
  if (PRESSED(PAD_B))     pad |= INPUT_B;
  if (PRESSED(PAD_C))     pad |= INPUT_C;
  if (PRESSED(PAD_START)) pad |= INPUT_START;
  if (PRESSED(PAD_X))     pad |= INPUT_X;
  if (PRESSED(PAD_Y))     pad |= INPUT_Y;
  if (PRESSED(PAD_Z))     pad |= INPUT_Z;
  if (PRESSED(PAD_MODE))  pad |= INPUT_MODE;

  /* Opposite directions cannot be held on real hardware. */
  if (PRESSED(PAD_UP))         pad |= INPUT_UP;
  else if (PRESSED(PAD_DOWN))  pad |= INPUT_DOWN;

  if (PRESSED(PAD_LEFT))       pad |= INPUT_LEFT;
  else if (PRESSED(PAD_RIGHT)) pad |= INPUT_RIGHT;

  #undef PRESSED

  return pad;
}

/* Cursor position expressed in emulated pixels, clamped to the frame. */
static int cursor_in_frame(int *px, int *py)
{
  POINT pt;
  RECT dest;
  int sw, sh;
  int dw, dh;

  if (!has_focus) return 0;
  if (!GetCursorPos(&pt)) return 0;
  if (!ScreenToClient(g_hwnd, &pt)) return 0;

  video_get_output_rect(&dest, &sw, &sh);

  dw = dest.right - dest.left;
  dh = dest.bottom - dest.top;
  if (dw < 1 || dh < 1) return 0;

  *px = ((pt.x - dest.left) * sw) / dw;
  *py = ((pt.y - dest.top)  * sh) / dh;

  if (*px < 0) *px = 0; else if (*px >= sw) *px = sw - 1;
  if (*py < 0) *py = 0; else if (*py >= sh) *py = sh - 1;

  return 1;
}

/*
 * Movement since the last frame, in emulated pixels.
 *
 * The Sega Mouse reports relative motion, not a position, so an absolute
 * offset from the centre of the frame would read as a steady drift towards
 * the cursor rather than as the cursor moving.
 */
static int cursor_delta(int *dx, int *dy)
{
  static int last_x, last_y;
  static int have_last;
  int x, y;

  *dx = 0;
  *dy = 0;

  if (!cursor_in_frame(&x, &y))
  {
    have_last = 0;
    return 0;
  }

  if (have_last)
  {
    *dx = x - last_x;
    *dy = y - last_y;
  }

  last_x = x;
  last_y = y;
  have_last = 1;

  return 1;
}

static int mouse_button(int vk_button)
{
  if (!has_focus) return 0;
  return (GetAsyncKeyState(vk_button) & 0x8000) != 0;
}

/****************************************************************************
 * Per-frame update, called by the core
 ****************************************************************************/

int win32_input_update(void)
{
  int slot;
  int sw, sh;
  RECT dest;

  poll_gamepads();
  video_get_output_rect(&dest, &sw, &sh);

  for (slot = 0; slot < MAX_DEVICES; slot++)
  {
    int x = 0, y = 0;

    input.pad[slot] = 0;

    switch (input.dev[slot])
    {
      case NO_DEVICE:
        break;

      case DEVICE_MOUSE:
      {
        int dx, dy;

        if (!cursor_delta(&dx, &dy)) break;

        /* Y is reported bottom-up by the Sega Mouse. */
        input.analog[slot][0] = dx;
        input.analog[slot][1] = config.invert_mouse ? dy : -dy;

        if (mouse_button(VK_LBUTTON))  input.pad[slot] |= INPUT_MOUSE_LEFT;
        if (mouse_button(VK_RBUTTON))  input.pad[slot] |= INPUT_MOUSE_RIGHT;
        if (mouse_button(VK_MBUTTON))  input.pad[slot] |= INPUT_MOUSE_CENTER;
        if (key_down(gui.pad[0].key[PAD_START])) input.pad[slot] |= INPUT_START;
        break;
      }

      case DEVICE_LIGHTGUN:
      {
        if (!cursor_in_frame(&x, &y)) break;
        input.analog[slot][0] = x;
        input.analog[slot][1] = y;
        if (mouse_button(VK_LBUTTON)) input.pad[slot] |= INPUT_A;
        if (mouse_button(VK_RBUTTON)) input.pad[slot] |= INPUT_B;
        if (mouse_button(VK_MBUTTON)) input.pad[slot] |= INPUT_C;
        if (key_down(gui.pad[0].key[PAD_START])) input.pad[slot] |= INPUT_START;
        break;
      }

      case DEVICE_PADDLE:
      {
        if (cursor_in_frame(&x, &y) && sw > 0)
        {
          input.analog[slot][0] = (x * 256) / sw;
        }
        input.pad[slot] = read_pad(slot) & (INPUT_BUTTON1 | INPUT_BUTTON2 | INPUT_START);
        break;
      }

      case DEVICE_SPORTSPAD:
      {
        if (cursor_in_frame(&x, &y) && sw > 0 && sh > 0)
        {
          input.analog[slot][0] = (x * 256) / sw;
          input.analog[slot][1] = (y * 256) / sh;
        }
        input.pad[slot] = read_pad(slot) & (INPUT_BUTTON1 | INPUT_BUTTON2);
        break;
      }

      case DEVICE_PICO:
      {
        if (cursor_in_frame(&x, &y) && sw > 0 && sh > 0)
        {
          input.analog[0][0] = 0x03c + ((x * (0x17c - 0x03c + 1)) / sw);
          input.analog[0][1] = 0x1fc + ((y * (0x2f7 - 0x1fc + 1)) / sh);
        }
        if (mouse_button(VK_LBUTTON)) input.pad[slot] |= INPUT_PICO_PEN;
        if (mouse_button(VK_RBUTTON)) input.pad[slot] |= INPUT_PICO_RED;
        break;
      }

      case DEVICE_TEREBI:
      {
        if (cursor_in_frame(&x, &y) && sw > 0 && sh > 0)
        {
          input.analog[0][0] = (x * 250) / sw;
          input.analog[0][1] = (y * 250) / sh;
        }
        if (mouse_button(VK_LBUTTON)) input.pad[slot] |= INPUT_B;
        break;
      }

      case DEVICE_GRAPHIC_BOARD:
      {
        if (cursor_in_frame(&x, &y) && sw > 0 && sh > 0)
        {
          input.analog[0][0] = (x * 255) / sw;
          input.analog[0][1] = (y * 255) / sh;
        }
        if (mouse_button(VK_LBUTTON)) input.pad[slot] |= INPUT_GRAPHIC_PEN;
        if (mouse_button(VK_RBUTTON)) input.pad[slot] |= INPUT_GRAPHIC_MENU;
        if (mouse_button(VK_MBUTTON)) input.pad[slot] |= INPUT_GRAPHIC_DO;
        break;
      }

      case DEVICE_XE_1AP:
      {
        uint16 pad = read_pad(slot);
        t_pad_map map;
        DWORD buttons;

        resolve_map(slot, &map);
        buttons = gamepad_buttons(map.device);

        if (pad & INPUT_A)     input.pad[slot] |= INPUT_XE_A;
        if (pad & INPUT_B)     input.pad[slot] |= INPUT_XE_B;
        if (pad & INPUT_C)     input.pad[slot] |= INPUT_XE_C;
        if (pad & INPUT_X)     input.pad[slot] |= INPUT_XE_D;
        if (pad & INPUT_START) input.pad[slot] |= INPUT_XE_START;
        if (pad & INPUT_MODE)  input.pad[slot] |= INPUT_XE_SELECT;

        /* Left stick drives the analog axes, d-pad falls back to the extremes. */
        if (map.device >= 0 && map.device < XPAD_MAX_DEVICES && xpad_connected[map.device])
        {
          input.analog[slot][0] = 128 + (xpad_state[map.device].Gamepad.sThumbLX >> 9);
          input.analog[slot][1] = 128 - (xpad_state[map.device].Gamepad.sThumbLY >> 9);
        }
        else
        {
          input.analog[slot][0] = (pad & INPUT_LEFT) ? 0 : ((pad & INPUT_RIGHT) ? 255 : 128);
          input.analog[slot][1] = (pad & INPUT_UP)   ? 0 : ((pad & INPUT_DOWN)  ? 255 : 128);
        }

        input.analog[slot + 1][0] = (buttons & XPAD_LEFT_SHOULDER) ? 0 : 128;
        input.analog[slot + 1][1] = 128;

        if (input.analog[slot][0] < 0)   input.analog[slot][0] = 0;
        if (input.analog[slot][0] > 255) input.analog[slot][0] = 255;
        if (input.analog[slot][1] < 0)   input.analog[slot][1] = 0;
        if (input.analog[slot][1] > 255) input.analog[slot][1] = 255;
        break;
      }

      case DEVICE_ACTIVATOR:
      {
        if (key_down('G')) input.pad[slot] |= INPUT_ACTIVATOR_7L;
        if (key_down('H')) input.pad[slot] |= INPUT_ACTIVATOR_7U;
        if (key_down('J')) input.pad[slot] |= INPUT_ACTIVATOR_8L;
        if (key_down('K')) input.pad[slot] |= INPUT_ACTIVATOR_8U;
        break;
      }

      case DEVICE_SMASH:
      {
        if (key_down(VK_NUMPAD9)) input.pad[slot] |= INPUT_SMASH_UP_RIGHT;
        if (key_down(VK_NUMPAD8)) input.pad[slot] |= INPUT_SMASH_UP;
        if (key_down(VK_NUMPAD7)) input.pad[slot] |= INPUT_SMASH_UP_LEFT;
        if (key_down(VK_NUMPAD6)) input.pad[slot] |= INPUT_SMASH_RIGHT;
        if (key_down(VK_NUMPAD5)) input.pad[slot] |= INPUT_SMASH_CENTER;
        if (key_down(VK_NUMPAD4)) input.pad[slot] |= INPUT_SMASH_LEFT;
        if (key_down(VK_NUMPAD3)) input.pad[slot] |= INPUT_SMASH_DOWN_RIGHT;
        if (key_down(VK_NUMPAD2)) input.pad[slot] |= INPUT_SMASH_DOWN;
        if (key_down(VK_NUMPAD1)) input.pad[slot] |= INPUT_SMASH_DOWN_LEFT;
        break;
      }

      default:
        input.pad[slot] = read_pad(slot);
        break;
    }
  }

  return 1;
}

/****************************************************************************
 * Host shortcuts polled outside the core
 ****************************************************************************/

/* Independent of xpad_state[]/poll_gamepads() on purpose -- that cache is
   only refreshed once per forward emulated frame (inside
   win32_input_update()), which the rewind path deliberately never calls.
   A hotkey checked against that cache while rewinding would read
   whatever button state happened to be true the instant rewind started,
   forever, regardless of what's actually being pressed right now. This
   does its own fresh XInput read every single call instead, the same
   way input_capture_gamepad() already does for the "listening" dialog. */
static int gamepad_button_held_now(int device, int mask)
{
  XPAD_STATE st;
  DWORD b;

  if (!mask || !xinput_get_state) return 0;
  if (device < 0 || device >= XPAD_MAX_DEVICES) return 0;

  ZeroMemory(&st, sizeof(st));
  if (xinput_get_state((DWORD)device, &st) != ERROR_SUCCESS) return 0;

  b = st.Gamepad.wButtons;
  if (st.Gamepad.sThumbLY >  XPAD_DEADZONE) b |= XPAD_DPAD_UP;
  if (st.Gamepad.sThumbLY < -XPAD_DEADZONE) b |= XPAD_DPAD_DOWN;
  if (st.Gamepad.sThumbLX < -XPAD_DEADZONE) b |= XPAD_DPAD_LEFT;
  if (st.Gamepad.sThumbLX >  XPAD_DEADZONE) b |= XPAD_DPAD_RIGHT;
  if (st.Gamepad.bLeftTrigger  > XPAD_TRIGGER_THRESHOLD) b |= XPAD_LEFT_TRIGGER;
  if (st.Gamepad.bRightTrigger > XPAD_TRIGGER_THRESHOLD) b |= XPAD_RIGHT_TRIGGER;

  return (b & (DWORD)mask) != 0;
}

int gui_input_fast_forward(void)
{
  /* Deliberately absent from the accelerator table so it reaches us here
     regardless of which key it's mapped to. */
  if (gui.key_fast_forward && key_down(gui.key_fast_forward)) return 1;
  if (gamepad_button_held_now(gui.pad[0].device, gui.pad_fast_forward)) return 1;
  return 0;
}

int gui_input_rewind(void)
{
  if (gui.key_rewind && key_down(gui.key_rewind)) return 1;
  if (gamepad_button_held_now(gui.pad[0].device, gui.pad_rewind)) return 1;
  return 0;
}

int gui_input_frame_advance(void)
{
  int down = key_down(VK_OEM_5);   /* backslash */

  if (down && !frame_advance_armed)
  {
    frame_advance_armed = 1;
    return 1;
  }
  if (!down) frame_advance_armed = 0;

  return 0;
}

static int save_slot_armed[10];
static int load_slot_armed[10];

/* Returns the slot 0-9 whose Shift+digit was just pressed, or -1. */
int gui_input_save_slot_shortcut(void)
{
  int i;

  for (i = 0; i <= 9; i++)
  {
    int down = key_down('0' + i) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);

    if (down && !save_slot_armed[i])
    {
      save_slot_armed[i] = 1;
      load_slot_armed[i] = 1;   /* same physical key is still down -- don't let releasing Shift a moment early re-trigger this as a plain-digit load */
      return i;
    }
    if (!down) save_slot_armed[i] = 0;
  }

  return -1;
}

/* Returns the slot 0-9 whose plain digit was just pressed, or -1.
   Shift held excludes this so the same key press can't register as
   both a save and a load. */
int gui_input_load_slot_shortcut(void)
{
  int i;

  for (i = 0; i <= 9; i++)
  {
    int key_is_down = key_down('0' + i);
    int shift_down = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    if (key_is_down && !shift_down && !load_slot_armed[i])
    {
      load_slot_armed[i] = 1;
      return i;
    }
    /* Cleared only once the physical key itself comes up -- not merely
       when Shift changes -- so a Shift release that happens a moment
       before the digit key's own release can't be misread as a fresh,
       separate plain-digit press of the same still-held key. */
    if (!key_is_down) load_slot_armed[i] = 0;
  }

  return -1;
}

static int fullscreen_menu_click_armed;

int gui_input_fullscreen_menu_click(void)
{
  /* Polled instead of handled via WM_LBUTTONDOWN/WM_RBUTTONDOWN: confirmed
     by direct, repeated testing that once a menu is attached to the
     window (SetMenu), a second click's button-down message simply never
     arrives at the window procedure at all, no matter what happens in
     between -- not a redraw problem, a message-delivery one.
     GetAsyncKeyState reads the button state directly from the OS,
     independent of whatever is interfering with message delivery. */
  int down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

  if (down && !fullscreen_menu_click_armed)
  {
    fullscreen_menu_click_armed = 1;

    /* GetAsyncKeyState has no idea which window, if any, was actually
       clicked -- without this check, a right-click anywhere on screen,
       in any application, would show/hide this app's menu bar even
       while it's in the background. */
    if (GetForegroundWindow() == g_hwnd) return 1;
    return 0;
  }
  if (!down) fullscreen_menu_click_armed = 0;

  return 0;
}

/****************************************************************************
 * Capture helpers for the configuration dialog
 *
 * These read the raw key state directly instead of going through key_down():
 * while the dialog is up the main window has lost focus, which is exactly
 * when the emulated pad should go quiet but capture should still work.
 ****************************************************************************/

int input_capture_key(void)
{
  int vk;

  for (vk = 1; vk < 256; vk++)
  {
    switch (vk)
    {
      /* Mouse buttons belong to the pointer devices, Esc cancels. */
      case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON:
      case VK_XBUTTON1: case VK_XBUTTON2:
      case VK_ESCAPE:
        continue;
      default:
        break;
    }

    if (GetAsyncKeyState(vk) & 0x8000) return vk;
  }

  return 0;
}

int input_capture_gamepad(int device)
{
  static const int masks[] =
  {
    XPAD_DPAD_UP, XPAD_DPAD_DOWN, XPAD_DPAD_LEFT, XPAD_DPAD_RIGHT,
    XPAD_START, XPAD_BACK, XPAD_LEFT_SHOULDER, XPAD_RIGHT_SHOULDER,
    XPAD_A, XPAD_B, XPAD_X, XPAD_Y, XPAD_LEFT_TRIGGER, XPAD_RIGHT_TRIGGER
  };

  XPAD_STATE st;
  DWORD b;
  int i;

  if (!xinput_get_state) return 0;
  if (device < 0 || device >= XPAD_MAX_DEVICES) return 0;

  ZeroMemory(&st, sizeof(st));
  if (xinput_get_state((DWORD)device, &st) != ERROR_SUCCESS) return 0;

  b = st.Gamepad.wButtons;
  if (st.Gamepad.sThumbLY >  XPAD_DEADZONE) b |= XPAD_DPAD_UP;
  if (st.Gamepad.sThumbLY < -XPAD_DEADZONE) b |= XPAD_DPAD_DOWN;
  if (st.Gamepad.sThumbLX < -XPAD_DEADZONE) b |= XPAD_DPAD_LEFT;
  if (st.Gamepad.sThumbLX >  XPAD_DEADZONE) b |= XPAD_DPAD_RIGHT;
  if (st.Gamepad.bLeftTrigger  > XPAD_TRIGGER_THRESHOLD) b |= XPAD_LEFT_TRIGGER;
  if (st.Gamepad.bRightTrigger > XPAD_TRIGGER_THRESHOLD) b |= XPAD_RIGHT_TRIGGER;

  for (i = 0; i < (int)(sizeof(masks) / sizeof(masks[0])); i++)
  {
    if (b & masks[i]) return masks[i];
  }

  return 0;
}

int input_any_input_down(int device)
{
  if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return 1;
  if (input_capture_key()) return 1;
  if (input_capture_gamepad(device)) return 1;
  return 0;
}

/****************************************************************************
 * Names for the configuration dialog
 ****************************************************************************/

const char *input_key_name(int vk)
{
  static char name[64];
  UINT scan;
  LONG lparam;

  if (!vk) return "(unassigned)";

  scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
  if (!scan) { wsprintfA(name, "Key %d", vk); return name; }

  lparam = (LONG)(scan << 16);

  switch (vk)
  {
    /* Keys on the grey block report the same scan code as the numpad. */
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
      lparam |= (1 << 24);
      break;
    default:
      break;
  }

  if (GetKeyNameTextA(lparam, name, sizeof(name)) > 0) return name;

  wsprintfA(name, "Key %d", vk);
  return name;
}

const char *input_pad_button_name(int mask)
{
  static const struct { int mask; const char *name; } names[] =
  {
    { XPAD_DPAD_UP,        "D-pad Up"    },
    { XPAD_DPAD_DOWN,      "D-pad Down"  },
    { XPAD_DPAD_LEFT,      "D-pad Left"  },
    { XPAD_DPAD_RIGHT,     "D-pad Right" },
    { XPAD_START,          "Start button"  },
    { XPAD_BACK,           "Back button"   },
    { XPAD_LEFT_SHOULDER,  "LB button"   },
    { XPAD_RIGHT_SHOULDER, "RB button"   },
    { XPAD_A,              "A button"    },
    { XPAD_B,              "B button"    },
    { XPAD_X,              "X button"    },
    { XPAD_Y,              "Y button"    },
    { XPAD_LEFT_TRIGGER,   "LT button"   },
    { XPAD_RIGHT_TRIGGER,  "RT button"   },
  };
  static char name[32];
  int i;

  if (!mask) return "(unassigned)";

  for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
  {
    if (names[i].mask == mask) return names[i].name;
  }

  /* Should not happen for anything this app itself ever assigns, but
     falls back to the raw value rather than showing nothing if it does. */
  wsprintfA(name, "pad 0x%05X", mask);
  return name;
}

const char *input_button_label(int index)
{
  static const char *labels[PAD_KEYS] =
  {
    "Up", "Down", "Left", "Right",
    "A", "B", "C",
    "X", "Y", "Z", "Start", "Mode"
  };

  if (index < 0 || index >= PAD_KEYS) return "?";
  return labels[index];
}
