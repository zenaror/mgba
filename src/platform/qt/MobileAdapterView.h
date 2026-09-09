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

#include "CoreController.h"
#include "ui_MobileAdapterView.h"

namespace QGBA {

class Window;

class MobileAdapterView : public QDialog {
Q_OBJECT

public:
	// Opened only while a game runs, the same as on every other port, so the
	// controller is never null.
	MobileAdapterView(std::shared_ptr<CoreController> controller, Window* window, QWidget* parent = nullptr);
	~MobileAdapterView();

	// Whether the user asked for an adapter. Outlives any one game so the next
	// one loaded gets it plugged in, but is never written to disk.
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
	MobileAdapterGB* adapterGB();
	void setDns(int which, const QString& text);

	// Runs fn against whichever adapter is live, pausing emulation for as long
	// as it takes when there is a game to pause.
	template <typename F> void withAdapter(F&& fn) {
		CoreController::Interrupter interrupter(m_controller);
		if (adapter()) {
			fn(adapter());
		}
		getConfig();
	}

	Ui::MobileAdapterView m_ui;

	std::shared_ptr<CoreController> m_controller;

	static bool s_wanted;

	Window* m_window;
};

}

#endif
