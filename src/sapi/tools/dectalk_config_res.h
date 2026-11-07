// Control ids for the DECtalk DTC-01 configuration utility dialog.
// Adapted layout from BstSpeech-sapi-master/tools/bestspeech_config_res.h.
#pragma once

#define IDD_MAIN                  100

#define IDC_VOICE                 1001

// "Apply settings to all voices": voice-slider changes go to every voice.
#define IDC_APPLY_ALL             1002

// Per-voice sliders (0..100) and their live numeric readouts.
#define IDC_PITCH                 1010
#define IDC_PITCH_VAL             1011
#define IDC_INFLECTION            1012
#define IDC_INFLECTION_VAL        1013
#define IDC_HEADSIZE              1014
#define IDC_HEADSIZE_VAL          1015
#define IDC_BREATHINESS           1016
#define IDC_BREATHINESS_VAL       1017
#define IDC_RICHNESS              1018
#define IDC_RICHNESS_VAL          1019
#define IDC_SMOOTHNESS            1020
#define IDC_SMOOTHNESS_VAL        1021
#define IDC_LOUDNESS              1022
#define IDC_LOUDNESS_VAL          1023

// Advanced per-voice sliders, in their own group box.
#define IDC_LARYNGEALIZATION      1024
#define IDC_LARYNGEALIZATION_VAL  1025
#define IDC_ASSERTIVENESS         1026
#define IDC_ASSERTIVENESS_VAL     1027

// Global settings.
#define IDC_RATE                  1030
#define IDC_RATE_VAL              1031
#define IDC_VOLUME                1032
#define IDC_VOLUME_VAL            1033
#define IDC_RATEBOOST             1034
#define IDC_RATEBOOST_VAL         1035
#define IDC_FIRMWARE              1036
// "Allow SAPI5 apps to control rate, pitch and volume".
#define IDC_APP_CONTROL           1037
// "Diagnostic log for bug reports": Off or On.
#define IDC_LOGGING               1038

// Buttons. Close uses the stock IDOK id so Enter/Esc/the title-bar X and
// Alt-F4 all reach the same dialog_proc branch that ends the dialog.
#define IDC_RESET_VOICE           1040
#define IDC_RESET_ALL             1041
#define IDC_PREVIEW               1042
