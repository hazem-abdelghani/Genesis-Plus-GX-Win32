/****************************************************************************
 *  Genesis Plus GX -- Win32 GUI frontend
 *
 *  dialogs.c -- the modal dialogs reachable from the menu.
 ****************************************************************************/

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include "shared.h"
#include "gui.h"
#include "resource.h"
#include "theme.h"

/****************************************************************************
 * Input configuration
 *
 * Assigning works by listening rather than by asking the user to pick from a
 * list: choose a button, press what you want it to be. The same listening
 * pass covers the keyboard and the gamepad, so there is no separate mode to
 * switch into for controllers.
 ****************************************************************************/

/* Listening state machine */
enum { LISTEN_OFF = 0, LISTEN_WAIT_RELEASE, LISTEN_ACTIVE };

static struct
{
  int       player;
  t_pad_map map;        /* working copy, committed on Save */
  uint8     padtype;
  int       key_fast_forward, key_rewind;  /* working copy */
  int       pad_fast_forward, pad_rewind;  /* working copy */
  int       listen;
  int       listen_target;  /* which row listening is for, fixed at start */
  int       auto_advance;   /* set by Auto Assign only; Assign/double-click leave it off */
  HWND      dlg;
} inp;

/* Fast forward/rewind are global hotkeys, not per-player pad buttons, so
   they don't belong in t_pad_map -- shown as two extra rows appended
   after the normal pad buttons, in both players' dialogs since it's the
   same single global setting either way (changing it from Player 2's
   dialog affects the same gui.key_fast_forward/key_rewind Player 1's
   would). Reuses the exact same listen/assign/clear machinery via a
   couple of special cases below rather than building a separate UI
   section for them. */
#define EXTRA_KEY_COUNT   2
#define EXTRA_FASTFORWARD PAD_KEYS
#define EXTRA_REWIND      (PAD_KEYS + 1)

static int input_list_count(void)
{
  return PAD_KEYS + EXTRA_KEY_COUNT;
}

static const char *input_list_entry_name(int sel)
{
  if (sel == EXTRA_FASTFORWARD) return "Fast forward";
  if (sel == EXTRA_REWIND)      return "Rewind";
  return input_button_label(sel);
}

static void input_fill_list(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_INPUT_LIST);
  int sel = (int)SendMessage(list, LB_GETCURSEL, 0, 0);
  int i, count = input_list_count();

  SendMessage(list, LB_RESETCONTENT, 0, 0);

  for (i = 0; i < count; i++)
  {
    char line[128];
    int key, button;

    if (i == EXTRA_FASTFORWARD)      { key = inp.key_fast_forward; button = inp.pad_fast_forward; }
    else if (i == EXTRA_REWIND)      { key = inp.key_rewind; button = inp.pad_rewind; }
    else                              { key = inp.map.key[i]; button = inp.map.button[i]; }

    wsprintfA(line, "%s\t%s", input_key_name(key), input_pad_button_name(button));
    SendMessageA(list, LB_ADDSTRING, 0, (LPARAM)line);
  }

  if (sel == LB_ERR) sel = 0;
  SendMessage(list, LB_SETCURSEL, (WPARAM)sel, 0);
}

static void input_set_listening(HWND dlg, int on)
{
  /* IDC_INPUT_LIST is included here again -- graying it out while
     listening previously also hid which button was being assigned,
     since the button name used to be its own first column. Now that
     the button names are separate static labels beside it (never
     disabled, since they're not interactive controls at all), that's
     no longer a problem, and graying this one out makes clear it's
     not interactive while listening. */
  static const int controls[] =
  {
    IDC_INPUT_LIST, IDC_INPUT_ASSIGN, IDC_INPUT_AUTOASSIGN, IDC_INPUT_CLEAR, IDC_INPUT_DEFAULTS,
    IDC_INPUT_DEVICE, IDC_INPUT_PADTYPE, IDOK, IDCANCEL
  };
  int i;

  /* Moved off whatever button was just clicked (Assign or Auto Assign,
     typically) onto a static label before disabling anything, rather
     than disabling every other control first and the focused one last.
     That two-pass approach did stop the original bug (disabling a
     focused control while something else was still enabled let
     Windows cascade focus onto it, and a still-in-flight activation key
     would fire again on whatever it landed on -- confirmed directly:
     Assign correctly setting auto_advance to 0 was having Auto Assign's
     handler fire right after and set it back to 1, purely because Auto
     Assign was still enabled the instant Assign got disabled). But
     disabling a button while it still held focus turned out to leave
     it stuck rendering as grayed even after being genuinely re-enabled
     later (confirmed via IsWindowEnabled returning true while it still
     visibly looked disabled) -- multiple different repaint techniques
     couldn't clear it. Moving focus to a plain static label first means
     nothing ever needs disabling while focused in the first place, so
     neither problem has room to happen: labels aren't in controls[], so
     they're never disabled, and a control that isn't focused when
     disabled has nothing to cascade its focus onto. */
  if (on) SetFocus(GetDlgItem(dlg, IDC_INPUT_LABEL_BASE));

  for (i = 0; i < (int)(sizeof(controls) / sizeof(controls[0])); i++)
  {
    HWND ctl = GetDlgItem(dlg, controls[i]);
    EnableWindow(ctl, on ? FALSE : TRUE);
    RedrawWindow(ctl, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
  }

  /* Whichever pushbutton was tabbed to or clicked most recently (Auto
     Assign, typically, since it's what starts this) keeps Windows'
     blue default-button border even once disabled -- that indicator
     tracks focus history, not enabled state, and won't clear on its
     own. Explicitly handing it back to the dialog's real default. */
  SendMessage(dlg, DM_SETDEFID, (WPARAM)IDOK, 0);

  if (on)
  {
    HWND list = GetDlgItem(dlg, IDC_INPUT_LIST);
    int sel = (int)SendMessage(list, LB_GETCURSEL, 0, 0);
    char hint[128];

    inp.listen = LISTEN_WAIT_RELEASE;
    inp.listen_target = (sel == LB_ERR) ? 0 : sel;

    wsprintfA(hint, "Press the key or gamepad button for \"%s\".%s Esc cancels.",
              input_list_entry_name(inp.listen_target),
              inp.auto_advance ? " Then moves to the next button." : "");
    SetDlgItemTextA(dlg, IDC_INPUT_HINT, hint);
    SetTimer(dlg, 1, 15, NULL);
  }
  else
  {
    inp.listen = LISTEN_OFF;
    KillTimer(dlg, 1);
    SetDlgItemTextA(dlg, IDC_INPUT_HINT,
                    "Pick a button, choose Assign, then press the key you want.");
  }
}

/* After a successful assignment, either moves on to the next button and
   re-arms listening automatically (Auto Assign), or just stops there
   (Assign / double-click) -- inp.auto_advance, set by whichever one
   actually started this listen, decides which. Reuses
   input_set_listening(dlg, 1) as-is when advancing, including its
   "wait for the current key/button to be released first" safety --
   without that, whatever's still physically held from the assignment
   just made could get immediately captured again as the next one. */
static void input_advance_or_stop(HWND dlg, int sel)
{
  if (inp.auto_advance && sel + 1 < input_list_count())
  {
    HWND list = GetDlgItem(dlg, IDC_INPUT_LIST);
    SendMessage(list, LB_SETCURSEL, (WPARAM)(sel + 1), 0);
    input_set_listening(dlg, 1);
  }
  else
  {
    input_set_listening(dlg, 0);
  }
}

static void input_capture_tick(HWND dlg)
{
  int sel = inp.listen_target;
  int vk, button;

  if (sel < 0 || sel >= input_list_count())
  {
    input_set_listening(dlg, 0);
    return;
  }

  /* GetAsyncKeyState below (both directly for Escape, and inside
     input_capture_key()/input_capture_gamepad()) queries key/button
     state system-wide, with no idea which window is focused -- without
     this check, switching away to a different application entirely
     while a button was mid-assignment would still have whatever's
     typed there captured and assigned here, in the background. */
  if (GetForegroundWindow() != dlg) return;

  if (inp.listen == LISTEN_WAIT_RELEASE)
  {
    /* Do not capture the very keypress that opened the prompt. */
    if (!input_any_input_down(inp.map.device)) inp.listen = LISTEN_ACTIVE;
    return;
  }

  if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
  {
    input_set_listening(dlg, 0);
    return;
  }

  vk = input_capture_key();
  if (vk)
  {
    if (sel == EXTRA_FASTFORWARD)      inp.key_fast_forward = vk;
    else if (sel == EXTRA_REWIND)      inp.key_rewind = vk;
    else                                inp.map.key[sel] = vk;
    input_fill_list(dlg);
    input_advance_or_stop(dlg, sel);
    return;
  }

  button = input_capture_gamepad(sel >= PAD_KEYS ? gui.pad[0].device : inp.map.device);
  if (button)
  {
    if (sel == EXTRA_FASTFORWARD)      inp.pad_fast_forward = button;
    else if (sel == EXTRA_REWIND)      inp.pad_rewind = button;
    else                                inp.map.button[sel] = button;
    input_fill_list(dlg);
    input_advance_or_stop(dlg, sel);
  }
}

static void input_load_controls(HWND dlg)
{
  HWND dev = GetDlgItem(dlg, IDC_INPUT_DEVICE);
  HWND type = GetDlgItem(dlg, IDC_INPUT_PADTYPE);
  int tabs[1] = { 65 };
  int i;

  SendMessage(GetDlgItem(dlg, IDC_INPUT_LIST), LB_SETTABSTOPS, 1, (LPARAM)tabs);

  SendMessage(dev, CB_RESETCONTENT, 0, 0);
  SendMessageA(dev, CB_ADDSTRING, 0, (LPARAM)"Keyboard only");
  for (i = 0; i < 4; i++)
  {
    char buf[32];
    wsprintfA(buf, "Gamepad %d", i + 1);
    SendMessageA(dev, CB_ADDSTRING, 0, (LPARAM)buf);
  }
  SendMessage(dev, CB_SETCURSEL, (WPARAM)(inp.map.device + 1), 0);

  SendMessage(type, CB_RESETCONTENT, 0, 0);
  SendMessageA(type, CB_ADDSTRING, 0, (LPARAM)"Match the game");
  SendMessageA(type, CB_ADDSTRING, 0, (LPARAM)"3-button pad");
  SendMessageA(type, CB_ADDSTRING, 0, (LPARAM)"6-button pad");
  SendMessageA(type, CB_ADDSTRING, 0, (LPARAM)"2-button pad");

  switch (inp.padtype)
  {
    case DEVICE_PAD3B: SendMessage(type, CB_SETCURSEL, 1, 0); break;
    case DEVICE_PAD6B: SendMessage(type, CB_SETCURSEL, 2, 0); break;
    case DEVICE_PAD2B: SendMessage(type, CB_SETCURSEL, 3, 0); break;
    default:           SendMessage(type, CB_SETCURSEL, 0, 0); break;
  }
}

static void input_read_controls(HWND dlg)
{
  int dev = (int)SendMessage(GetDlgItem(dlg, IDC_INPUT_DEVICE), CB_GETCURSEL, 0, 0);
  int type = (int)SendMessage(GetDlgItem(dlg, IDC_INPUT_PADTYPE), CB_GETCURSEL, 0, 0);

  inp.map.device = (dev == CB_ERR) ? -1 : dev - 1;

  switch (type)
  {
    case 1:  inp.padtype = DEVICE_PAD3B; break;
    case 2:  inp.padtype = DEVICE_PAD6B; break;
    case 3:  inp.padtype = DEVICE_PAD2B; break;
    default: inp.padtype = DEVICE_PAD2B | DEVICE_PAD3B | DEVICE_PAD6B; break;
  }
}

static void input_align_labels(HWND dlg)
{
  HWND list = GetDlgItem(dlg, IDC_INPUT_LIST);
  RECT list_rc;
  POINT list_origin;
  int item_height = (int)SendMessage(list, LB_GETITEMHEIGHT, 0, 0);
  int i, count = input_list_count();
  int old_height, tight_height, delta;
  static const int below_list[] = { IDC_INPUT_HINT, IDOK, IDCANCEL };

  /* The listbox's top-left in dialog client coordinates -- GetWindowRect
     gives screen coordinates, ScreenToClient (against the dialog, not
     the listbox itself) converts that to the same coordinate space
     every child control's position is already expressed in. */
  GetWindowRect(list, &list_rc);
  list_origin.x = list_rc.left;
  list_origin.y = list_rc.top;
  ScreenToClient(dlg, &list_origin);

  /* Shrinks the listbox to exactly fit its 14 rows -- its .rc height was
     a guess sized generously enough to avoid clipping before the real
     per-item height was known, leaving empty space below the last row.
     Now that the real height is queried directly, it can be sized
     exactly instead of guessed. */
  old_height = list_rc.bottom - list_rc.top;
  tight_height = item_height * count + 4;   /* +4 for the WS_BORDER frame */
  delta = old_height - tight_height;
  if (delta < 0) delta = 0;

  SetWindowPos(list, NULL, 0, 0, list_rc.right - list_rc.left, tight_height,
               SWP_NOMOVE | SWP_NOZORDER);

  for (i = 0; i < count; i++)
  {
    HWND label = GetDlgItem(dlg, IDC_INPUT_LABEL_BASE + i);
    RECT label_rc;
    POINT label_origin;

    if (!label) continue;

    /* Only Y changes -- X, width, and height stay exactly what the .rc
       template already set them to; only the vertical row spacing
       needed correcting to match the listbox's actual per-item height,
       which doesn't reduce to a clean round number of dialog units.
       The +2 nudges each label down slightly to align better with the
       listbox text's own vertical position within each row. */
    GetWindowRect(label, &label_rc);
    label_origin.x = label_rc.left;
    label_origin.y = label_rc.top;
    ScreenToClient(dlg, &label_origin);

    SetWindowPos(label, NULL, label_origin.x, list_origin.y + i * item_height + 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
  }

  /* Everything below the listbox (hint text, Save/Cancel) shifts up by
     the same amount the listbox just shrank, closing the gap rather
     than leaving it sitting empty partway down the dialog. */
  for (i = 0; i < (int)(sizeof(below_list) / sizeof(below_list[0])); i++)
  {
    HWND ctl = GetDlgItem(dlg, below_list[i]);
    RECT ctl_rc;
    POINT ctl_origin;

    GetWindowRect(ctl, &ctl_rc);
    ctl_origin.x = ctl_rc.left;
    ctl_origin.y = ctl_rc.top;
    ScreenToClient(dlg, &ctl_origin);

    SetWindowPos(ctl, NULL, ctl_origin.x, ctl_origin.y - delta, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
  }
}

static INT_PTR CALLBACK input_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  (void)lp;

  switch (msg)
  {
    case WM_INITDIALOG:
    {
      char title[64];
      inp.dlg = dlg;
      wsprintfA(title, "Configure Player %d", inp.player + 1);
      SetWindowTextA(dlg, title);
      input_load_controls(dlg);
      input_fill_list(dlg);
      input_align_labels(dlg);

      /* Explicit, not left to default tab-order focus -- observed the
         dialog occasionally opening already in a listening state when
         triggered via keyboard menu navigation, which this avoids
         regardless of the exact cause by making sure no button ever
         holds initial focus. Returning FALSE tells Windows this dialog
         handled focus itself and not to override it. */
      SetFocus(GetDlgItem(dlg, IDC_INPUT_LIST));
      SendMessage(dlg, DM_SETDEFID, (WPARAM)IDOK, 0);

      /* Applied last, after focus is already settled -- SWP_FRAMECHANGED
         inside this can trigger synchronous frame recalculation, which
         risks pumping other pending messages (like a stray Return still
         working its way through from the menu selection that opened
         this dialog) before focus was safely parked on the list. */
      theme_apply_to_window(dlg);
      return FALSE;
    }

    case WM_TIMER:
      if (inp.listen != LISTEN_OFF) input_capture_tick(dlg);
      return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br;
      if (GetDlgCtrlID((HWND)lp) == IDC_INPUT_LIST)
        br = theme_ctlcolor_custom((HDC)wp, RGB(0x33, 0x33, 0x33), 2);
      else
        br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_INPUT_LIST:
          if (HIWORD(wp) == LBN_DBLCLK)
          {
            inp.auto_advance = 0;
            input_set_listening(dlg, 1);
          }
          return TRUE;

        case IDC_INPUT_ASSIGN:
          input_read_controls(dlg);
          inp.auto_advance = 0;
          input_set_listening(dlg, 1);
          return TRUE;

        case IDC_INPUT_AUTOASSIGN:
          input_read_controls(dlg);
          inp.auto_advance = 1;
          SendMessage(GetDlgItem(dlg, IDC_INPUT_LIST), LB_SETCURSEL, 0, 0);
          input_set_listening(dlg, 1);
          return TRUE;

        case IDC_INPUT_CLEAR:
        {
          HWND list = GetDlgItem(dlg, IDC_INPUT_LIST);
          int sel = (int)SendMessage(list, LB_GETCURSEL, 0, 0);
          if (sel != LB_ERR && sel >= 0 && sel < input_list_count())
          {
            if (sel == EXTRA_FASTFORWARD)      { inp.key_fast_forward = 0; inp.pad_fast_forward = 0; }
            else if (sel == EXTRA_REWIND)      { inp.key_rewind = 0; inp.pad_rewind = 0; }
            else
            {
              inp.map.key[sel] = 0;
              inp.map.button[sel] = 0;
            }
            input_fill_list(dlg);
          }
          return TRUE;
        }

        case IDC_INPUT_DEFAULTS:
        {
          t_gui_config saved = gui;
          set_config_defaults();
          inp.map = gui.pad[inp.player];
          inp.padtype = config.input[inp.player].padtype;
          inp.key_fast_forward = gui.key_fast_forward;
          inp.key_rewind = gui.key_rewind;
          inp.pad_fast_forward = gui.pad_fast_forward;
          inp.pad_rewind = gui.pad_rewind;
          gui = saved;
          input_load_controls(dlg);
          input_fill_list(dlg);
          return TRUE;
        }

        case IDOK:
          input_read_controls(dlg);
          gui.pad[inp.player] = inp.map;
          config.input[inp.player].padtype = inp.padtype;
          gui.key_fast_forward = inp.key_fast_forward;
          gui.key_rewind = inp.key_rewind;
          gui.pad_fast_forward = inp.pad_fast_forward;
          gui.pad_rewind = inp.pad_rewind;
          EndDialog(dlg, IDOK);
          return TRUE;

        case IDCANCEL:
          if (inp.listen != LISTEN_OFF) { input_set_listening(dlg, 0); return TRUE; }
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_input(HWND parent, int player)
{
  if (player < 0 || player > 1) return;

  ZeroMemory(&inp, sizeof(inp));
  inp.player  = player;
  inp.map     = gui.pad[player];
  inp.padtype = config.input[player].padtype;
  inp.key_fast_forward = gui.key_fast_forward;
  inp.key_rewind = gui.key_rewind;
  inp.pad_fast_forward = gui.pad_fast_forward;
  inp.pad_rewind = gui.pad_rewind;

  if (DialogBoxParamA(g_inst, MAKEINTRESOURCEA(gui.large_ui ? IDD_INPUT_LARGE : IDD_INPUT), parent, input_proc, 0) == IDOK)
  {
    config_save();
    gui_status("Player %d controls saved", player + 1);
  }
}

/****************************************************************************
 * Audio levels and latency
 ****************************************************************************/

static struct
{
  int volume;
  int fm;
  int psg;
  int cdda;
  int latency;
} aud;

static void audio_set_slider(HWND dlg, int id, int text_id, int lo, int hi,
                             int value, const char *suffix)
{
  HWND bar = GetDlgItem(dlg, id);
  char buf[32];

  SendMessage(bar, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
  SendMessage(bar, TBM_SETTICFREQ, (WPARAM)((hi - lo) / 8 ? (hi - lo) / 8 : 1), 0);
  SendMessage(bar, TBM_SETPOS, TRUE, (LPARAM)value);

  wsprintfA(buf, "%d%s", value, suffix);
  SetDlgItemTextA(dlg, text_id, buf);
}

static void audio_refresh_labels(HWND dlg)
{
  char buf[32];

  wsprintfA(buf, "%d%%", aud.volume);
  SetDlgItemTextA(dlg, IDC_AUDIO_VOLUME_TEXT, buf);
  wsprintfA(buf, "%d%%", aud.fm);
  SetDlgItemTextA(dlg, IDC_AUDIO_FM_TEXT, buf);
  wsprintfA(buf, "%d%%", aud.psg);
  SetDlgItemTextA(dlg, IDC_AUDIO_PSG_TEXT, buf);
  wsprintfA(buf, "%d%%", aud.cdda);
  SetDlgItemTextA(dlg, IDC_AUDIO_CDDA_TEXT, buf);
  wsprintfA(buf, "%d", aud.latency);
  SetDlgItemTextA(dlg, IDC_AUDIO_LATENCY_TEXT, buf);
}

static void audio_load_controls(HWND dlg)
{
  audio_set_slider(dlg, IDC_AUDIO_VOLUME,  IDC_AUDIO_VOLUME_TEXT,  0, 100, aud.volume,  "%");
  audio_set_slider(dlg, IDC_AUDIO_FM,      IDC_AUDIO_FM_TEXT,      0, 200, aud.fm,      "%");
  audio_set_slider(dlg, IDC_AUDIO_PSG,     IDC_AUDIO_PSG_TEXT,     0, 200, aud.psg,     "%");
  audio_set_slider(dlg, IDC_AUDIO_CDDA,    IDC_AUDIO_CDDA_TEXT,    0, 200, aud.cdda,    "%");
  audio_set_slider(dlg, IDC_AUDIO_LATENCY, IDC_AUDIO_LATENCY_TEXT, 2, 8,   aud.latency, "");
}

static INT_PTR CALLBACK audio_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
      theme_apply_to_window(dlg);
      audio_load_controls(dlg);
      return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_HSCROLL:
    {
      HWND bar = (HWND)lp;
      int pos = (int)SendMessage(bar, TBM_GETPOS, 0, 0);
      int id = GetDlgCtrlID(bar);

      switch (id)
      {
        case IDC_AUDIO_VOLUME:  aud.volume  = pos; break;
        case IDC_AUDIO_FM:      aud.fm      = pos; break;
        case IDC_AUDIO_PSG:     aud.psg     = pos; break;
        case IDC_AUDIO_CDDA:    aud.cdda    = pos; break;
        case IDC_AUDIO_LATENCY: aud.latency = pos; break;
        default: break;
      }

      audio_refresh_labels(dlg);

      /* Master volume is audible straight away, the rest applies on Save. */
      if (id == IDC_AUDIO_VOLUME) waveout_set_volume(aud.volume);
      return TRUE;
    }

    case WM_COMMAND:
      switch (LOWORD(wp))
      {
        case IDC_AUDIO_DEFAULTS:
          aud.volume  = 100;
          aud.fm      = 100;
          aud.psg     = 150;
          aud.cdda    = 100;
          aud.latency = 4;
          audio_load_controls(dlg);
          waveout_set_volume(aud.volume);
          return TRUE;

        case IDOK:
          EndDialog(dlg, IDOK);
          return TRUE;

        case IDCANCEL:
          waveout_set_volume(gui.volume);   /* undo the live preview */
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      waveout_set_volume(gui.volume);
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_audio(HWND parent)
{
  aud.volume  = gui.volume;
  aud.fm      = config.fm_preamp;
  aud.psg     = config.psg_preamp;
  aud.cdda    = config.cdda_volume;
  aud.latency = gui.latency;

  if (DialogBoxParamA(g_inst, MAKEINTRESOURCEA(gui.large_ui ? IDD_AUDIO_LARGE : IDD_AUDIO), parent, audio_proc, 0) == IDOK)
  {
    gui.volume         = aud.volume;
    config.fm_preamp   = (int16)aud.fm;
    config.psg_preamp  = (int16)aud.psg;
    config.cdda_volume = (int16)aud.cdda;
    config.pcm_volume  = (int16)aud.cdda;
    gui.latency        = aud.latency;

    waveout_set_volume(gui.volume);
    emu_apply_audio_settings();
    config_save();
    gui_status("Audio settings saved");
  }
}

/****************************************************************************
 * Keyboard shortcuts
 ****************************************************************************/

static INT_PTR CALLBACK shortcuts_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  (void)lp;

  switch (msg)
  {
    case WM_INITDIALOG:
      theme_apply_to_window(dlg);
      {
        /* One consistent tab stop, wide enough for the longest key name
           ("Backspace (held)"), rather than a hand-guessed mix of one and
           two tabs per line -- that guessing is exactly what left "Toggle
           fullscreen" misaligned from its neighbours before. */
        int tabs[1] = { 190 };
        SendDlgItemMessage(dlg, IDC_SHORTCUTS_TEXT, EM_SETTABSTOPS, 1, (LPARAM)tabs);
      }

      SetDlgItemTextA(dlg, IDC_SHORTCUTS_TEXT,
        "Open a ROM\tCtrl+O\r\n"
        "ROM Browser\tCtrl+B\r\n"
        "Close the ROM\tCtrl+W\r\n"
        "Reset\tCtrl+R\r\n"
        "Hard reset\tCtrl+Shift+R\r\n"
        "Cheats\tCtrl+C\r\n"
        "\r\n"
        "Pause and resume\tF2 / Pause\r\n"
        "Stop the ROM\tF3\r\n"
        "Save to the current slot\tF5\r\n"
        "Load from the current slot\tF8\r\n"
        "Previous / next slot\tF6 / F7\r\n"
        "Save a screenshot\tF11\r\n"
        "\r\n"
        "Toggle fullscreen\tAlt+Enter\r\n"
        "Show/hide the menu bar (while fullscreen)\tRight-click\r\n"
        "Toggle fullscreen (while a game is running)\tEsc\r\n"
        "\r\n"
        "Fast forward\tTab (held)\r\n"
        "Rewind\tBackspace (held)\r\n"
        "Advance one frame while paused\t\\\r\n"
        "\r\n"
        "Player 1 defaults:\r\n"
        "D-pad\tArrow keys\r\n"
        "A B C\tA S D\r\n"
        "X Y Z\tQ W E\r\n"
        "Start\tEnter\r\n"
        "Mode\tRight Shift\r\n"
        "\r\n"
        "A connected gamepad works without setup. Change any of the pad\r\n"
        "bindings under Input > Configure Player.");
      return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
      {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_shortcuts(HWND parent)
{
  DialogBoxParamA(g_inst, MAKEINTRESOURCEA(gui.large_ui ? IDD_SHORTCUTS_LARGE : IDD_SHORTCUTS), parent, shortcuts_proc, 0);
}

static INT_PTR CALLBACK bios_info_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  (void)lp;

  switch (msg)
  {
    case WM_INITDIALOG:
      theme_apply_to_window(dlg);
      {
        int tabs[1] = { 130 };
        SendDlgItemMessage(dlg, IDC_BIOSINFO_TEXT, EM_SETTABSTOPS, 1, (LPARAM)tabs);
      }

      SetDlgItemTextA(dlg, IDC_BIOSINFO_TEXT,
        "Required or optional firmware files go in the frontend's \"./bios\" directory.\r\n"
        "\r\n"
        "Filename:\tDescription:\r\n"
        "\r\n"
        "\x95 bios_MD.bin\tMegaDrive TMSS startup ROM (bootrom) - Optional\r\n"
        "\x95 bios_CD_E.bin\tMegaCD EU BIOS - Required for MegaCD EU games\r\n"
        "\x95 bios_CD_U.bin\tSegaCD US BIOS - Required for SegaCD US games\r\n"
        "\x95 bios_CD_J.bin\tMegaCD JP BIOS - Required for MegaCD JP games\r\n"
        "\x95 bios_E.sms\tMasterSystem EU BIOS (bootrom) - Optional\r\n"
        "\x95 bios_U.sms\tMasterSystem US BIOS (bootrom) - Optional\r\n"
        "\x95 bios_J.sms\tMasterSystem JP BIOS (bootrom) - Optional\r\n"
        "\x95 bios.gg\tGameGear BIOS (bootrom) - Optional\r\n"
        "\x95 sk.bin\tSonic & Knuckles ROM (lock-on) - Optional\r\n"
        "\x95 sk2chip.bin\tSonic & Knuckles UPMEM ROM (lock-on) - Optional\r\n"
        "\x95 areplay.bin\tAction Replay ROM (lock-on) - Optional\r\n"
        "\x95 ggenie.bin\tGame Genie ROM (lock-on) - Optional");
      return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
      {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_bios_info(HWND parent)
{
  DialogBoxParamA(g_inst, MAKEINTRESOURCEA(gui.large_ui ? IDD_BIOSINFO_LARGE : IDD_BIOSINFO), parent, bios_info_proc, 0);
}

static const char *system_name(void)
{
  if (system_hw == SYSTEM_MCD) return "Sega CD / Mega CD";
  if ((system_hw & SYSTEM_PBC) == SYSTEM_MD) return "Genesis / Mega Drive";
  if (system_hw == SYSTEM_GG || system_hw == SYSTEM_GGMS) return "Game Gear";
  if (system_hw == SYSTEM_SMS || system_hw == SYSTEM_SMS2) return "Master System";
  if (system_hw == SYSTEM_SG || system_hw == SYSTEM_SGII || system_hw == SYSTEM_SGII_RAM_EXT) return "SG-1000";
  return "Unknown";
}

static INT_PTR CALLBACK rominfo_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  (void)lp;

  switch (msg)
  {
    case WM_INITDIALOG:
      theme_apply_to_window(dlg);
      {
        char text[2048];
        char domestic[50], international[50], product[14], romtype_str[4],
             country[18], copyright[18];
        uint32 rom_size;

        /* Copied out and trimmed -- the core's own fields are fixed-size
           and space-padded, not necessarily null-terminated at the point
           the real text ends. */
        lstrcpynA(domestic, rominfo.domestic, sizeof(domestic));
        lstrcpynA(international, rominfo.international, sizeof(international));
        lstrcpynA(product, rominfo.product, sizeof(product));
        lstrcpynA(romtype_str, rominfo.ROMType, sizeof(romtype_str));
        lstrcpynA(country, rominfo.country, sizeof(country));
        lstrcpynA(copyright, rominfo.copyright, sizeof(copyright));

        rom_size = rominfo.romend - rominfo.romstart + 1;

        wsprintfA(text,
          "File:\t%s\r\n"
          "System:\t%s\r\n"
          "\r\n"
          "Domestic title:\t%s\r\n"
          "International title:\t%s\r\n"
          "\r\n"
          "Product code:\t%s\r\n"
          "Region:\t%s\r\n"
          "Copyright:\t%s\r\n"
          "\r\n"
          "ROM size:\t%lu KB\r\n"
          "Header checksum:\t%04X\r\n"
          "Calculated checksum:\t%04X %s\r\n",
          emu_rom_filename()[0] ? emu_rom_filename() : "(unknown)",
          system_name(),
          domestic[0] ? domestic : "(none)",
          international[0] ? international : "(none)",
          product[0] ? product : "(none)",
          country[0] ? country : "(none)",
          copyright[0] ? copyright : "(none)",
          (unsigned long)(rom_size / 1024),
          rominfo.checksum,
          rominfo.realchecksum,
          (rominfo.checksum == rominfo.realchecksum) ? "(match)" : "(mismatch)");

        {
          int tabs[1] = { 100 };
          SendDlgItemMessage(dlg, IDC_ROMINFO_TEXT, EM_SETTABSTOPS, 1, (LPARAM)tabs);
        }
        SetDlgItemTextA(dlg, IDC_ROMINFO_TEXT, text);
      }
      return TRUE;

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
      {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_rom_info(HWND parent)
{
  if (!emu_running && !emu_rom_filename()[0]) return;
  DialogBoxParamA(g_inst, MAKEINTRESOURCEA(gui.large_ui ? IDD_ROMINFO_LARGE : IDD_ROMINFO), parent, rominfo_proc, 0);
}

/****************************************************************************
 * About
 ****************************************************************************/

static INT_PTR CALLBACK about_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  (void)lp;

  switch (msg)
  {
    case WM_INITDIALOG:
      theme_apply_to_window(dlg);
      if (gui.large_ui)
      {
        HICON big_icon = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_APPICON),
                                            IMAGE_ICON, 64, 64, 0);
        if (big_icon) SendDlgItemMessage(dlg, IDC_ABOUT_ICON, STM_SETICON, (WPARAM)big_icon, 0);
      }
      SetDlgItemTextA(dlg, IDC_ABOUT_TEXT,
        "Genesis Plus GX\r\n"
        "Sega Mega Drive / Genesis, Master System, Game Gear,\r\n"
        "SG-1000, Mega CD and Pico emulator.\r\n"
        "\r\n"
        "Core by Charles MacDonald (1998-2003) and\r\n"
        "Eke-Eke (2007-2026).\r\n"
        "Windows Frontend by Hazem Abdelghani (2026).\r\n"
        "\r\n"
        "Distributed under the Genesis Plus GX licence: source\r\n"
        "must accompany modified redistributions, and it may\r\n"
        "not be sold or used commercially.");

      SendDlgItemMessage(dlg, IDC_ABOUT_LINK1, WM_SETTEXT, 0,
        (LPARAM)"<a href=\"https://github.com/ekeeke/Genesis-Plus-GX\">Genesis Plus GX</a>");
      SendDlgItemMessage(dlg, IDC_ABOUT_LINK2, WM_SETTEXT, 0,
        (LPARAM)"<a href=\"https://github.com/hazem-abdelghani/Genesis-Plus-GX-Win32\">Genesis Plus GX Windows Frontend</a>");
      return TRUE;

    case WM_NOTIFY:
    {
      NMHDR *hdr = (NMHDR *)lp;
      if ((hdr->code == NM_CLICK || hdr->code == NM_RETURN) &&
          (hdr->idFrom == IDC_ABOUT_LINK1 || hdr->idFrom == IDC_ABOUT_LINK2))
      {
        NMLINK *link = (NMLINK *)lp;
        char url[256];

        WideCharToMultiByte(CP_ACP, 0, link->item.szUrl, -1, url, sizeof(url), NULL, NULL);
        ShellExecuteA(dlg, "open", url, NULL, NULL, SW_SHOWNORMAL);
        return TRUE;
      }
      break;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)
      {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      return FALSE;

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_about(HWND parent)
{
  DialogBoxParamA(g_inst, MAKEINTRESOURCEA(gui.large_ui ? IDD_ABOUT_LARGE : IDD_ABOUT), parent, about_proc, 0);
}

/****************************************************************************
 * Manage Save States
 ****************************************************************************/

#define STATEMGR_SLOTS 10

static HBITMAP statemgr_bitmaps[STATEMGR_SLOTS];

static void statemgr_refresh_slot(HWND dlg, int slot)
{
  char state_p[GUI_PATH_LEN], thumb_p[GUI_PATH_LEN], label[32];
  HWND btn = GetDlgItem(dlg, IDC_STATEMGR_SLOT_BASE + slot);
  HWND lbl = GetDlgItem(dlg, IDC_STATEMGR_LABEL_BASE + slot);
  int exists;

  state_path(slot, state_p, sizeof(state_p));
  exists = GetFileAttributesA(state_p) != INVALID_FILE_ATTRIBUTES;

  if (statemgr_bitmaps[slot])
  {
    SendMessage(btn, BM_SETIMAGE, IMAGE_BITMAP, (LPARAM)NULL);
    DeleteObject(statemgr_bitmaps[slot]);
    statemgr_bitmaps[slot] = NULL;
  }

  if (exists)
  {
    thumb_path(slot, thumb_p, sizeof(thumb_p));
    statemgr_bitmaps[slot] = video_load_thumbnail(thumb_p);
    wsprintfA(label, "Slot %d", slot);
  }
  else
  {
    wsprintfA(label, "Slot %d (empty)", slot);
  }

  /* Enabled whenever a state exists, even without a loadable thumbnail --
     an older save from before this dialog existed still has no .bmp next
     to it, and should still be loadable, just without a preview image. */
  SendMessage(btn, BM_SETIMAGE, IMAGE_BITMAP, (LPARAM)statemgr_bitmaps[slot]);
  EnableWindow(btn, exists);
  SetWindowTextA(lbl, label);
}

static INT_PTR CALLBACK statemgr_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_INITDIALOG:
    {
      int i;
      theme_apply_to_window(dlg);
      for (i = 0; i < STATEMGR_SLOTS; i++) statemgr_refresh_slot(dlg, i);
      return TRUE;
    }

    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      HBRUSH br = theme_ctlcolor((HDC)wp);
      if (br) return (LRESULT)br;
      break;
    }

    case WM_CONTEXTMENU:
    {
      HWND target = (HWND)wp;
      int id = GetDlgCtrlID(target);

      if (id >= IDC_STATEMGR_SLOT_BASE && id < IDC_STATEMGR_SLOT_BASE + STATEMGR_SLOTS)
      {
        int slot = id - IDC_STATEMGR_SLOT_BASE;
        char state_p[GUI_PATH_LEN];
        int exists;
        HMENU menu;
        int cmd;

        state_path(slot, state_p, sizeof(state_p));
        exists = GetFileAttributesA(state_p) != INVALID_FILE_ATTRIBUTES;

        menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, 1, "Save to this slot");
        AppendMenuA(menu, MF_STRING | (exists ? 0 : MF_GRAYED), 2, "Delete");

        /* Documented Windows workaround (MSDN, TrackPopupMenu remarks):
           without this, the popup can fail to dismiss correctly if this
           window isn't confirmed as the foreground one. */
        SetForegroundWindow(dlg);
        cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                              LOWORD(lp), HIWORD(lp), 0, dlg, NULL);
        PostMessage(dlg, WM_NULL, 0, 0);
        DestroyMenu(menu);

        if (cmd == 1)
        {
          emu_save_state(slot);
          statemgr_refresh_slot(dlg, slot);
        }
        else if (cmd == 2 && exists)
        {
          char thumb_p[GUI_PATH_LEN];
          DeleteFileA(state_p);
          thumb_path(slot, thumb_p, sizeof(thumb_p));
          DeleteFileA(thumb_p);
          statemgr_refresh_slot(dlg, slot);
        }
        return TRUE;
      }
      return FALSE;
    }

    case WM_COMMAND:
    {
      int id = LOWORD(wp);

      if (id >= IDC_STATEMGR_SLOT_BASE && id < IDC_STATEMGR_SLOT_BASE + STATEMGR_SLOTS &&
          HIWORD(wp) == BN_CLICKED)
      {
        int slot = id - IDC_STATEMGR_SLOT_BASE;
        char state_p[GUI_PATH_LEN];

        state_path(slot, state_p, sizeof(state_p));
        if (GetFileAttributesA(state_p) != INVALID_FILE_ATTRIBUTES)
        {
          emu_load_state(slot);
          EndDialog(dlg, IDOK);
        }
        return TRUE;
      }

      if (id == IDCANCEL)
      {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      return FALSE;
    }

    case WM_DESTROY:
    {
      int i;
      for (i = 0; i < STATEMGR_SLOTS; i++)
      {
        if (statemgr_bitmaps[i])
        {
          DeleteObject(statemgr_bitmaps[i]);
          statemgr_bitmaps[i] = NULL;
        }
      }
      return FALSE;
    }

    case WM_CLOSE:
      EndDialog(dlg, IDCANCEL);
      return TRUE;
  }

  return FALSE;
}

void dlg_state_manager(HWND parent)
{
  if (!emu_running) return;
  DialogBoxParamA(g_inst, MAKEINTRESOURCEA(gui.large_ui ? IDD_STATEMGR_LARGE : IDD_STATEMGR), parent, statemgr_proc, 0);
}
