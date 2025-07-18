#pragma once

#define IDR_MYMENU          200
#define IDR_MYACCEL         210
#define IDM_NEWSTREAM       201
#define IDM_CLOSEACTIVE     202
#define IDM_CLOSEALL        203
#define IDM_RUNMULTI        204
#define IDM_EXIT            205
#define IDM_SETTINGS        206
#define IDM_ABOUT           207

#define IDD_SETTINGS        300

#define IDC_STATIC          -1

#define IDC_TAB             1000
#define IDC_CHANNEL         1200
#define IDC_QUALITY         1201
#define IDC_PLAY            1202
#define IDC_NUMSTREAMS      1203

#define IDC_PLAYERPATH      1300
#define IDC_PLAYERARGS      1301
#define IDC_MINIMIZETOTRAY  1302
#define IDC_BROWSE_PLAYER   1303
#define IDC_VERBOSE_DEBUG   1304
#define IDC_LOG_TO_FILE     1305
#define IDC_LOAD        1101
#define IDC_QUALITIES   1102
#define IDC_WATCH       1103
#define IDC_STOP        1104
#define IDC_LOG_LIST    1105

// Favorites panel controls
#define IDC_FAVORITES_LIST      1106
#define IDC_FAVORITES_ADD       1107
#define IDC_FAVORITES_DELETE    1108
#define IDC_FAVORITES_EDIT      1109
#define IDC_CHECK_VERSION       1110
#define IDC_STATUS_BAR          1111
#define IDC_TSDUCK_MODE         1112  // TSDuck transport stream mode checkbox

// Tray icon support
#define WM_TRAYICON             (WM_USER + 100)
#define WM_PLAYER_STATE         (WM_USER + 101)
#define ID_TRAYICON             2000
#define TIMER_PLAYER_CHECK      3000
#define TIMER_CHUNK_UPDATE      3001
