/****************************************************************************
 *  Genesis Plus GX -- Win32 GUI frontend
 *
 *  main.c -- window, menu handling, ROM lifecycle and the emulation loop.
 *
 *  Emulation runs on the UI thread from a PeekMessage loop rather than on a
 *  worker thread. That trades a little responsiveness while a menu is open
 *  (Windows runs its own modal loop there, which we handle explicitly) for
 *  having no locking anywhere around the framebuffer or the core's state.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>

#include "shared.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"

/* Required by main.h / the core. */
int log_error = 0;
int debug_on  = 0;

HWND      g_hwnd;
HWND      g_status;
HINSTANCE g_inst;

int emu_running;
int emu_paused;

/* Shared UI font -- the normal system default everywhere, or (when
   gui.large_ui is on) a version scaled up ~17.5% -- the middle of the
   "at least 50%" scale asked for, applied uniformly rather than
   guessing a different amount per control. Created once, lazily, and
   kept for the app's lifetime rather than per-control, since every
   caller wants the exact same font. */
HFONT gui_get_ui_font(void)
{
  static HFONT large_font;
  HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

  if (!gui.large_ui) return stock;

  if (!large_font)
  {
    LOGFONTA lf;
    GetObjectA(stock, sizeof(lf), &lf);
    /* lfHeight is conventionally negative (character height, not cell
       height) -- scaling while preserving sign keeps this correct
       regardless of which convention the stock font happens to use. */
    lf.lfHeight = (LONG)(lf.lfHeight * 1.5);
    large_font = CreateFontIndirectA(&lf);
    if (!large_font) large_font = stock;
  }

  return large_font;
}

/* Set only when emu_paused was switched on by losing focus, not by the
   person pressing pause themselves -- distinguishes "resume automatically
   when focus comes back" from "leave a deliberate pause alone". */
static int auto_paused_by_focus;
static int auto_paused_by_minimize;

static HACCEL g_accel;
static HMENU  g_menu;

static char rom_path[GUI_PATH_LEN];

/*
 * Filename without directory or extension. Deliberately much shorter than a
 * full path: it gets formatted into window titles, status text and save-file
 * names, and wvsprintf has a hard 1024-byte output limit.
 */
static char rom_base[128];

static int16 soundframe[4096];

static int   in_modal_loop;
static int   frames_this_second;
static DWORD fps_tick;
static LARGE_INTEGER perf_freq;
static double next_frame_time;
static double next_rewind_time;
static int    rewind_hit_limit_notified;

/* Mega CD backup RAM header, used to tell formatted RAM from blank RAM. */
static uint8 brm_format[0x40] =
{
  0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x5f,0x00,0x00,0x00,0x00,0x40,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x53,0x45,0x47,0x41,0x5f,0x43,0x44,0x5f,0x52,0x4f,0x4d,0x00,0x01,0x00,0x00,0x00,
  0x52,0x41,0x4d,0x5f,0x43,0x41,0x52,0x54,0x52,0x49,0x44,0x47,0x45,0x5f,0x5f,0x5f
};

static const uint16 vc_table[4][2] =
{
  /* NTSC, PAL */
  {0xDA , 0xF2},  /* Mode 4 (192 lines) */
  {0xEA , 0x102}, /* Mode 5 (224 lines) */
  {0xDA , 0xF2},  /* Mode 4 (192 lines) */
  {0x106, 0x10A}  /* Mode 5 (240 lines) */
};

/****************************************************************************
 * Paths
 ****************************************************************************/

char *osd_path(const char *filename)
{
  /*
   * Rotating buffers: the core occasionally has one path in flight while
   * building the next, and a single static buffer would corrupt both.
   */
  static char slots[4][GUI_PATH_LEN];
  static int  next;
  static char base[GUI_PATH_LEN];
  static int  base_ready;

  char *out = slots[next];
  next = (next + 1) & 3;

  if (!base_ready)
  {
    char *slash;
    DWORD len = GetModuleFileNameA(NULL, base, sizeof(base) - 1);

    if (len == 0 || len >= sizeof(base) - 1)
    {
      base[0] = '.';
      base[1] = '\0';
    }
    else
    {
      slash = strrchr(base, '\\');
      if (slash) *slash = '\0';
      else lstrcpyA(base, ".");
    }
    base_ready = 1;
  }

  wsprintfA(out, "%s\\%s", base, filename);
  return out;
}

static void ensure_dir(const char *name)
{
  CreateDirectoryA(osd_path(name), NULL);
}

static void split_basename(const char *path, char *out, int out_len)
{
  const char *start = path;
  const char *slash = strrchr(path, '\\');
  const char *fwd   = strrchr(path, '/');
  const char *dot;
  int n;

  if (fwd > slash) slash = fwd;
  if (slash) start = slash + 1;

  dot = strrchr(start, '.');
  n = dot ? (int)(dot - start) : (int)strlen(start);
  if (n >= out_len) n = out_len - 1;

  memcpy(out, start, (size_t)n);
  out[n] = '\0';
}

/****************************************************************************
 * Status bar and notices
 ****************************************************************************/

static char status_persistent[128];
static DWORD status_transient_until;

/* The baseline message pane 0 should show once any transient message
   above it clears -- "Running <game>" or the browsing prompt. */
void gui_status_persistent(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(status_persistent, fmt, ap);
  va_end(ap);

  status_transient_until = 0;
  if (g_status) SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)status_persistent);
}

void gui_status(const char *fmt, ...)
{
  char buf[1024];
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(buf, fmt, ap);
  va_end(ap);

  /* Transient -- reverts to the persistent baseline on its own after a
     few seconds instead of sitting there until something else happens
     to overwrite it, which was the actual bug: there was no revert path
     at all, so whatever the last one-off message was (e.g. "Audio
     settings saved") just stayed there indefinitely. */
  status_transient_until = GetTickCount() + 4000;
  if (g_status) SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)buf);
}

void gui_notify(const char *fmt, ...)
{
  char buf[256];
  va_list ap;

  va_start(ap, fmt);
  wvsprintfA(buf, fmt, ap);
  va_end(ap);

  video_show_notice(buf, 1800);
  gui_status("%s", buf);
}

static void status_set_slot(void)
{
  char buf[32];
  wsprintfA(buf, "Slot %d", gui.state_slot);
  if (g_status) SendMessageA(g_status, SB_SETTEXTA, 1, (LPARAM)buf);
}

/* Lets the browser panel show the game count in the same pane Slot N
   normally occupies -- the slot number means nothing without a ROM
   running anyway, so the two never need the space at the same time. */
void gui_status_slot(const char *text)
{
  if (g_status) SendMessageA(g_status, SB_SETTEXTA, 1, (LPARAM)text);
}

static void status_set_fps(int fps)
{
  static char last_buf[32];
  char buf[32];

  if (emu_running && !emu_paused) wsprintfA(buf, "%d fps", fps);
  else lstrcpyA(buf, emu_running ? "Paused" : "");

  if (lstrcmpA(buf, last_buf) == 0) return;
  lstrcpyA(last_buf, buf);

  if (g_status) SendMessageA(g_status, SB_SETTEXTA, 2, (LPARAM)buf);
}

/*
 * Remembers where the window is so it can reopen there next time. Skipped
 * during fullscreen (that geometry is a full-monitor rect, not a window
 * position) and while minimized (GetWindowRect would just capture the
 * taskbar-parked position, overwriting the last real one for no reason).
 */
static void capture_window_geometry(void)
{
  if (gui.fullscreen) return;
  if (IsIconic(g_hwnd)) return;

  gui.window_maximized = IsZoomed(g_hwnd) ? 1 : 0;

  if (!gui.window_maximized)
  {
    RECT r;
    GetWindowRect(g_hwnd, &r);
    gui.window_x = r.left;
    gui.window_y = r.top;
  }
}

static void layout_status(void)
{
  RECT rc;
  int parts[3];

  if (!g_status) return;

  SendMessage(g_status, WM_SIZE, 0, 0);
  GetClientRect(g_hwnd, &rc);

  /* Middle pane widened from its original "Slot N"-only sizing -- it now
     also carries the browser panel's game count, which runs noticeably
     longer ("15 of 934 games", "999+ games found (list capped)"). */
  parts[0] = rc.right - 220;
  parts[1] = rc.right - 70;
  parts[2] = -1;
  if (parts[0] < 0) parts[0] = 0;
  if (parts[1] < parts[0]) parts[1] = parts[0];

  SendMessage(g_status, SB_SETPARTS, 3, (LPARAM)parts);
}

/* Same area video.c's present() renders into -- client area minus the
   status bar, when it's visible. The browser panel occupies exactly this
   region so nothing black shows through around its edges. */
static void get_content_rect(RECT *out)
{
  GetClientRect(g_hwnd, out);

  if (g_status && IsWindowVisible(g_status))
  {
    RECT sb;
    GetWindowRect(g_status, &sb);
    out->bottom -= (sb.bottom - sb.top);
    if (out->bottom < out->top) out->bottom = out->top;
  }
}

/* Lets browser.c (which can't see the static get_content_rect above)
   trigger a relayout of its own controls -- needed after picking a folder
   from the "Choose ROM Folder" button, which switches the panel from that
   button to the normal search+list view without a window resize to
   otherwise trigger it. */
void browser_relayout(void)
{
  RECT content;
  get_content_rect(&content);
  browser_panel_layout(&content);
}

/****************************************************************************
 * Window title
 ****************************************************************************/

static void update_title(void)
{
  char title[512];

  if (emu_running)
  {
    const char *name = (rominfo.international[0] != 0x20 && rominfo.international[0])
                       ? rominfo.international : rominfo.domestic;

    if (name && name[0] && name[0] != 0x20)
    {
      char trimmed[64];
      int i;
      lstrcpynA(trimmed, name, sizeof(trimmed));
      for (i = lstrlenA(trimmed) - 1; i >= 0 && trimmed[i] == ' '; i--) trimmed[i] = '\0';
      wsprintfA(title, "%s - " APP_NAME, trimmed[0] ? trimmed : rom_base);
    }
    else
    {
      wsprintfA(title, "%s - " APP_NAME, rom_base);
    }
  }
  else
  {
    lstrcpyA(title, APP_NAME);
  }

  SetWindowTextA(g_hwnd, title);
}

/****************************************************************************
 * Menu state
 ****************************************************************************/

static void check_radio(HMENU menu, int base, int count, int selected)
{
  CheckMenuRadioItem(menu, base, base + count - 1, base + selected, MF_BYCOMMAND);
}

static void rebuild_recent_menu(void)
{
  HMENU file_menu = GetSubMenu(g_menu, 0);
  /* Position 2: Open ROM (0), ROM Browser (1), Open Recent (2). */
  HMENU recent = GetSubMenu(file_menu, 2);
  int i, added = 0;

  if (!recent) return;

  while (GetMenuItemCount(recent) > 0) DeleteMenu(recent, 0, MF_BYPOSITION);

  for (i = 0; i < GUI_RECENT_MAX; i++)
  {
    char label[GUI_PATH_LEN + 8];
    char name[GUI_PATH_LEN];

    if (!gui.recent[i][0]) continue;

    split_basename(gui.recent[i], name, sizeof(name));
    wsprintfA(label, "&%d  %s", added + 1, name);
    AppendMenuA(recent, MF_STRING, (UINT_PTR)(IDM_FILE_RECENT_BASE + i), label);
    added++;
  }

  if (!added)
  {
    AppendMenuA(recent, MF_STRING | MF_GRAYED, (UINT_PTR)IDM_FILE_RECENT_BASE, "(nothing yet)");
  }
  else
  {
    AppendMenuA(recent, MF_SEPARATOR, 0, NULL);
    AppendMenuA(recent, MF_STRING, (UINT_PTR)IDM_FILE_RECENT_CLEAR, "&Clear this list");
  }

  if (gui.large_ui) theme_ownerdraw_menu(recent);

  DrawMenuBar(g_hwnd);
}

/* Room reserved for filter entries: IDM_VIDEO_FILTER_BASE .. +23. The menu
   command for entry i is IDM_VIDEO_FILTER_BASE + i, where i is the filter's
   index in filters.c's table. */
#define FILTER_MENU_MAX 24

/* Finds the "Video" top-level menu by what's actually in it (a known,
   always-direct-child item) rather than by position -- a hardcoded
   position here is exactly the same fragility find_render_filter_menu
   already works around one level down, and broke the same way when
   the View menu was inserted before Video, shifting its position. */
static HMENU find_video_menu(void)
{
  int i, count = GetMenuItemCount(g_menu);

  for (i = 0; i < count; i++)
  {
    HMENU sub = GetSubMenu(g_menu, i);
    if (sub && GetMenuState(sub, IDM_VIDEO_FULLSCREEN, MF_BYCOMMAND) != (UINT)-1) return sub;
  }
  return NULL;
}

/* Finds "Render Filter" by what's actually in it (its first item's command
   ID never changes) rather than by position -- a hardcoded position here
   broke silently the last time an item got added above it in the Video
   menu, and rebuilt the wrong submenu's contents without any error. */
static HMENU find_render_filter_menu(HMENU video_menu)
{
  int i, count = GetMenuItemCount(video_menu);

  for (i = 0; i < count; i++)
  {
    HMENU sub = GetSubMenu(video_menu, i);
    if (sub && GetMenuItemID(sub, 0) == (UINT)IDM_VIDEO_FILTER_NONE) return sub;
  }
  return NULL;
}

static void rebuild_filter_menu(void)
{
  HMENU video_menu = find_video_menu();
  HMENU filters = video_menu ? find_render_filter_menu(video_menu) : NULL;
  int current, count, i;

  if (!filters) return;

  while (GetMenuItemCount(filters) > 0) DeleteMenu(filters, 0, MF_BYPOSITION);

  current = video_filter_current();
  count   = video_filter_count();
  if (count > FILTER_MENU_MAX) count = FILTER_MENU_MAX;

  AppendMenuA(filters, MF_STRING, (UINT_PTR)IDM_VIDEO_FILTER_NONE, "None");

  if (count > 0)
  {
    AppendMenuA(filters, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < count; i++)
    {
      if (i > 0 && video_filter_separator_before(i)) AppendMenuA(filters, MF_SEPARATOR, 0, NULL);
      AppendMenuA(filters, MF_STRING, (UINT_PTR)(IDM_VIDEO_FILTER_BASE + i), video_filter_name(i));
    }
  }

  if (current < 0 || current >= count)
  {
    CheckMenuItem(filters, IDM_VIDEO_FILTER_NONE, MF_BYCOMMAND | MF_CHECKED);
  }
  else
  {
    CheckMenuItem(filters, (UINT)(IDM_VIDEO_FILTER_BASE + current), MF_BYCOMMAND | MF_CHECKED);
  }

  if (gui.large_ui) theme_ownerdraw_menu(filters);

  DrawMenuBar(g_hwnd);
}

static int system_menu_index(void)
{
  switch (config.system)
  {
    case SYSTEM_SG:           return 1;
    case SYSTEM_SGII:         return 2;
    case SYSTEM_SGII_RAM_EXT: return 3;
    case SYSTEM_MARKIII:      return 4;
    case SYSTEM_SMS:          return 5;
    case SYSTEM_SMS2:         return 6;
    case SYSTEM_GG:           return 7;
    case SYSTEM_MD:           return 8;
    default:                  return 0;
  }
}

static int port_menu_index(int port)
{
  static const int port_a[] = { NO_SYSTEM, SYSTEM_GAMEPAD, SYSTEM_MOUSE, SYSTEM_XE_1AP,
                                SYSTEM_ACTIVATOR, SYSTEM_LIGHTPHASER, SYSTEM_PADDLE,
                                SYSTEM_SPORTSPAD, SYSTEM_GRAPHIC_BOARD, SYSTEM_TEAMPLAYER };
  static const int port_b[] = { NO_SYSTEM, SYSTEM_GAMEPAD, SYSTEM_MOUSE, SYSTEM_MENACER,
                                SYSTEM_JUSTIFIER, SYSTEM_XE_1AP, SYSTEM_ACTIVATOR,
                                SYSTEM_LIGHTPHASER, SYSTEM_PADDLE, SYSTEM_TEAMPLAYER };
  const int *table = port ? port_b : port_a;
  int i;

  for (i = 0; i < 10; i++)
  {
    if (table[i] == input.system[port]) return i;
  }
  return 1;
}

static int port_menu_value(int port, int index)
{
  static const int port_a[] = { NO_SYSTEM, SYSTEM_GAMEPAD, SYSTEM_MOUSE, SYSTEM_XE_1AP,
                                SYSTEM_ACTIVATOR, SYSTEM_LIGHTPHASER, SYSTEM_PADDLE,
                                SYSTEM_SPORTSPAD, SYSTEM_GRAPHIC_BOARD, SYSTEM_TEAMPLAYER };
  static const int port_b[] = { NO_SYSTEM, SYSTEM_GAMEPAD, SYSTEM_MOUSE, SYSTEM_MENACER,
                                SYSTEM_JUSTIFIER, SYSTEM_XE_1AP, SYSTEM_ACTIVATOR,
                                SYSTEM_LIGHTPHASER, SYSTEM_PADDLE, SYSTEM_TEAMPLAYER };

  if (index < 0 || index > 9) return SYSTEM_GAMEPAD;
  return port ? port_b[index] : port_a[index];
}

void gui_update_menu(void)
{
  UINT rom_state = emu_running ? MF_ENABLED : (MF_GRAYED | MF_DISABLED);
  UINT browsing_state = emu_running ? (MF_GRAYED | MF_DISABLED) : MF_ENABLED;

  if (!g_menu) return;

  EnableMenuItem(g_menu, IDM_VIEW_LIST, MF_BYCOMMAND | browsing_state);
  EnableMenuItem(g_menu, IDM_VIEW_GRID, MF_BYCOMMAND | browsing_state);
  EnableMenuItem(g_menu, IDM_VIDEO_LARGE_UI, MF_BYCOMMAND | browsing_state);

  EnableMenuItem(g_menu, IDM_FILE_CLOSE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_STOP, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_ROMINFO, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_SAVESTATE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_LOADSTATE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_STATEMGR, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_FILE_SCREENSHOT, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_PAUSE, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_RESET, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_HARDRESET, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_EMU_CHEATS, MF_BYCOMMAND | rom_state);
  EnableMenuItem(g_menu, IDM_VIDEO_FULLSCREEN, MF_BYCOMMAND | rom_state);

  CheckMenuItem(g_menu, IDM_EMU_PAUSE,
                MF_BYCOMMAND | (emu_paused ? MF_CHECKED : MF_UNCHECKED));

  check_radio(g_menu, IDM_FILE_SLOT_BASE, GUI_SLOT_MAX, gui.state_slot);
  check_radio(g_menu, IDM_EMU_REGION_BASE, 5, config.region_detect);
  check_radio(g_menu, IDM_EMU_SYSTEM_BASE, 9, system_menu_index());
  check_radio(g_menu, IDM_EMU_LOCKON_BASE, 4, config.lock_on);

  CheckMenuItem(g_menu, IDM_EMU_BIOS,
                MF_BYCOMMAND | (config.bios ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_EMU_ADDRERROR,
                MF_BYCOMMAND | (config.addr_error ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_EMU_PAUSE_UNFOCUSED,
                MF_BYCOMMAND | (gui.pause_on_focus_loss ? MF_CHECKED : MF_UNCHECKED));

  check_radio(g_menu, IDM_VIEW_LIST, 2, gui.browser_grid_view);
  check_radio(g_menu, IDM_VIDEO_SCALE_BASE, 4, gui.scale - 1);
  check_radio(g_menu, IDM_VIDEO_ASPECT_BASE, 3, gui.aspect);
  check_radio(g_menu, IDM_VIDEO_NTSC_BASE, 4, config.ntsc);
  check_radio(g_menu, IDM_VIDEO_OVERSCAN_BASE, 4, config.overscan);

  CheckMenuItem(g_menu, IDM_VIDEO_FULLSCREEN,
                MF_BYCOMMAND | (gui.fullscreen ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_FULLSCREEN_START,
                MF_BYCOMMAND | (gui.fullscreen_on_load ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_SMOOTH,
                MF_BYCOMMAND | (gui.smooth ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_HWACCEL,
                MF_BYCOMMAND | (gui.hw_accel ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_VSYNC,
                MF_BYCOMMAND | (gui.vsync ? MF_CHECKED : MF_UNCHECKED));
  check_radio(g_menu, IDM_VIDEO_SCANLINE_BASE, 5, gui.scanline_pct / 25);
  check_radio(g_menu, IDM_VIDEO_THEME_BASE, 3, theme_get_mode());
  CheckMenuItem(g_menu, IDM_VIDEO_LARGE_UI,
                MF_BYCOMMAND | (gui.large_ui ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_SHOWFPS,
                MF_BYCOMMAND | (gui.show_fps ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_INTERLACE,
                MF_BYCOMMAND | (config.render ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_VIDEO_GGEXTRA,
                MF_BYCOMMAND | (config.gg_extra ? MF_CHECKED : MF_UNCHECKED));

  CheckMenuItem(g_menu, IDM_AUDIO_ENABLE,
                MF_BYCOMMAND | (gui.sound_enabled ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_MONO,
                MF_BYCOMMAND | (config.mono ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_LOWPASS,
                MF_BYCOMMAND | (config.filter ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, IDM_AUDIO_HQPSG,
                MF_BYCOMMAND | (config.hq_psg ? MF_CHECKED : MF_UNCHECKED));

  check_radio(g_menu, IDM_AUDIO_FMCORE_BASE, 4,
              config.ym3438 ? 3 : ((config.ym2612 > 2) ? 2 : config.ym2612));
  check_radio(g_menu, IDM_AUDIO_RATE_BASE, 2, (gui.sample_rate == 44100) ? 0 : 1);

  check_radio(g_menu, IDM_INPUT_PORTA_BASE, 10, port_menu_index(0));
  check_radio(g_menu, IDM_INPUT_PORTB_BASE, 10, port_menu_index(1));

  status_set_slot();
}

/****************************************************************************
 * Window sizing
 ****************************************************************************/

void gui_resize_to_scale(int scale)
{
  RECT want;
  int w, h, sb = 0;

  if (gui.fullscreen) return;

  video_preferred_size(scale, &w, &h);

  if (g_status && IsWindowVisible(g_status))
  {
    RECT r;
    GetWindowRect(g_status, &r);
    sb = r.bottom - r.top;
  }

  want.left = 0;
  want.top = 0;
  want.right = w;
  want.bottom = h + sb;

  AdjustWindowRectEx(&want,
                     (DWORD)GetWindowLongPtr(g_hwnd, GWL_STYLE), TRUE,
                     (DWORD)GetWindowLongPtr(g_hwnd, GWL_EXSTYLE));

  SetWindowPos(g_hwnd, NULL, 0, 0,
               want.right - want.left, want.bottom - want.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/****************************************************************************
 * Save files
 ****************************************************************************/

static void save_backup_ram(void)
{
  FILE *fp;
  char path[GUI_PATH_LEN];

  if (!emu_running) return;

  ensure_dir("saves");

  if (system_hw == SYSTEM_MCD)
  {
    if (!memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
    {
      wsprintfA(path, "%s\\%s.brm", osd_path("saves"), rom_base);
      fp = fopen(path, "wb");
      if (fp) { fwrite(scd.bram, 0x2000, 1, fp); fclose(fp); }
    }

    if (scd.cartridge.id &&
        !memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
    {
      wsprintfA(path, "%s\\cart.brm", osd_path("saves"));
      fp = fopen(path, "wb");
      if (fp) { fwrite(scd.cartridge.area, scd.cartridge.mask + 1, 1, fp); fclose(fp); }
    }
  }

  if (sram.on)
  {
    wsprintfA(path, "%s\\%s.srm", osd_path("saves"), rom_base);
    fp = fopen(path, "wb");
    if (fp) { fwrite(sram.sram, 0x10000, 1, fp); fclose(fp); }
  }
}

static void load_backup_ram(void)
{
  FILE *fp;
  char path[GUI_PATH_LEN];

  if (system_hw == SYSTEM_MCD)
  {
    wsprintfA(path, "%s\\%s.brm", osd_path("saves"), rom_base);
    fp = fopen(path, "rb");
    if (fp) { fread(scd.bram, 0x2000, 1, fp); fclose(fp); }

    if (memcmp(scd.bram + 0x2000 - 0x20, brm_format + 0x20, 0x20))
    {
      memset(scd.bram, 0x00, 0x200);
      brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] = 0x00;
      brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] =
        (uint8)((sizeof(scd.bram) / 64) - 3);
      memcpy(scd.bram + 0x2000 - 0x40, brm_format, 0x40);
    }

    if (scd.cartridge.id)
    {
      wsprintfA(path, "%s\\cart.brm", osd_path("saves"));
      fp = fopen(path, "rb");
      if (fp) { fread(scd.cartridge.area, scd.cartridge.mask + 1, 1, fp); fclose(fp); }

      if (memcmp(scd.cartridge.area + scd.cartridge.mask + 1 - 0x20, brm_format + 0x20, 0x20))
      {
        memset(scd.cartridge.area, 0x00, scd.cartridge.mask + 1);
        brm_format[0x10] = brm_format[0x12] = brm_format[0x14] = brm_format[0x16] =
          (uint8)((((scd.cartridge.mask + 1) / 64) - 3) >> 8);
        brm_format[0x11] = brm_format[0x13] = brm_format[0x15] = brm_format[0x17] =
          (uint8)((((scd.cartridge.mask + 1) / 64) - 3) & 0xff);
        memcpy(scd.cartridge.area + scd.cartridge.mask + 1 - sizeof(brm_format),
               brm_format, sizeof(brm_format));
      }
    }
  }

  if (sram.on)
  {
    wsprintfA(path, "%s\\%s.srm", osd_path("saves"), rom_base);
    fp = fopen(path, "rb");
    if (fp) { fread(sram.sram, 0x10000, 1, fp); fclose(fp); }
  }
}

/* Persists the cheat list against whatever ROM the list belongs to: the one
   that is running, or the one that was only inspected (emu_peek_rom, from the
   browser's Edit Cheats). rom_base is set in both cases and cleared when a
   running game is closed, and the list is always the one loaded for it. */
void cheats_save_current(void)
{
  if (rom_base[0]) cheats_save_for_rom(rom_base);
}

/****************************************************************************
 * Save states
 ****************************************************************************/

void state_path(int slot, char *out, int out_len)
{
  wsprintfA(out, "%s\\%s.gp%d", osd_path("states"), rom_base, slot);
  (void)out_len;
}

void thumb_path(int slot, char *out, int out_len)
{
  wsprintfA(out, "%s\\%s.gp%d.bmp", osd_path("states"), rom_base, slot);
  (void)out_len;
}

void emu_save_state(int slot)
{
  char path[GUI_PATH_LEN];
  uint8 *buffer;
  FILE *fp;
  int len;

  if (!emu_running) return;

  buffer = (uint8 *)malloc(STATE_SIZE);
  if (!buffer) { gui_notify("Not enough memory to save the state"); return; }

  len = state_save(buffer);

  ensure_dir("states");
  state_path(slot, path, sizeof(path));

  fp = fopen(path, "wb");
  if (fp)
  {
    fwrite(buffer, (size_t)len, 1, fp);
    fclose(fp);
    thumb_path(slot, path, sizeof(path));
    video_save_thumbnail(path);
    gui_notify("Saved to slot %d", slot);
  }
  else
  {
    gui_notify("Could not write slot %d", slot);
  }

  free(buffer);
}

void emu_load_state(int slot)
{
  char path[GUI_PATH_LEN];
  uint8 *buffer;
  FILE *fp;
  size_t got;

  if (!emu_running) return;

  state_path(slot, path, sizeof(path));

  fp = fopen(path, "rb");
  if (!fp) { gui_notify("Slot %d is empty", slot); return; }

  buffer = (uint8 *)malloc(STATE_SIZE);
  if (!buffer) { fclose(fp); gui_notify("Not enough memory to load the state"); return; }

  got = fread(buffer, 1, STATE_SIZE, fp);
  fclose(fp);

  if (got > 0 && state_load(buffer))
  {
    gui_notify("Loaded slot %d", slot);
    waveout_flush();
    /* Banking was restored from the state, so 8-bit ROM patches now point at
       the wrong page. Rewrite them all. */
    cheats_apply();
    video_viewport_changed();
  }
  else
  {
    gui_notify("Slot %d was saved by a different version", slot);
  }

  free(buffer);
}

/****************************************************************************
 * Audio plumbing
 ****************************************************************************/

void emu_apply_audio_settings(void)
{
  if (!gui.sound_enabled)
  {
    waveout_close();
    return;
  }

  /* Only touch the device when the format actually changed -- reopening it
     on every ROM load produces an audible click for no reason. */
  if (waveout_rate() != gui.sample_rate)
  {
    waveout_close();

    if (!waveout_open(gui.sample_rate))
    {
      gui.sound_enabled = 0;
      gui_status("No audio device available, sound is off");
      return;
    }
  }

  waveout_set_volume(gui.volume);

  if (emu_running)
  {
    /* Only audio_init() -- it's the one that actually depends on the
       sample rate (it rebuilds the blip buffers to match). sound_init()
       does not: reading its implementation directly shows its behaviour
       depends purely on which FM core is configured (config.ym2612/
       config.ym3438), never on the sample rate. Calling it here anyway
       was unnecessarily wiping the FM/PSG chip's entire register/channel
       state on every Sample Rate or Enable Sound change -- silencing
       whatever instruments the running game had already set up and
       wasn't continuously re-sending, since nothing told the game itself
       anything had changed. That's what "missing instruments until a
       reset" was: not a timing bug, but real state being thrown away for
       a change that never needed to touch it. */
    audio_init(gui.sample_rate, 0);
  }
}

/****************************************************************************
 * Region changes
 ****************************************************************************/

void emu_apply_region(void)
{
  if (!emu_running) return;

  get_region(0);

  /* The frame rate moved, so the audio timing has to be rebuilt. */
  audio_init(snd.sample_rate, 0);

  if ((system_hw == SYSTEM_MCD) || ((system_hw & SYSTEM_SMS) && (config.bios & 1)))
  {
    system_init();
    system_reset();
  }
  else
  {
    if (system_hw == SYSTEM_MD)
    {
      io_reg[0x00] = (uint8)(0x20 | region_code | (config.bios & 1));
    }
    else
    {
      io_reg[0x00] = (uint8)(0x80 | (region_code >> 1));
    }

    if (vdp_pal)
    {
      status |= 1;
      lines_per_frame = 313;
    }
    else
    {
      status &= ~1;
      lines_per_frame = 262;
    }

    switch (bitmap.viewport.h)
    {
      case 192: vc_max = vc_table[0][vdp_pal]; break;
      case 224: vc_max = vc_table[1][vdp_pal]; break;
      case 240: vc_max = vc_table[3][vdp_pal]; break;
      default: break;
    }
  }

  cheats_apply();
  waveout_flush();
  video_viewport_changed();
}

/****************************************************************************
 * ROM lifecycle
 ****************************************************************************/

static void load_boot_rom(void)
{
  FILE *fp;

  system_bios = 0;
  memset(boot_rom, 0xFF, sizeof(boot_rom));

  fp = fopen(MD_BIOS, "rb");
  if (!fp) return;

  fread(boot_rom, 1, 0x800, fp);
  fclose(fp);

  if (!memcmp((char *)(boot_rom + 0x120), "GENESIS OS", 10))
  {
    int i;

    system_bios = SYSTEM_MD;

    for (i = 0; i < 0x800; i += 2)
    {
      uint8 temp = boot_rom[i];
      boot_rom[i] = boot_rom[i + 1];
      boot_rom[i + 1] = temp;
    }

    for (i = 0x800; i < 0x10000; i++)
    {
      boot_rom[i] = boot_rom[i & 0x7ff];
    }
  }
}

void emu_close_rom(void)
{
  if (!emu_running) return;

  cheats_save_current();
  cheats_suspend();          /* undo patches while cart.rom is still valid */
  cheats_remove_all();

  save_backup_ram();
  audio_shutdown();
  waveout_flush();

  emu_running = 0;
  emu_paused = 0;
  auto_paused_by_focus = 0;
  rewind_reset();
  rom_path[0] = '\0';
  rom_base[0] = '\0';

  update_title();
  gui_update_menu();
  gui_status_persistent("Open a ROM to start, or drop one on this window");

  {
    RECT content;
    get_content_rect(&content);
    browser_panel_layout(&content);
    browser_panel_show(1);
  }

  InvalidateRect(g_hwnd, NULL, TRUE);
}

int emu_load_rom(const char *path)
{
  char attempt[GUI_PATH_LEN];

  if (!path || !path[0]) return 0;

  lstrcpynA(attempt, path, sizeof(attempt));

  emu_close_rom();

  if (!load_rom(attempt))
  {
    char msg[GUI_PATH_LEN + 96];
    wsprintfA(msg, "Could not load:\n\n%s\n\n"
                   "The file may be missing, unreadable, or not a supported ROM.",
              attempt);
    MessageBoxA(g_hwnd, msg, APP_NAME, MB_OK | MB_ICONWARNING);
    gui_status("Loading failed");
    return 0;
  }

  lstrcpynA(rom_path, attempt, sizeof(rom_path));
  split_basename(rom_path, rom_base, sizeof(rom_base));

  audio_init(gui.sample_rate, 0);
  system_init();

  load_backup_ram();

  emu_running = 1;
  emu_paused = 0;
  browser_panel_show(0);
  rewind_reset();

  /* Set the audio format up before the reset so the chips come out of reset
     already matched to the output rate. */
  emu_apply_audio_settings();
  system_reset();

  cheats_load_for_rom(rom_base);
  video_viewport_changed();

  config_add_recent(rom_path);
  config_save();

  rebuild_recent_menu();
  update_title();
  gui_update_menu();
  gui_resize_to_scale(gui.scale);
  gui_status_persistent("Running %s", rom_base);

  if (gui.fullscreen_on_load && !gui.fullscreen)
  {
    video_set_fullscreen(1);
    gui_update_menu();
  }

  next_frame_time = 0.0;
  return 1;
}

/* For ROM Information / Edit Cheats from the browser's context menu on
   a ROM that isn't the one currently running: populates rominfo and
   loads that ROM's cheat list for viewing/editing, without starting
   emulation -- no system_init/system_reset, no audio, emu_running
   stays 0, and the browser panel is left exactly as it was. This is
   only safe because nothing is running yet: load_rom() overwrites the
   same cart.rom buffer a live game would be using, so calling this
   while a *different* ROM is actually playing would corrupt it out
   from under it. Returns 0 without doing anything if a game is
   currently running -- callers fall back to emu_load_rom() then,
   which is the only safe option in that case. */
int emu_peek_rom(const char *path)
{
  char attempt[GUI_PATH_LEN];

  if (emu_running) return 0;
  if (!path || !path[0]) return 0;

  lstrcpynA(attempt, path, sizeof(attempt));

  emu_close_rom();   /* no-op given the emu_running guard above, but mirrors emu_load_rom's own pattern */

  if (!load_rom(attempt)) return 0;

  lstrcpynA(rom_path, attempt, sizeof(rom_path));
  split_basename(rom_path, rom_base, sizeof(rom_base));

  cheats_load_for_rom(rom_base);

  return 1;
}

const char *emu_rom_filename(void)
{
  return rom_base;
}

const char *emu_rom_path(void)
{
  return rom_path;
}

void emu_reset(int hard)
{
  if (!emu_running) return;

  cheats_suspend();

  if (hard)
  {
    save_backup_ram();
    system_init();
    load_backup_ram();
  }

  system_reset();
  cheats_apply();
  waveout_flush();
  gui_notify(hard ? "Hard reset" : "Reset");
}

/****************************************************************************
 * Open dialog
 ****************************************************************************/

static void browse_for_rom(void)
{
  OPENFILENAMEA ofn;
  char file[GUI_PATH_LEN] = "";

  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner   = g_hwnd;
  ofn.lpstrFile   = file;
  ofn.nMaxFile    = sizeof(file);
  ofn.lpstrTitle  = "Open ROM";
  ofn.lpstrFilter =
    "All supported files\0*.zip;*.gz;*.md;*.gen;*.bin;*.smd;*.mdx;*.sms;*.gg;*.sg;*.68k;*.cue;*.iso;*.chd\0"
    "Mega Drive / Genesis\0*.md;*.gen;*.bin;*.smd;*.mdx;*.68k\0"
    "Master System / Game Gear / SG-1000\0*.sms;*.gg;*.sg\0"
    "Mega CD / Sega CD\0*.cue;*.iso;*.chd\0"
    "Archives\0*.zip;*.gz\0"
    "All files\0*.*\0\0";
  ofn.nFilterIndex = 1;
  ofn.lpstrInitialDir = gui.rom_dir[0] ? gui.rom_dir : NULL;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
              OFN_EXPLORER | OFN_NOCHANGEDIR;

  if (GetOpenFileNameA(&ofn)) emu_load_rom(file);
}

/****************************************************************************
 * Emulation
 ****************************************************************************/

static void run_one_frame(int present_video)
{
  int samples;

  /* RAM patches are overwritten by the game, so they go back in each frame. */
  cheats_ram_update();

  if (system_hw == SYSTEM_MCD)              system_frame_scd(0);
  else if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) system_frame_gen(0);
  else                                      system_frame_sms(0);

  if (bitmap.viewport.changed & 1)
  {
    bitmap.viewport.changed &= ~1;
    video_viewport_changed();
  }

  /* audio_update must run every frame or the blip buffers overrun, even
     when the samples are then thrown away. */
  samples = audio_update(soundframe);

  if (gui.sound_enabled && waveout_pending() < gui.latency)
  {
    waveout_submit(soundframe, samples);
  }

  if (present_video) video_frame();

  frames_this_second++;
  rewind_capture_tick(soundframe, samples);
}

static double now_seconds(void)
{
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)perf_freq.QuadPart;
}

static void tick_fps(void)
{
  DWORD now = GetTickCount();

  if (now - fps_tick >= 1000)
  {
    int fps = (int)(((DWORD)frames_this_second * 1000) / (now - fps_tick));
    video_report_fps(fps);
    status_set_fps(fps);
    frames_this_second = 0;
    fps_tick = now;
  }
}

/* Shows or hides the menu bar while fullscreen, in response to a polled
   right-click rather than WM_RBUTTONDOWN -- see the note on
   gui_input_fullscreen_menu_click() for why. */
static void toggle_fullscreen_menu(void)
{
  int showing = (GetMenu(g_hwnd) != NULL);

  SetMenu(g_hwnd, showing ? NULL : g_menu);

  if (showing)
  {
    /* Menu bar just got detached. Every way of asking Windows to redraw
       the non-client area on its own (DrawMenuBar()+InvalidateRect(),
       RedrawWindow() with RDW_FRAME, an actual resize-and-restore)
       failed to reliably clear it in testing -- painting directly over
       where it was doesn't depend on any of that, it's just an ordinary
       GDI fill like every other frame in this app already does. */
    RECT rc;
    HDC hdc = GetDC(NULL);

    if (hdc)
    {
      GetWindowRect(g_hwnd, &rc);
      rc.bottom = rc.top + GetSystemMetrics(SM_CYMENU);
      FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
      ReleaseDC(NULL, hdc);
    }
  }

  /* Fullscreen hides the cursor (repeated ShowCursor(FALSE) calls, since
     its internal counter needs to go negative to actually hide) --
     showing the menu bar without also bringing the cursor back left no
     way to see where a click would land. */
  if (showing) while (ShowCursor(FALSE) >= 0) { }
  else          while (ShowCursor(TRUE) < 0) { }

  video_invalidate();
  InvalidateRect(g_hwnd, NULL, TRUE);
}

static void tick_status_expiry(void)
{
  DWORD now = GetTickCount();

  if (status_transient_until && now >= status_transient_until)
  {
    status_transient_until = 0;
    if (g_status) SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)status_persistent);
  }
}

static void emulation_step(void)
{
  int fast, i, count;
  static DWORD last_mouse_poll;
  DWORD now_ms = GetTickCount();

  tick_status_expiry();

  if (now_ms - last_mouse_poll >= 16)
  {
    last_mouse_poll = now_ms;

    if (gui.fullscreen && gui_input_fullscreen_menu_click())
    {
      toggle_fullscreen_menu();
    }
  }

  if (emu_running && !in_modal_loop)
  {
    int slot;

    slot = gui_input_save_slot_shortcut();
    if (slot >= 0)
    {
      gui.state_slot = slot;
      emu_save_state(slot);
      gui_notify("Saved to slot %d", slot);
      gui_update_menu();
    }

    slot = gui_input_load_slot_shortcut();
    if (slot >= 0)
    {
      gui.state_slot = slot;
      emu_load_state(slot);
      gui_notify("Loaded slot %d", slot);
      gui_update_menu();
    }
  }

  if (!emu_running || emu_paused || in_modal_loop)
  {
    if (emu_running && emu_paused && gui_input_frame_advance())
    {
      run_one_frame(1);
      return;
    }

    MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT);
    return;
  }

  if (gui_input_rewind())
  {
    /* Matches normal playback's own cadence -- snapshots are now captured
       every single frame (see rewind.c), so popping one every 60th of a
       second reads as real-time-speed reverse motion instead of jumping. */
    double now = now_seconds();
    double period = 1.0 / 60.0;

    if (next_rewind_time == 0.0 || now - next_rewind_time > 0.5)
    {
      next_rewind_time = now;
    }
    else if (now < next_rewind_time)
    {
      MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
      return;
    }
    next_rewind_time += period;

    if (rewind_step())
    {
      int audio_frames;
      const int16 *audio;

      rewind_hit_limit_notified = 0;

      /* rewind_step() already restored bitmap.data directly -- no
         forward frame needed to regenerate it, just present what's
         there. */
      video_frame();

      /* Reversed sample order, not forward playback -- this is what
         actually produces the characteristic reversed-sound rewind
         effect, matching the reversed picture. */
      audio = rewind_get_audio(&audio_frames);
      if (gui.sound_enabled && audio_frames > 0 && waveout_pending() < gui.latency)
      {
        waveout_submit(audio, audio_frames);
      }
    }
    else if (!rewind_hit_limit_notified)
    {
      rewind_hit_limit_notified = 1;
      gui_notify("Rewind limit reached");
    }

    next_frame_time = 0.0;
    tick_fps();
    return;
  }

  fast = gui_input_fast_forward();

  if (!fast)
  {
    if (gui.sound_enabled)
    {
      /* Let the sound card set the pace. */
      if (waveout_pending() >= gui.latency)
      {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
        return;
      }
    }
    else
    {
      double now = now_seconds();
      double period = (snd.frame_rate > 1.0) ? (1.0 / snd.frame_rate) : (1.0 / 60.0);

      if (next_frame_time == 0.0 || now - next_frame_time > 0.5)
      {
        next_frame_time = now;
      }
      else if (now < next_frame_time)
      {
        MsgWaitForMultipleObjects(0, NULL, FALSE, 1, QS_ALLINPUT);
        return;
      }
      next_frame_time += period;
    }

    run_one_frame(1);
  }
  else
  {
    /* Draw one frame in every fast_forward_ratio to keep the window alive. */
    count = gui.fast_forward_ratio;
    for (i = 0; i < count; i++)
    {
      run_one_frame(i == count - 1);
    }
    next_frame_time = 0.0;
  }

  tick_fps();
}

/****************************************************************************
 * Commands
 ****************************************************************************/

static void on_command(int id)
{
  /* Ranged commands first. */
  if (id >= IDM_FILE_RECENT_BASE && id < IDM_FILE_RECENT_BASE + GUI_RECENT_MAX)
  {
    int i = id - IDM_FILE_RECENT_BASE;
    if (gui.recent[i][0])
    {
      char path[GUI_PATH_LEN];
      lstrcpynA(path, gui.recent[i], sizeof(path));
      emu_load_rom(path);
    }
    return;
  }

  if (id >= IDM_FILE_SLOT_BASE && id < IDM_FILE_SLOT_BASE + GUI_SLOT_MAX)
  {
    gui.state_slot = id - IDM_FILE_SLOT_BASE;
    gui_update_menu();
    gui_notify("Slot %d selected", gui.state_slot);
    config_save();
    return;
  }

  if (id >= IDM_EMU_REGION_BASE && id < IDM_EMU_REGION_BASE + 5)
  {
    config.region_detect = (uint8)(id - IDM_EMU_REGION_BASE);
    emu_apply_region();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_EMU_SYSTEM_BASE && id < IDM_EMU_SYSTEM_BASE + 9)
  {
    static const uint8 systems[] =
    {
      0, SYSTEM_SG, SYSTEM_SGII, SYSTEM_SGII_RAM_EXT, SYSTEM_MARKIII,
      SYSTEM_SMS, SYSTEM_SMS2, SYSTEM_GG, SYSTEM_MD
    };
    config.system = systems[id - IDM_EMU_SYSTEM_BASE];
    gui_update_menu();
    config_save();
    gui_status("Console setting applies the next time a ROM is loaded");
    return;
  }

  if (id >= IDM_EMU_LOCKON_BASE && id < IDM_EMU_LOCKON_BASE + 4)
  {
    config.lock_on = (uint8)(id - IDM_EMU_LOCKON_BASE);
    gui_update_menu();
    config_save();
    gui_status("Lock-on cartridge applies the next time a ROM is loaded");
    return;
  }

  if (id >= IDM_VIDEO_SCALE_BASE && id < IDM_VIDEO_SCALE_BASE + 4)
  {
    gui.scale = id - IDM_VIDEO_SCALE_BASE + 1;
    if (gui.fullscreen) video_set_fullscreen(0);
    gui_resize_to_scale(gui.scale);
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_ASPECT_BASE && id < IDM_VIDEO_ASPECT_BASE + 3)
  {
    gui.aspect = id - IDM_VIDEO_ASPECT_BASE;
    video_viewport_changed();
    gui_resize_to_scale(gui.scale);
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_NTSC_BASE && id < IDM_VIDEO_NTSC_BASE + 4)
  {
    video_set_ntsc(id - IDM_VIDEO_NTSC_BASE);
    gui_update_menu();
    rebuild_filter_menu();   /* NTSC and a render filter are mutually exclusive */
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_FILTER_BASE && id < IDM_VIDEO_FILTER_BASE + FILTER_MENU_MAX)
  {
    int i = id - IDM_VIDEO_FILTER_BASE;
    if (i < video_filter_count())
    {
      video_set_filter(i);
      rebuild_filter_menu();
      config_save();
    }
    return;
  }

  if (id >= IDM_VIDEO_OVERSCAN_BASE && id < IDM_VIDEO_OVERSCAN_BASE + 4)
  {
    config.overscan = (uint8)(id - IDM_VIDEO_OVERSCAN_BASE);
    if (emu_running) bitmap.viewport.changed = 3;
    video_viewport_changed();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_SCANLINE_BASE && id < IDM_VIDEO_SCANLINE_BASE + 5)
  {
    gui.scanline_pct = (id - IDM_VIDEO_SCANLINE_BASE) * 25;
    video_invalidate();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_VIDEO_THEME_BASE && id < IDM_VIDEO_THEME_BASE + 3)
  {
    theme_set_mode(id - IDM_VIDEO_THEME_BASE);
    theme_apply_to_window(g_hwnd);
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_VIEW_LIST || id == IDM_VIEW_GRID)
  {
    gui.browser_grid_view = (id == IDM_VIEW_GRID);
    browser_apply_view_mode();
    gui_update_menu();
    config_save();
    return;
  }

  if (id == IDM_VIDEO_LARGE_UI)
  {
    gui.large_ui = !gui.large_ui;
    gui_update_menu();
    config_save();

    if (MessageBoxA(g_hwnd,
          "Genesis Plus GX needs to restart for this to take effect.\n\nRestart now?",
          "Larger UI", MB_YESNO | MB_ICONQUESTION) == IDYES)
    {
      char exe_path[GUI_PATH_LEN];
      char cmdline[GUI_PATH_LEN * 2 + 16];
      STARTUPINFOA si;
      PROCESS_INFORMATION pi;

      GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

      /* Only hand the new instance a ROM if one is actually running. rom_path
         is also set by ROM Information / Edit Cheats on a game that was only
         inspected (emu_peek_rom), and that must not launch it. */
      if (emu_running && rom_path[0])
        wsprintfA(cmdline, "\"%s\" \"%s\"", exe_path, rom_path);
      else
        wsprintfA(cmdline, "\"%s\"", exe_path);

      ZeroMemory(&si, sizeof(si));
      si.cb = sizeof(si);
      ZeroMemory(&pi, sizeof(pi));

      /* Launch the replacement first, and only close this instance if
         that actually succeeded -- if CreateProcess failed for some
         reason, the setting is still saved (just applies next time the
         user starts the app some other way) rather than leaving them
         with no running instance at all. */
      if (CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
      {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        PostMessage(g_hwnd, WM_CLOSE, 0, 0);
      }
    }
    return;
  }

  if (id >= IDM_AUDIO_FMCORE_BASE && id < IDM_AUDIO_FMCORE_BASE + 4)
  {
    int choice = id - IDM_AUDIO_FMCORE_BASE;

    config.ym3438 = (uint8)(choice == 3);
    switch (choice)
    {
      case 0: config.ym2612 = YM2612_DISCRETE; break;
      case 1: config.ym2612 = YM2612_INTEGRATED; break;
      default: config.ym2612 = YM2612_ENHANCED; break;
    }

    if (emu_running)
    {
      /* Unlike Sample Rate, Enable Sound, HQPSG, or Region -- none of
         which actually need sound_init() at all, since its behaviour
         depends purely on which FM core is configured, confirmed by
         reading it -- this menu is the one place that genuinely changes
         that configuration, so sound_init() actually has to run here.
         audio_init() then sound_reset() are the same fix as before:
         audio_init() rebuilds the blip buffers (skipping it left the OLD
         core's accumulator contents in place); sound_reset() resyncs
         fm_cycles_count/fm_cycles_start/fm_ptr with whatever ratio
         sound_init() just set up (skipping it crashed outright, since
         MAME's YM2612 and Nuked's YM3438 run at very different ratios).
         What this can't fix: MAME and Nuked are independent C
         implementations with no shared internal state, so switching
         between them can't carry over which instrument was playing on
         which channel -- the running game isn't told anything changed
         and won't re-send configuration it thinks is already in place.
         Some instruments may stay quiet until the game's own music driver
         naturally re-triggers them (a new area, a menu, etc.) or the game
         is reset. That part is inherent to swapping cores mid-run, not a
         bug this sequence can paper over. */
      audio_init(gui.sample_rate, 0);
      sound_init();
      sound_reset();

      /* audio_reset() is what actually clears the blip buffers/low-pass
         filter state/re-syncs the equalizer -- see the longer note in
         emu_apply_audio_settings(). Missing it here was why some
         instruments stayed silent until an actual reset happened. */
      audio_reset();
    }
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_AUDIO_RATE_BASE && id < IDM_AUDIO_RATE_BASE + 2)
  {
    gui.sample_rate = (id == IDM_AUDIO_RATE_BASE) ? 44100 : 48000;
    emu_apply_audio_settings();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_INPUT_PORTA_BASE && id < IDM_INPUT_PORTA_BASE + 10)
  {
    input.system[0] = (uint8)port_menu_value(0, id - IDM_INPUT_PORTA_BASE);
    if (emu_running) io_init();
    gui_update_menu();
    config_save();
    return;
  }

  if (id >= IDM_INPUT_PORTB_BASE && id < IDM_INPUT_PORTB_BASE + 10)
  {
    input.system[1] = (uint8)port_menu_value(1, id - IDM_INPUT_PORTB_BASE);
    if (emu_running) io_init();
    gui_update_menu();
    config_save();
    return;
  }

  switch (id)
  {
    case IDM_FILE_OPEN:
      browse_for_rom();
      break;

    case IDM_FILE_BROWSER:
      browser_panel_change_folder();
      break;

    case IDM_FILE_ROMINFO:
      dlg_rom_info(g_hwnd);
      break;

    case IDM_FILE_STATEMGR:
      dlg_state_manager(g_hwnd);
      break;

    case IDM_FILE_CLOSE:
    case IDM_EMU_STOP:
      emu_close_rom();
      break;

    case IDM_FILE_RECENT_CLEAR:
    {
      int i;
      for (i = 0; i < GUI_RECENT_MAX; i++) gui.recent[i][0] = '\0';
      rebuild_recent_menu();
      config_save();
      break;
    }

    case IDM_FILE_SAVESTATE:
    case IDM_ACCEL_QUICKSAVE:
      emu_save_state(gui.state_slot);
      break;

    case IDM_FILE_LOADSTATE:
    case IDM_ACCEL_QUICKLOAD:
      emu_load_state(gui.state_slot);
      break;

    case IDM_ACCEL_SLOT_NEXT:
      gui.state_slot = (gui.state_slot + 1) % GUI_SLOT_MAX;
      gui_update_menu();
      gui_notify("Slot %d selected", gui.state_slot);
      break;

    case IDM_ACCEL_SLOT_PREV:
      gui.state_slot = (gui.state_slot + GUI_SLOT_MAX - 1) % GUI_SLOT_MAX;
      gui_update_menu();
      gui_notify("Slot %d selected", gui.state_slot);
      break;

    case IDM_FILE_SCREENSHOT:
    {
      char path[GUI_PATH_LEN];
      char name[GUI_PATH_LEN];

      if (video_screenshot(path, sizeof(path)))
      {
        char *slash = strrchr(path, '\\');
        lstrcpynA(name, slash ? slash + 1 : path, sizeof(name));
        gui_notify("Saved %s", name);
      }
      else
      {
        gui_notify("Could not save the screenshot");
      }
      break;
    }

    case IDM_FILE_EXIT:
      PostMessage(g_hwnd, WM_CLOSE, 0, 0);
      break;

    case IDM_EMU_PAUSE:
      if (!emu_running) break;
      emu_paused = !emu_paused;
      auto_paused_by_focus = 0;
      if (emu_paused) waveout_flush();
      next_frame_time = 0.0;
      gui_update_menu();
      status_set_fps(0);
      gui_notify(emu_paused ? "Paused" : "Resumed");
      break;

    case IDM_EMU_RESET:
      emu_reset(0);
      break;

    case IDM_EMU_HARDRESET:
      emu_reset(1);
      break;

    case IDM_EMU_CHEATS:
      if (!emu_running) break;
      dlg_cheats(g_hwnd);
      break;

    case IDM_EMU_BIOS:
      /* 0 = cartridge only, 3 = BIOS enabled and booted first. */
      config.bios = (uint8)(config.bios ? 0 : 3);
      gui_update_menu();
      config_save();
      gui_status("BIOS setting applies the next time a ROM is loaded");
      break;

    case IDM_EMU_ADDRERROR:
      config.addr_error = (uint8)(!config.addr_error);
      gui_update_menu();
      config_save();
      break;

    case IDM_EMU_PAUSE_UNFOCUSED:
      gui.pause_on_focus_loss = !gui.pause_on_focus_loss;
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_FULLSCREEN:
      if (!emu_running) break;
      video_set_fullscreen(!gui.fullscreen);
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_FULLSCREEN_START:
      gui.fullscreen_on_load = !gui.fullscreen_on_load;
      gui_update_menu();
      config_save();
      break;

    case IDM_ACCEL_LEAVE_FULLSCREEN:
      if (gui.fullscreen)
      {
        video_set_fullscreen(0);
        gui_update_menu();
        config_save();
      }
      else if (emu_running)
      {
        /* Only while a game is actually running -- otherwise Esc would
           unexpectedly jump into fullscreen while just browsing. */
        video_set_fullscreen(1);
        gui_update_menu();
        config_save();
      }
      break;

    case IDM_VIDEO_SMOOTH:
      gui.smooth = !gui.smooth;
      video_viewport_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_HWACCEL:
      gui.hw_accel = !gui.hw_accel;
      video_set_hw_accel(gui.hw_accel);
      video_viewport_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_VSYNC:
      gui.vsync = !gui.vsync;
      video_set_vsync(gui.vsync);
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_FILTER_NONE:
      video_set_filter(-1);
      rebuild_filter_menu();
      config_save();
      break;

    case IDM_VIDEO_SHOWFPS:
      gui.show_fps = !gui.show_fps;
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_INTERLACE:
      config.render = (uint8)(!config.render);
      if (emu_running) bitmap.viewport.changed = 3;
      video_viewport_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_VIDEO_GGEXTRA:
      config.gg_extra = (uint8)(!config.gg_extra);
      if (emu_running) bitmap.viewport.changed = 3;
      video_viewport_changed();
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_ENABLE:
      gui.sound_enabled = !gui.sound_enabled;
      emu_apply_audio_settings();
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_SETTINGS:
      dlg_audio(g_hwnd);
      break;

    case IDM_AUDIO_MONO:
      config.mono = (uint8)(!config.mono);
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_LOWPASS:
      config.filter = (uint8)(config.filter ? 0 : 1);
      if (emu_running) audio_set_equalizer();
      gui_update_menu();
      config_save();
      break;

    case IDM_AUDIO_HQPSG:
      config.hq_psg = (uint8)(!config.hq_psg);
      if (emu_running)
      {
        /* Direct psg_init() call, not sound_init() -- sound_init() always
           reinitializes the FM chip too as an inseparable part of the same
           call (confirmed by reading it: FM and PSG setup are one
           function, no way to ask for just one), which has nothing to do
           with this setting and was needlessly wiping FM instrument state
           on every toggle. psg_init() is independently exposed by the
           core for exactly this. Matches sound_init()'s own ternary for
           which PSG variant a given system uses. */
        psg_init((system_hw == SYSTEM_SG) ? PSG_DISCRETE : PSG_INTEGRATED);
      }
      gui_update_menu();
      config_save();
      break;

    case IDM_INPUT_P1:
      dlg_input(g_hwnd, 0);
      break;

    case IDM_INPUT_P2:
      dlg_input(g_hwnd, 1);
      break;

    case IDM_HELP_SHORTCUTS:
      dlg_shortcuts(g_hwnd);
      break;

    case IDM_HELP_BIOSINFO:
      dlg_bios_info(g_hwnd);
      break;

    case IDM_HELP_ABOUT:
      dlg_about(g_hwnd);
      break;

    default:
      break;
  }
}

/****************************************************************************
 * Window procedure
 ****************************************************************************/

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_COMMAND:
    {
      WORD id = LOWORD(wp);
      WORD notify = HIWORD(wp);

      if (browser_panel_handle_command(id, notify)) return 0;

      if (notify == 0 || notify == 1) on_command(id);
      return 0;
    }

    case 0x0091:   /* WM_UAHDRAWMENU -- undocumented, no public name */
      if (theme_draw_menu_bar(hwnd, lp)) return 0;
      break;

    case 0x0092:   /* WM_UAHDRAWMENUITEM -- undocumented, no public name */
      if (theme_draw_menu_item(lp)) return 0;
      break;

    case WM_MEASUREITEM:
      if (theme_measure_menu_ownerdraw(lp)) return TRUE;
      break;

    case WM_DRAWITEM:
      if (theme_draw_menu_ownerdraw(lp)) return TRUE;
      break;

    case WM_NOTIFY:
    {
      LRESULT sb = theme_statusbar_customdraw((NMHDR *)lp, g_status);
      if (sb != -1) return sb;
      sb = theme_header_customdraw((NMHDR *)lp);
      if (sb != -1) return sb;
      if (browser_panel_handle_notify((NMHDR *)lp)) return 0;
      return 0;
    }

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      video_repaint(hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_ERASEBKGND:
      return 1;   /* video.c paints every pixel it owns */

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    {
      HBRUSH br = browser_panel_ctlcolor((HWND)lp, (HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_CONTEXTMENU:
      if (browser_panel_handle_contextmenu((HWND)wp,
            (lp == (LPARAM)-1) ? -1 : (int)(short)LOWORD(lp),
            (lp == (LPARAM)-1) ? -1 : (int)(short)HIWORD(lp))) return 0;
      break;

    case WM_SIZE:
      if (wp == SIZE_MINIMIZED)
      {
        if (emu_running && !emu_paused)
        {
          emu_paused = 1;
          auto_paused_by_minimize = 1;
          waveout_flush();
          gui_update_menu();
          status_set_fps(0);
        }
        return 0;
      }

      if (auto_paused_by_minimize)
      {
        auto_paused_by_minimize = 0;
        if (emu_running && emu_paused)
        {
          emu_paused = 0;
          gui_update_menu();
        }
      }

      layout_status();
      video_invalidate();
      capture_window_geometry();
      if (browser_panel_visible())
      {
        RECT content;
        get_content_rect(&content);
        browser_panel_layout(&content);
      }
      return 0;

    case WM_MOVE:
      capture_window_geometry();
      return 0;

    case 0x02E0:   /* WM_DPICHANGED, not in every mingw headers version */
    {
      /* The manifest claims per-monitor awareness, so Windows expects us to
         resize ourselves when the window moves to a different-DPI display. */
      RECT *suggested = (RECT *)lp;

      if (suggested && !gui.fullscreen)
      {
        SetWindowPos(hwnd, NULL,
                     suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
      }
      layout_status();
      video_invalidate();
      return 0;
    }

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO *mmi = (MINMAXINFO *)lp;
      mmi->ptMinTrackSize.x = 256;
      mmi->ptMinTrackSize.y = 224;
      return 0;
    }

    case WM_ACTIVATE:
    {
      int active = (LOWORD(wp) != WA_INACTIVE);

      gui_input_set_focus(active);

      if (!active && gui.pause_on_focus_loss && emu_running && !emu_paused)
      {
        emu_paused = 1;
        auto_paused_by_focus = 1;
        waveout_flush();
        gui_update_menu();
        status_set_fps(0);
      }
      else if (active && auto_paused_by_focus)
      {
        auto_paused_by_focus = 0;
        if (emu_running && emu_paused)
        {
          emu_paused = 0;
          gui_update_menu();
        }
      }
      next_frame_time = 0.0;
      return 0;
    }

    case WM_ENTERMENULOOP:
    case WM_ENTERSIZEMOVE:
      in_modal_loop = 1;
      waveout_flush();
      return 0;

    case WM_EXITMENULOOP:
    case WM_EXITSIZEMOVE:
      in_modal_loop = 0;
      next_frame_time = 0.0;
      return 0;

    case WM_DROPFILES:
    {
      HDROP drop = (HDROP)wp;
      char path[GUI_PATH_LEN];

      if (DragQueryFileA(drop, 0, path, sizeof(path)))
      {
        emu_load_rom(path);
        SetForegroundWindow(hwnd);
      }
      DragFinish(drop);
      return 0;
    }

    case WM_SYSCOMMAND:
      /* Do not let the screensaver interrupt a game. */
      if ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER)
      {
        if (emu_running && !emu_paused) return 0;
      }

      /* F10's default behavior is to activate the menu bar the same way
         Alt does -- both arrive here as SC_KEYMENU, distinguished by lp
         being 0 for F10 specifically (not a character key) versus the
         actual letter code for an Alt+letter mnemonic, which must still
         work normally. Left alone, F10 briefly deactivates the window
         while entering that menu mode, which triggers "Pause emulation
         when in background" as an unintended side effect -- there was
         never an actual F10-to-pause binding, just this indirect path
         through a feature that has nothing to do with F10 itself. */
      if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;

      break;

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_SETTINGCHANGE:
      theme_handle_settingchange(hwnd);
      return 0;

    default:
      break;
  }

  return DefWindowProc(hwnd, msg, wp, lp);
}

/****************************************************************************
 * Startup
 ****************************************************************************/

static int create_main_window(void)
{
  WNDCLASSEXA wc;
  RECT rc;
  int w, h;
  int x, y;

  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize        = sizeof(wc);
  wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  wc.lpfnWndProc   = wnd_proc;
  wc.hInstance     = g_inst;
  wc.hIcon         = LoadIconA(g_inst, MAKEINTRESOURCEA(IDI_APPICON));
  wc.hIconSm       = wc.hIcon;
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  wc.lpszClassName = APP_CLASS;

  if (!RegisterClassExA(&wc)) return 0;

  g_menu = LoadMenuA(g_inst, MAKEINTRESOURCEA(IDR_MAINMENU));
  if (gui.large_ui) theme_ownerdraw_menu(g_menu);

  video_preferred_size(gui.scale, &w, &h);
  rc.left = 0; rc.top = 0; rc.right = w; rc.bottom = h;
  AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, TRUE, 0);

  /*
   * Use the saved position only if it still lands on a real monitor --
   * otherwise a since-unplugged second monitor would put the window
   * somewhere the person can never reach. MonitorFromRect returns NULL for
   * a rect that intersects nothing, which is exactly the check needed.
   */
  x = CW_USEDEFAULT;
  y = CW_USEDEFAULT;

  if (gui.window_x > -32000 && gui.window_y > -32000)
  {
    RECT candidate;
    candidate.left   = gui.window_x;
    candidate.top    = gui.window_y;
    candidate.right  = gui.window_x + (rc.right - rc.left);
    candidate.bottom = gui.window_y + (rc.bottom - rc.top);

    if (MonitorFromRect(&candidate, MONITOR_DEFAULTTONULL) != NULL)
    {
      x = gui.window_x;
      y = gui.window_y;
    }
  }

  g_hwnd = CreateWindowExA(0, APP_CLASS, APP_NAME, WS_OVERLAPPEDWINDOW,
                           x, y,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, g_menu, g_inst, NULL);
  if (!g_hwnd) return 0;

  g_status = CreateWindowExA(WS_EX_COMPOSITED, STATUSCLASSNAMEA, NULL,
                             WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                             0, 0, 0, 0, g_hwnd, NULL, g_inst, NULL);
  SendMessage(g_status, WM_SETFONT, (WPARAM)gui_get_ui_font(), TRUE);
  if (gui.large_ui) SendMessage(g_status, SB_SETMINHEIGHT, 30, 0);

  browser_panel_create(g_hwnd);

  g_accel = LoadAcceleratorsA(g_inst, MAKEINTRESOURCEA(IDR_ACCELERATORS));

  DragAcceptFiles(g_hwnd, TRUE);
  layout_status();

  /* Skipped when about to restore maximized: WinMain will maximize right
     after this returns, which would just discard this resize. */
  if (!gui.window_maximized)
  {
    /* Now that the status bar exists its real height is known, so the
       client area can be sized to give exactly the requested scale. */
    gui_resize_to_scale(gui.scale);
  }

  return 1;
}

/* Pulls the first command-line argument out, if there is one. */
static void first_argument(char *out, int out_len)
{
  LPWSTR *argv;
  int argc = 0;

  out[0] = '\0';

  argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return;

  if (argc >= 2)
  {
    WideCharToMultiByte(CP_ACP, 0, argv[1], -1, out, out_len, NULL, NULL);
  }

  LocalFree(argv);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
  INITCOMMONCONTROLSEX icc;
  MSG msg;
  char startup_rom[GUI_PATH_LEN];

  (void)prev;
  (void)cmdline;

  g_inst = inst;

  QueryPerformanceFrequency(&perf_freq);
  if (perf_freq.QuadPart == 0) perf_freq.QuadPart = 1;

  icc.dwSize = sizeof(icc);
  icc.dwICC  = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_LINK_CLASS;
  InitCommonControlsEx(&icc);

  error_init();
  config_load();
  gui.fullscreen = 0;   /* never restored across runs -- see the note near WinMain's end */

  ensure_dir("saves");
  ensure_dir("states");
  ensure_dir("screenshots");
  ensure_dir("bios");
  ensure_dir("cheats");

  /* Before create_main_window(), not after -- that call loads and
     attaches the menu resource, and the app-wide dark mode preference
     needs to already be active for the menu *bar* itself (not just its
     dropdown popups, which pick it up regardless of timing) to render
     dark. */
  theme_init();

  if (!create_main_window())
  {
    MessageBoxA(NULL, "The main window could not be created.",
                APP_NAME, MB_OK | MB_ICONERROR);
    return 1;
  }

  theme_apply_to_window(g_hwnd);

  if (!video_init())
  {
    MessageBoxA(g_hwnd, "The video buffer could not be created.",
                APP_NAME, MB_OK | MB_ICONERROR);
    return 1;
  }

  /* Restore whichever render filter was active last session. A name that no
     longer matches anything (a filter that was removed or renamed, or a
     leftover .rpi filename) just leaves filtering off and clears the saved
     value. */
  if (gui.render_filter[0])
  {
    char saved[64];
    lstrcpynA(saved, gui.render_filter, sizeof(saved));
    video_set_filter_by_name(saved);
  }

  gui_input_init();
  rewind_init();
  load_boot_rom();

  if (gui.sound_enabled && !waveout_open(gui.sample_rate))
  {
    gui.sound_enabled = 0;
    gui_status("No audio device available, sound is off");
  }
  waveout_set_volume(gui.volume);

  rebuild_recent_menu();
  rebuild_filter_menu();
  gui_update_menu();
  update_title();

  ShowWindow(g_hwnd, gui.window_maximized ? SW_MAXIMIZE : show);
  UpdateWindow(g_hwnd);

  /* Confirmed by direct testing: any real resize (either direction)
     fixes a stale-paint artifact that's otherwise present from first
     launch -- so trigger the exact same layout/repaint path a genuine
     WM_SIZE runs, once, right here, rather than waiting for the user
     to happen to resize the window themselves. */
  SendMessage(g_hwnd, WM_SIZE, 0, 0);

  first_argument(startup_rom, sizeof(startup_rom));
  if (startup_rom[0]) emu_load_rom(startup_rom);

  if (!emu_running)
  {
    RECT content;
    gui_status_persistent("Open a ROM to start, or drop one on this window");
    get_content_rect(&content);
    browser_panel_layout(&content);
    browser_panel_show(1);
  }

  /* Fullscreen deliberately does not carry over between runs -- always
     start windowed regardless of what gui.fullscreen was saved as. The
     field itself still gets tracked and saved during the session (menu
     checkmark state, various runtime guards elsewhere read it), it's only
     the startup restore that's skipped. */
  fps_tick = GetTickCount();

  for (;;)
  {
    int quit = 0;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
      if (msg.message == WM_QUIT) { quit = 1; break; }

      if (browser_panel_visible())
      {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN)
        {
          browser_panel_handle_return();
          continue;
        }

        /* IsDialogMessage is documented as usable on any top-level window
           that manages child controls the way a dialog does, not only real
           dialog boxes -- this is what gives Tab/Shift+Tab navigation
           between the search box, list and buttons without hand-rolling
           focus management. Gated on the panel being visible so it can
           never intercept a keystroke meant for the emulator itself, which
           only reads input while this panel is hidden. */
        if (IsDialogMessage(g_hwnd, &msg)) continue;
      }

      if (!g_accel || !TranslateAccelerator(g_hwnd, g_accel, &msg))
      {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    }

    if (quit) break;

    emulation_step();
  }

  emu_close_rom();
  config_save();

  waveout_close();
  gui_input_shutdown();
  video_shutdown();
  error_shutdown();

  return (int)msg.wParam;
}
