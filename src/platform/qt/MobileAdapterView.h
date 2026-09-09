/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#ifdef USE_LIBMOBILE

#include <QDialog>

#include <memory>

#include <mgba/core/interface.h>
#include <mgba/core/mobile.h>
#include <mgba/internal/gb/sio/mobile.h>

#include "CoreController.h"
#include "ui_MobileAdapterView.h"

namespace QGBA {

class Window;

class MobileAdapterView : public QDialog {
Q_OBJECT

public:
	// The controller may be null, in which case no game is running and the
	// dialog edits the stored settings through an adapter of its own.
	MobileAdapterView(std::shared_ptr<CoreController> controller, Window* window, QWidget* parent = nullptr);
	~MobileAdapterView();

	// Whether the user asked for an adapter. Outlives any one game so it can
	// be set before loading one, but is never written to disk.
	static bool wanted() { return s_wanted; }

public slots:
	void setAdapterEnabled(bool enabled);
	void setType(int type);
	void setUnmetered(bool unmetered);
	void setDns1();
	void setDns2();
	void setPort(int port);
	void setRelay();
	void setToken();
	void copyToken(bool checked);
	void importConfig(bool checked);

private slots:
	void getConfig();
	void advanceFrameCounter();

private:
	struct mobile_adapter* adapter();
	void setDns(int which, const QString& text);

	// Runs fn against whichever adapter is live, pausing emulation for as long
	// as it takes when there is a game to pause.
	template <typename F> void withAdapter(F&& fn) {
		CoreController::Interrupter interrupter;
		if (m_controller) {
			interrupter.interrupt(m_controller);
		}
		if (adapter()) {
			fn(adapter());
		}
		getConfig();
	}

	void createStandalone();
	void destroyStandalone();
	void saveStandalone();

	Ui::MobileAdapterView m_ui;

	std::shared_ptr<CoreController> m_controller;

	// Only used when there is no controller: an adapter that is never started,
	// so it never needs emulation timing behind it.
	GBSIOMobileAdapter m_standalone;
	bool m_hasStandalone = false;

	static bool s_wanted;

	Window* m_window;
};

}

#endif
