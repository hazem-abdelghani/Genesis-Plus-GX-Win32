/****************************************************************************
 *  Genesis Plus GX -- Win32 GUI frontend
 *
 *  resource.h -- identifiers shared between gpgx.rc and the C sources.
 ****************************************************************************/

#ifndef _RESOURCE_H_
#define _RESOURCE_H_

/* Controls that never need to be addressed by code. */
#define IDC_STATIC                 -1

#define IDI_APPICON                 1
#define IDR_MAINMENU                2
#define IDR_ACCELERATORS            3

/* --- File ------------------------------------------------------------- */
#define IDM_FILE_OPEN             100
#define IDM_FILE_CLOSE            101
#define IDM_FILE_LOADSTATE        102
#define IDM_FILE_SAVESTATE        103
#define IDM_FILE_SCREENSHOT       104
#define IDM_FILE_EXIT             105
#define IDM_FILE_RECENT_CLEAR     106
#define IDM_FILE_BROWSER          107
#define IDM_FILE_ROMINFO          108
#define IDM_FILE_RECENT_BASE      110   /* .. 119 */
#define IDM_FILE_SLOT_BASE        120   /* .. 129 */

/* --- Emulation -------------------------------------------------------- */
#define IDM_EMU_PAUSE             200
#define IDM_EMU_STOP              203
#define IDM_EMU_RESET             201
#define IDM_EMU_HARDRESET         202
#define IDM_EMU_REGION_BASE       210   /* auto, US, EU, JP NTSC, JP PAL */
#define IDM_EMU_SYSTEM_BASE       220   /* .. 228 */
#define IDM_EMU_LOCKON_BASE       230   /* .. 233 */
#define IDM_EMU_BIOS              240
#define IDM_EMU_ADDRERROR         241
#define IDM_EMU_PAUSE_UNFOCUSED   242
#define IDM_EMU_CHEATS            243

#define IDM_VIEW_LIST             250
#define IDM_VIEW_GRID             251

/* --- Video ------------------------------------------------------------ */
#define IDM_VIDEO_SCALE_BASE      300   /* .. 303 == 1x .. 4x */
#define IDM_VIDEO_FULLSCREEN      310
#define IDM_VIDEO_FULLSCREEN_START 311
#define IDM_VIDEO_ASPECT_BASE     320   /* square, 4:3, stretch */
#define IDM_VIDEO_SMOOTH          330
#define IDM_VIDEO_SCANLINE_BASE   331   /* .. 335, 0/25/50/75/100% */
#define IDM_VIDEO_NTSC_BASE       340   /* off, composite, s-video, rgb */
#define IDM_VIDEO_OVERSCAN_BASE   350   /* none, vertical, horizontal, all */
#define IDM_VIDEO_SHOWFPS         360
#define IDM_VIDEO_INTERLACE       361
#define IDM_VIDEO_GGEXTRA         362
#define IDM_VIDEO_FILTER_NONE     363
                                        /* 364 .. 369 were the .rpi rescan and
                                           filter-scale items; left unused so
                                           the numbers below do not move */
#define IDM_VIDEO_FILTER_BASE     370   /* .. 393, one per built-in filter */
#define IDM_VIDEO_HWACCEL         394
#define IDM_VIDEO_VSYNC           395
#define IDM_VIDEO_THEME_BASE      396   /* .. 398: Auto/Light/Dark */
#define IDM_VIDEO_LARGE_UI        399

/* --- Audio ------------------------------------------------------------ */
#define IDM_AUDIO_ENABLE          400
#define IDM_AUDIO_SETTINGS        401
#define IDM_AUDIO_MONO            402
#define IDM_AUDIO_LOWPASS         403
#define IDM_AUDIO_HQPSG           404
#define IDM_AUDIO_FMCORE_BASE     410   /* MAME discrete, MAME ASIC, Nuked 2612, Nuked 3438 */
#define IDM_AUDIO_RATE_BASE       420   /* 44100, 48000 */

/* --- Input ------------------------------------------------------------ */
#define IDM_INPUT_P1              500
#define IDM_INPUT_P2              501
#define IDM_INPUT_PORTA_BASE      510   /* .. 519 */
#define IDM_INPUT_PORTB_BASE      530   /* .. 539 */

/* --- Help ------------------------------------------------------------- */
#define IDM_HELP_SHORTCUTS        600
#define IDM_HELP_ABOUT            601
#define IDM_HELP_BIOSINFO         602

/* --- Accelerator-only commands ---------------------------------------- */
#define IDM_ACCEL_QUICKSAVE       700
#define IDM_ACCEL_QUICKLOAD       701
#define IDM_ACCEL_SLOT_NEXT       702
#define IDM_ACCEL_SLOT_PREV       703
#define IDM_ACCEL_LEAVE_FULLSCREEN 704

#define IDM_BROWSER_PLAY           800
#define IDM_BROWSER_PLAY_STATE_BASE 810   /* .. 819, one per save slot (GUI_SLOT_MAX = 10) */
#define IDM_BROWSER_REFRESH        830
#define IDM_BROWSER_CHANGE_DIR     831
#define IDM_BROWSER_ROMINFO        832
#define IDM_BROWSER_CHEATS         833
#define IDM_BROWSER_SET_COVER      834
#define IDM_BROWSER_REMOVE_COVER   835
#define IDM_BROWSER_VIEW_LIST      836
#define IDM_BROWSER_VIEW_GRID      837

/* --- Dialogs ---------------------------------------------------------- */
#define IDD_ABOUT                 900
#define IDD_INPUT                 901
#define IDD_AUDIO                 902
#define IDD_SHORTCUTS             903
#define IDD_CHEATS                904
#define IDD_ROMINFO               905
#define IDD_BIOSINFO              906
#define IDD_ABOUT_LARGE           910
#define IDD_INPUT_LARGE           911
#define IDD_AUDIO_LARGE           912
#define IDD_SHORTCUTS_LARGE       913
#define IDD_CHEATS_LARGE          914
#define IDD_ROMINFO_LARGE         915
#define IDD_STATEMGR_LARGE        916
#define IDD_BIOSINFO_LARGE        917

#define IDC_ABOUT_TEXT           1000
#define IDC_ABOUT_LINK1          1069
#define IDC_ABOUT_LINK2          1070
#define IDC_ABOUT_ICON           1058
#define IDC_ABOUT_LINK           1001

#define IDC_INPUT_LIST           1010
#define IDC_INPUT_ASSIGN         1011
#define IDC_INPUT_AUTOASSIGN     1014
#define IDC_INPUT_CLEAR          1012
#define IDC_INPUT_DEFAULTS       1013
#define IDC_INPUT_LABEL_BASE     1071   /* .. 1084, one per row */
#define IDC_INPUT_DEVICE         1017
#define IDC_INPUT_PADTYPE        1015
#define IDC_INPUT_HINT           1016

#define IDC_AUDIO_VOLUME         1020
#define IDC_AUDIO_VOLUME_TEXT    1021
#define IDC_AUDIO_FM             1022
#define IDC_AUDIO_FM_TEXT        1023
#define IDC_AUDIO_PSG            1024
#define IDC_AUDIO_PSG_TEXT       1025
#define IDC_AUDIO_LATENCY        1026
#define IDC_AUDIO_LATENCY_TEXT   1027
#define IDC_AUDIO_CDDA           1028
#define IDC_AUDIO_CDDA_TEXT      1029
#define IDC_AUDIO_DEFAULTS       1030

#define IDC_SHORTCUTS_TEXT       1040
#define IDC_BIOSINFO_TEXT        1068
#define IDC_ROMINFO_TEXT         1041

#define IDC_CHEATS_LIST          1050
#define IDC_CHEATS_CODE          1051
#define IDC_CHEATS_DESC          1052
#define IDC_CHEATS_ADD           1053
#define IDC_CHEATS_REMOVE        1054
#define IDC_CHEATS_CLEAR         1055
#define IDC_CHEATS_HINT          1056
#define IDC_CHEATS_LOADCHT       1057
#define IDC_CHEATS_SAVE          1058
#define IDC_CHEATS_ENABLE        1059

#define IDC_BROWSER_LIST         1060
#define IDC_BROWSER_PATH         1061
#define IDC_BROWSER_CHANGE       1062
#define IDC_BROWSER_REFRESH      1063
#define IDC_BROWSER_COUNT        1064
#define IDC_BROWSER_SEARCH       1065
#define IDC_BROWSER_CHOOSE       1066

#define IDD_STATEMGR              908
#define IDM_FILE_STATEMGR         109
#define IDC_STATEMGR_SLOT_BASE   1100  /* .. 1109, one BS_BITMAP button per slot */
#define IDC_STATEMGR_LABEL_BASE  1110  /* .. 1119, one static label per slot */

#endif /* _RESOURCE_H_ */
