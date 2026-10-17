// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "deck-layout.h"
#include "mono-api.h"
#include "deck-motion.h"
typedef struct {
 uint32_t prepareError,sceneStatus;volatile uint32_t* prepareTrace;
 PmMono mono;void* domain;void* label;void* widget;void* font;void* color;void* panel;
 void* getApplication;void* findScene;void* sceneClass;void* sceneVisible;void* getRoot;void* append;void* remove;void* labelCtor;void* fontCtor;void* colorCtor;
 void* containerClass;void* sceneCtor;void* addScene;void* removeScene;void* sceneOverlay;
 void* sceneFocusable;void* sceneFrontable;void* sceneWidth;void* sceneHeight;void* sceneBackground;
 void* scenePassFrontActive;void* gameLayerFocused;
 void* sceneBackgroundControl;void* sceneBackgroundLimit;
 uint32_t parentScene,overlayScene,sceneManualMode;uint8_t sceneAttached;
 void* text;void* name;void* x;void* y;void* width;void* height;void* textColor;void* labelFont;
 void* panelCtor;void* panelWidth;void* panelHeight;void* panelColor;void* panelVisible;
 // Optional UI2.ImageBox: a real picture widget for the replaceable logo. Every
 // symbol is probed; the deck keeps the drawn mark when any of them is missing.
 void* imageBox;void* imageBoxCtor;void* imageLoadAsync;void* imageWidth;void* imageHeight;void* imageVisible;
 PmDeckLayout painted;
 PmDeckMotion motion;uint8_t animate,moving;
 // When the focused row last changed: drives the marquee phase for long labels.
 uint64_t selectedAt;
 uint64_t attachedLabels,cleanupAt,openingAt;uint32_t attachedPanels;
 uint32_t root,labels[PM_DECK_LABELS],fonts[PM_DECK_STYLES],colors[PM_DECK_INKS];uint64_t checkedAt;uint32_t sequence,metricsSequence,noticeSequence;
 uint32_t panels[PM_DECK_PANELS],backgrounds[PM_DECK_BACKGROUNDS];
 uint32_t images[PM_DECK_IMAGES],attachedImages;char logoApplied[192];
 uint16_t flags;uint8_t selected,open,initialized,failed,noticeVisible,paintedValid,hud;
 uint32_t themeSerial; // ProsperoSettings palette currently in the colour objects
 uint32_t videoSerialApplied; // last boot-animation frame counter loaded into ImageBox
} PmDeckView;
// Resolve all required metadata before publishing hooks. Fail closed on an
// unfamiliar managed ABI rather than treating firmware offsets as UI metadata.
int pm_deck_view_prepare(PmDeckView* view,void** update,void** input,volatile uint32_t* trace);
// Read-only PUI member probe: writes /data/ProsperoMenu/pui-probe.txt (pui-probe.c).
void pui_probe_run(PmMono* mono,void* domain,void* puiImage);
// UI thread only. No files, sockets, sleeps, credential changes or kernel calls.
// 0 unavailable, 1 ready (including closed/detached), 2 waiting for scene visibility.
int pm_deck_view_render(PmDeckView* view,const PmDeckFrame* frame,int open,unsigned selected,uint64_t now);
// UI thread only: 0 ShellUI foreground, 1 full-screen game, -1 unknown,
// -2 method unavailable (retain the existing input route).
int pm_deck_view_game_focus(PmDeckView* view);
