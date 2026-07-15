// Resource identifiers for the Windows shell.
#pragma once

#define IDI_VIETKI 101

// Phase 2 E / Phase 3: tray status icons (V / E / V+ / V-).
#define IDI_TRAY_V 102
#define IDI_TRAY_E 103
#define IDI_TRAY_VPLUS 104
#define IDI_TRAY_VMINUS 105
// Phase 5 G.1: Gaming Mode status icons (G / G+).
#define IDI_TRAY_G 106
#define IDI_TRAY_GPLUS 107

// Phase 2 C: the Settings dialog and its controls.
#define IDD_SETTINGS 200
#define IDD_SETTINGS_BASIC 201
#define IDD_SETTINGS_HOTKEYS 202
#define IDD_SETTINGS_SYSTEM 203
#define IDD_SETTINGS_GAMING 204
#define IDD_SETTINGS_STATS 205

#define IDC_STATUS_TEXT 1000
#define IDC_RADIO_TELEX 1001
#define IDC_RADIO_VNI 1002
#define IDC_RADIO_TONE_MODERN 1003
#define IDC_RADIO_TONE_OLD 1004
#define IDC_COMBO_MASTERHK 1005
#define IDC_HOTKEY_OVERRIDE 1006
#define IDC_HOTKEY_EXCLUSION 1007
#define IDC_CHECK_AUTOSTART 1008
#define IDC_CHECK_AUTOCOMPLETE 1009
#define IDC_CHECK_EXCLUSIONON 1010
#define IDC_CHECK_REVERTBLUR 1011
#define IDC_BTN_RUNADMIN 1012
#define IDC_LIST_EXCLUDED 1013
#define IDC_BTN_REMOVE 1014
#define IDC_CROSSHAIR 1015
#define IDC_CROSSHAIR_HINT 1016
#define IDC_ABOUT_TEXT 1017
#define IDC_BTN_SAVE 1018
#define IDC_BTN_CLOSE 1019
#define IDC_DIRTY_MARK 1022
#define IDC_BTN_EXIT_APP 1023
// Phase 3 D/E: spell-check toggle and Vietnamese hotkey labels.
#define IDC_CHECK_SPELLCHECK 1024
#define IDC_LABEL_MASTERHK 1025
#define IDC_LABEL_OVERRIDE 1026
#define IDC_LABEL_EXCLUSION 1027
// Phase 3 D.3: tab control that splits the dialog into Cơ bản / Phím tắt / Hệ thống.
#define IDC_TAB 1028
// Group boxes get explicit ids so each tab page can show/hide its own frames.
#define IDC_GRP_STATUS 1029
#define IDC_GRP_TYPING 1030
#define IDC_GRP_SPELL 1031
#define IDC_GRP_HOTKEYS 1032
#define IDC_GRP_OPTIONS 1033
#define IDC_GRP_EXCLUDED 1034
// VietKi-style hotkey editors: modifier checkboxes + one key edit.
#define IDC_MASTER_CTRL 1035
#define IDC_MASTER_ALT 1036
#define IDC_MASTER_SHIFT 1037
#define IDC_MASTER_WIN 1038
#define IDC_MASTER_KEY 1039
#define IDC_OVERRIDE_CTRL 1040
#define IDC_OVERRIDE_ALT 1041
#define IDC_OVERRIDE_SHIFT 1042
#define IDC_OVERRIDE_WIN 1043
#define IDC_OVERRIDE_KEY 1044
#define IDC_EXCLUSION_CTRL 1045
#define IDC_EXCLUSION_ALT 1046
#define IDC_EXCLUSION_SHIFT 1047
#define IDC_EXCLUSION_WIN 1048
#define IDC_EXCLUSION_KEY 1049
#define IDC_CHECK_SOUND_GLOBAL 1050
#define IDC_CHECK_SOUND_EXCLUDED 1051
#define IDC_CHECK_MASTER_HOTKEY 1052
#define IDC_CHECK_OVERRIDE_HOTKEY 1053
// Phase 4 C.4/H: "keep the rest of the word literal after a cancel" toggle, plus
// the two help icons and the short under-checkbox description labels (H.5).
#define IDC_CHECK_LOCKCANCEL 1054
#define IDC_HELP_SPELLCHECK 1055
#define IDC_HELP_LOCKCANCEL 1056
#define IDC_DESC_SPELLCHECK 1057
#define IDC_DESC_LOCKCANCEL 1058
// Phase 5 I: the "Chơi game" tab.
#define IDC_GRP_GAMING 1059
#define IDC_CHECK_GAMING_TOGGLE 1060
#define IDC_DESC_GAMING_TOGGLE 1061
#define IDC_CHECK_GAMING_TEMP 1062
#define IDC_DESC_GAMING_TEMP 1063
#define IDC_LABEL_GAMING_TRIGGER 1064
#define IDC_GAMING_TRIGGER_KEY 1065
#define IDC_DESC_GAMING_TRIGGER 1066
#define IDC_GAMING_TRIGGER_WARN 1067
#define IDC_CHECK_GAMING_NOTIFY 1068
#define IDC_GRP_GAMING_APPS 1069
#define IDC_LIST_GAMING 1070
#define IDC_BTN_GAMING_ADD_CURRENT 1071
#define IDC_GAMING_CROSSHAIR 1072
#define IDC_BTN_GAMING_REMOVE 1073
#define IDC_BTN_GAMING_RESTORE 1074
// Manual (typed) entry into the excluded / gaming lists, with cross-list
// duplicate checking when adding.
#define IDC_EXCLUDED_NAME 1076
#define IDC_EXCLUDED_NAME_LABEL 1077
#define IDC_BTN_EXCLUDED_ADD 1078
#define IDC_GAMING_NAME 1079
#define IDC_GAMING_NAME_LABEL 1080
#define IDC_BTN_GAMING_ADD 1081
// Phase 5: notice that some games (elevated / anti-cheat) need VietKi run as admin.
#define IDC_GAMING_ADMIN_WARN 1082
// Phase 5.1: gaming-tab sound + overlay options and the ⓘ help icons.
#define IDC_CHECK_GAMING_SOUND 1083
#define IDC_CHECK_GAMING_OVERLAY 1084
#define IDC_COMBO_OVERLAY_CORNER 1085
#define IDC_LABEL_OVERLAY_POS 1086
#define IDC_HELP_GAMING_TOGGLE 1087
#define IDC_HELP_GAMING_TEMP 1088
#define IDC_HELP_GAMING_SOUND 1089
#define IDC_HELP_GAMING_OVERLAY 1090
#define IDC_HELP_OVERRIDE_HOTKEY 1091
#define IDC_GAMING_ADMIN_ICON 1092
#define IDC_GAMING_PASTE_WARN 1093
#define IDC_HELP_GAMING_PASTE 1094
// ⓘ help icon for the autocomplete diacritic-fix (selection-replace) option.
#define IDC_HELP_AUTOCOMPLETE 1095
// "Run elevated at logon" sub-option of Khởi động cùng Windows.
#define IDC_CHECK_AUTOSTART_ADMIN 1096
// Phase 6: restore the word Space just committed on an immediate Backspace.
#define IDC_CHECK_RESTOREAFTERSPACE 1097
#define IDC_HELP_RESTOREAFTERSPACE 1098
#define IDC_DESC_RESTOREAFTERSPACE 1099
// Phase 6 section 7: the "Thống kê" tab (local-only typing stats).
#define IDC_GRP_STATS_TOGGLE 1100
#define IDC_CHECK_TYPINGSTATS 1101
#define IDC_HELP_TYPINGSTATS 1102
#define IDC_DESC_TYPINGSTATS 1103
#define IDC_GRP_STATS_SUMMARY 1104
#define IDC_STATS_SUMMARY 1105
#define IDC_GRP_STATS_WORDS 1106
#define IDC_LIST_TOPWORDS 1107
#define IDC_BTN_CLEAR_STATS 1108
// "Fix whole-word completely": order-independent ươ pairing + keep composing
// across a Backspace (auto-off for a word after >1 Backspace).
#define IDC_CHECK_FIXWHOLEWORD 1109
#define IDC_DESC_FIXWHOLEWORD 1110
