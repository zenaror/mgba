/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifdef USE_LIBMOBILE

#include "MobileAdapterView.h"

#include "ConfigController.h"
#include "CoreController.h"
#include "ShortcutController.h"
#include "Window.h"
#include "utils.h"

#include <QtAlgorithms>
#include <QClipboard>
#include <QFileInfo>
#include <QFontMetrics>
#include <QMessageBox>
#include <QMultiMap>
#include <QSettings>
#include <QStringList>

using namespace QGBA;

static int mobileConvertAddr(const QString& addr, Address* output, unsigned* port) {
	QHostAddress qaddress;
	if (addr.isEmpty()) {
		return 0;
	}
	bool hasPort = false;
	unsigned portInt = *port;
	if (!qaddress.setAddress(addr)) {
		QString portText = addr.section(':', -1);
		QString mainText = addr.section(':', 0, -2);
		hasPort = false;
		portInt = portText.toUInt(&hasPort);
		if (!hasPort || mainText.contains(':')) {
			if (!mainText.startsWith('[')) {
				return -1;
			}
			if (!mainText.endsWith(']')) {
				if (!portText.endsWith(']')) {
					return -1;
				}
				mainText += QString(':') + portText;
				hasPort = false;
			}
			mainText.remove(0, 1);
			mainText.remove(-1, 1);
		}
		if (!qaddress.setAddress(mainText)) {
			return -1;
		}
	}
	if (!convertAddress(&qaddress, output)) {
		return -1;
	}
	if (hasPort) {
		*port = portInt;
	}
	return 1;
}

bool MobileAdapterView::s_wanted = false;

static QString mobileAddrToString(const struct mobile_addr* addr, unsigned defaultPort) {
	QString ret = "";
	if (addr->type == MOBILE_ADDRTYPE_IPV6) {
		const struct mobile_addr6* addr6 = (const struct mobile_addr6*) addr;
		QHostAddress qaddress(addr6->host);
		ret = qaddress.toString();
		if (addr6->port != defaultPort) {
			ret = QString('[') + ret + "]:" + QString::number(addr6->port);
		}
	} else if (addr->type == MOBILE_ADDRTYPE_IPV4) {
		const struct mobile_addr4* addr4 = (const struct mobile_addr4*) addr;
		QHostAddress qaddress(ntohl(*(unsigned*) addr4->host));
		ret = qaddress.toString();
		if (addr4->port != defaultPort) {
			ret += QString(':') + QString::number(addr4->port);
		}
	}
	return ret;
}

struct mobile_adapter* MobileAdapterView::adapter() {
	return adapterGB()->adapter;
}

MobileAdapterGB* MobileAdapterView::adapterGB() {
	return m_controller->getMobileAdapter();
}

MobileAdapterView::MobileAdapterView(std::shared_ptr<CoreController> controller, Window* window, QWidget* parent)
	: QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint)
	, m_controller(controller)
	, m_window(window)
{
	m_ui.setupUi(this);

	QRegularExpression reToken("[\\dA-Fa-f]{32}?");
	QRegularExpressionValidator vToken(reToken, m_ui.setToken);
	m_ui.setToken->setValidator(&vToken);

	connect(m_ui.setType, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
	        this, &MobileAdapterView::setType);
	connect(m_ui.setUnmetered, &QAbstractButton::toggled, this, &MobileAdapterView::setUnmetered);
	connect(m_ui.setDns1, &QLineEdit::editingFinished, this, &MobileAdapterView::setDns1);
	connect(m_ui.setDns2, &QLineEdit::editingFinished, this, &MobileAdapterView::setDns2);
	connect(m_ui.setPort, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
	        this, &MobileAdapterView::setPort);
	connect(m_ui.setRelay, &QLineEdit::editingFinished, this, &MobileAdapterView::setRelay);
	connect(m_ui.setToken, &QLineEdit::editingFinished, this, &MobileAdapterView::setToken);
	connect(m_ui.copyToken, &QAbstractButton::clicked, this, &MobileAdapterView::copyToken);
	connect(m_ui.importConfig, &QAbstractButton::clicked, this, &MobileAdapterView::importConfig);

	connect(m_controller.get(), &CoreController::frameAvailable, this, &MobileAdapterView::advanceFrameCounter);
	connect(m_controller.get(), &CoreController::stopping, this, &QWidget::close);

	QString versionText = QString("%1.%2.%3").arg(
		QString::number(mobile_version_major),
		QString::number(mobile_version_minor),
		QString::number(mobile_version_patch));
	m_ui.versionText->setText(versionText);

	// This state is intentionally not persisted: it always starts disabled for a
	// fresh session, and stays enabled only for as long as this session runs.
	m_ui.enableAdapter->setChecked(s_wanted);
	connect(m_ui.enableAdapter, &QAbstractButton::toggled, this, &MobileAdapterView::setAdapterEnabled);

	// Bring an adapter up so the Status/Settings tabs have live data to show and
	// edit, even if it isn't enabled to keep running afterwards.
	if (!adapter()) {
		m_controller->attachMobileAdapter();
	}

	getConfig();
}

MobileAdapterView::~MobileAdapterView() {
	// Only tear the adapter down if it isn't meant to keep running in the background;
	// closing this window is no longer required to keep the adapter attached.
	if (m_ui.enableAdapter->isChecked()) {
		m_controller->saveMobileAdapterConfig();
	} else {
		m_controller->detachMobileAdapter();
	}
}

void MobileAdapterView::setAdapterEnabled(bool enabled) {
	s_wanted = enabled;
	if (enabled) {
		if (!adapter()) {
			m_controller->attachMobileAdapter();
		}
		getConfig();
	} else if (adapter()) {
		m_controller->detachMobileAdapter();
	}
}

// Turns typed text into an address, treating empty text as "none". Returns
// false if something was typed but couldn't be read as an address.
static bool parseMobileAddr(const QString& text, unsigned defaultPort, struct mobile_addr* out) {
	unsigned port = defaultPort;
	Address host;
	int res = mobileConvertAddr(text, &host, &port);
	if (res < 0) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	if (!res) {
		out->type = MOBILE_ADDRTYPE_NONE;
	} else if (host.version == IPV6) {
		struct mobile_addr6* addr6 = (struct mobile_addr6*) out;
		addr6->type = MOBILE_ADDRTYPE_IPV6;
		memcpy(&addr6->host, &host.ipv6, MOBILE_HOSTLEN_IPV6);
		addr6->port = port;
	} else {
		struct mobile_addr4* addr4 = (struct mobile_addr4*) out;
		addr4->type = MOBILE_ADDRTYPE_IPV4;
		*(uint32_t*) &addr4->host = htonl(host.ipv4);
		addr4->port = port;
	}
	return true;
}

void MobileAdapterView::setType(int type) {
	withAdapter([type](struct mobile_adapter* adapter) {
		enum mobile_adapter_device device;
		bool unmetered;
		mobile_config_get_device(adapter, &device, &unmetered);
		mobile_config_set_device(adapter, (enum mobile_adapter_device) (MOBILE_ADAPTER_BLUE + type), unmetered);
	});
}

void MobileAdapterView::setUnmetered(bool unmetered) {
	withAdapter([unmetered](struct mobile_adapter* adapter) {
		enum mobile_adapter_device device;
		bool tmp;
		mobile_config_get_device(adapter, &device, &tmp);
		mobile_config_set_device(adapter, device, unmetered);
	});
}

void MobileAdapterView::setDns(int which, const QString& text) {
	struct mobile_addr addr;
	if (!parseMobileAddr(text, MOBILE_DNS_PORT, &addr)) {
		getConfig();
		return;
	}
	withAdapter([which, &addr](struct mobile_adapter* adapter) {
		mobile_config_set_dns(adapter, &addr, (enum mobile_dns) which);
	});
}

void MobileAdapterView::setDns1() {
	setDns(MOBILE_DNS1, m_ui.setDns1->text());
}

void MobileAdapterView::setDns2() {
	setDns(MOBILE_DNS2, m_ui.setDns2->text());
}

void MobileAdapterView::setPort(int port) {
	withAdapter([port](struct mobile_adapter* adapter) {
		mobile_config_set_p2p_port(adapter, (unsigned) port);
	});
}

void MobileAdapterView::setRelay() {
	struct mobile_addr addr;
	if (!parseMobileAddr(m_ui.setRelay->text(), MOBILE_DEFAULT_RELAY_PORT, &addr)) {
		getConfig();
		return;
	}
	withAdapter([&addr](struct mobile_adapter* adapter) {
		mobile_config_set_relay(adapter, &addr);
	});
}

void MobileAdapterView::setToken() {
	QString qToken = m_ui.setToken->text();
	withAdapter([&qToken](struct mobile_adapter* adapter) {
		if (qToken.size() != MOBILE_RELAY_TOKEN_SIZE * 2) {
			mobile_config_set_relay_token(adapter, nullptr);
			return;
		}
		unsigned char token[MOBILE_RELAY_TOKEN_SIZE];
		for (int i = 0; i < MOBILE_RELAY_TOKEN_SIZE * 2; i += 2) {
			bool ok = false;
			token[i / 2] = qToken.mid(i, 2).toInt(&ok, 0x10);
			if (!ok) {
				mobile_config_set_relay_token(adapter, nullptr);
				return;
			}
		}
		mobile_config_set_relay_token(adapter, token);
	});
}

void MobileAdapterView::copyToken(bool checked) {
	UNUSED(checked);
	getConfig();
	QGuiApplication::clipboard()->setText(m_ui.setToken->text());
}

void MobileAdapterView::importConfig(bool checked) {
	UNUSED(checked);
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select config file"));
	if (filename.isEmpty()) {
		return;
	}

	m_controller->importMobileAdapterConfig(filename);
	getConfig();
}

void MobileAdapterView::getConfig() {
	CoreController::Interrupter interrupter(m_controller);
	struct mobile_adapter* adapter = this->adapter();
	if (!adapter) {
		return;
	}

	// Not a setting, but it belongs with the status: it is what a person
	// matches this computer against the account's device list by.
	char pairing[MOBILE_PAIRING_CODE_LEN];
	if (MobileAdapterGBPairingCode(adapterGB(), pairing, sizeof(pairing))) {
		m_ui.pairingCode->setText(QString::fromLatin1(pairing));
	} else {
		m_ui.pairingCode->setText(tr("Unavailable"));
	}

	enum mobile_adapter_device device;
	bool unmetered;
	mobile_config_get_device(adapter, &device, &unmetered);
	m_ui.setType->setCurrentIndex((int) device - MOBILE_ADAPTER_BLUE);
	m_ui.setUnmetered->setChecked(unmetered);

	struct mobile_addr addr;
	mobile_config_get_dns(adapter, &addr, MOBILE_DNS1);
	m_ui.setDns1->setText(mobileAddrToString(&addr, MOBILE_DNS_PORT));
	mobile_config_get_dns(adapter, &addr, MOBILE_DNS2);
	m_ui.setDns2->setText(mobileAddrToString(&addr, MOBILE_DNS_PORT));
	mobile_config_get_relay(adapter, &addr);
	m_ui.setRelay->setText(mobileAddrToString(&addr, MOBILE_DEFAULT_RELAY_PORT));

	unsigned p2pPort;
	mobile_config_get_p2p_port(adapter, &p2pPort);
	m_ui.setPort->setValue(p2pPort);

	QString token;
	unsigned char tokenGet[MOBILE_RELAY_TOKEN_SIZE];
	if (mobile_config_get_relay_token(adapter, tokenGet)) {
		for (int i = 0; i < MOBILE_RELAY_TOKEN_SIZE; ++i) {
			token += QString("%1").arg(tokenGet[i], 2, 0x10, QChar('0'));
		}
	}
	m_ui.setToken->setText(token);
}

void MobileAdapterView::advanceFrameCounter() {
	if (!m_ui.enableAdapter->isChecked()) {
		return;
	}
	static QString statusText = tr("Current status");
	static QString userNumber;
	static QString peerNumber;
	if (!m_controller->updateMobileAdapter(&statusText, &userNumber, &peerNumber)) {
		delete this;
		return;
	}
	m_ui.statusText->setText(statusText);
	m_ui.userNumber->setText(userNumber);
	m_ui.peerNumber->setText(peerNumber);
}

#endif
