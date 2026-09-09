/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GUI_MOBILE_H
#define GUI_MOBILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

struct mGUIRunner;

void mGUIShowMobileAdapter(struct mGUIRunner* runner);
void mGUIMobileAdapterAttach(struct mGUIRunner* runner);
void mGUIMobileAdapterDetach(struct mGUIRunner* runner);

// Notices anything the adapter did during the frame that only this side can
// act on, such as a config change that needs writing out. Once per frame.
void mGUIMobileAdapterPoll(struct mGUIRunner* runner);

// Whether there is adapter chatter worth putting on screen, and the drawing of
// it. Must be called with the GUI surface already prepared.
bool mGUIMobileAdapterHasLog(struct mGUIRunner* runner);
void mGUIMobileAdapterDrawLog(struct mGUIRunner* runner);

CXX_GUARD_END

#endif
