/*
 * EdaTrayWindow.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifdef AIMON_EDA_BRIDGE_WITH_QT

#include "EdaTrayWindow.hxx"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QScrollArea>
#include <QUrl>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace aimon {

namespace {

void clearLayoutWidgets(QLayout* layout) {
    if (!layout) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            delete w;
        }
        if (QLayout* childLayout = item->layout()) {
            clearLayoutWidgets(childLayout);
        }
        delete item;
    }
}

QString progressBarStyleForPercent(double remainingPct) {
    QString barColor = "#00f2fe";
    if (remainingPct < 20.0) {
        barColor = "#ff4d4f";
    } else if (remainingPct < 50.0) {
        barColor = "#faad14";
    }
    return QString(
        "QProgressBar { border: 1px solid #1e293b; border-radius: 4px; "
        "background-color: #0f172a; text-align: center; color: #e2e8f0; font-weight: bold; min-height: 18px; } "
        "QProgressBar::chunk { background-color: %1; border-radius: 3px; }"
    ).arg(barColor);
}

QString spendProgressBarStyle(double usedPct) {
    QString barColor = "#10b981";
    if (usedPct >= 85.0) {
        barColor = "#ff4d4f";
    } else if (usedPct >= 60.0) {
        barColor = "#faad14";
    }
    return QString(
        "QProgressBar { border: 1px solid #1e293b; border-radius: 4px; "
        "background-color: #0f172a; text-align: center; color: #e2e8f0; font-weight: bold; min-height: 18px; } "
        "QProgressBar::chunk { background-color: %1; border-radius: 3px; }"
    ).arg(barColor);
}

} // anonymous namespace

EdaTrayWindow::EdaTrayWindow(EdaRpcServer& server,
                             const EdaBridgeAppSettings& initialSettings,
                             const std::string& configFilePath,
                             QWidget* parent)
    : QDialog(parent),
      _server(server),
      _settings(initialSettings),
      _configFilePath(configFilePath) {
    setWindowTitle("aimon");
    setMinimumSize(680, 620);
    resize(720, 660);

    setStyleSheet(
        "QDialog { background-color: #0a0e17; color: #e2e8f0; font-family: 'Segoe UI', sans-serif; } "
        "QTabWidget::pane { border: 1px solid #1e293b; background-color: #0f172a; border-radius: 6px; } "
        "QTabBar::tab { background-color: #1e293b; color: #94a3b8; padding: 8px 18px; "
        "border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 4px; font-weight: 600; } "
        "QTabBar::tab:selected { background-color: #0f172a; color: #00f2fe; border-bottom: 2px solid #00f2fe; } "
        "QGroupBox { border: 1px solid #1e293b; border-radius: 6px; margin-top: 12px; padding-top: 10px; "
        "color: #00f2fe; font-weight: bold; background-color: #0b1120; } "
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 6px; } "
        "QLabel { color: #e2e8f0; } "
        "QCheckBox { color: #e2e8f0; spacing: 8px; } "
        "QLineEdit, QSpinBox, QComboBox { background-color: #0a0e17; color: #f8fafc; "
        "border: 1px solid #334155; border-radius: 4px; padding: 4px 8px; } "
        "QPushButton { background-color: #1e293b; color: #f8fafc; border: 1px solid #00f2fe; "
        "border-radius: 4px; padding: 6px 14px; font-weight: 600; } "
        "QPushButton:hover { background-color: #0f172a; border-color: #38bdf8; color: #00f2fe; }"
    );

    buildUi();
    buildTrayIcon();
    syncControlsFromSettings();
    applyRuntimeToggles();

    _statusTimer = new QTimer(this);
    QObject::connect(_statusTimer, &QTimer::timeout, [this]() {
        refreshLiveStatus();
    });
    _statusTimer->start(2000);

    _quotaTimer = new QTimer(this);
    QObject::connect(_quotaTimer, &QTimer::timeout, [this]() {
        refreshAiQuotas(false);
    });
    _quotaTimer->start(30000);

    refreshLiveStatus();
    refreshAiQuotas(false);
}

EdaTrayWindow::~EdaTrayWindow() {
    if (_statusTimer) _statusTimer->stop();
    if (_quotaTimer) _quotaTimer->stop();
}

void EdaTrayWindow::showQuotasTab() {
    if (_tabWidget) {
        _tabWidget->setCurrentIndex(0);
    }
    showNormal();
    raise();
    activateWindow();
    refreshAiQuotas(false);
}

void EdaTrayWindow::showBridgesTab() {
    if (_tabWidget) {
        _tabWidget->setCurrentIndex(1);
    }
    showNormal();
    raise();
    activateWindow();
    refreshLiveStatus();
}

void EdaTrayWindow::closeEvent(QCloseEvent* event) {
    if (!_quitting && _trayIcon && _trayIcon->isVisible()) {
        hide();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

QIcon EdaTrayWindow::createStatusTrayIcon(bool healthy, bool activePlugin) const {
    QPixmap pix(32, 32);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Outer dark circle (#0a0e17)
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#0a0e17"));
    p.drawEllipse(1, 1, 30, 30);

    // Cyan ring (#00f2fe)
    QPen ringPen(QColor("#00f2fe"));
    ringPen.setWidth(2);
    p.setPen(ringPen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(3, 3, 26, 26);

    // Inner stylized 'A' glyph
    QFont font("Segoe UI", 12, QFont::Bold);
    p.setFont(font);
    p.setPen(QColor("#f8fafc"));
    p.drawText(QRect(0, 0, 32, 32), Qt::AlignCenter, "A");

    // Live status badge dot in bottom-right corner
    QColor dotColor = healthy ? (activePlugin ? QColor("#10b981") : QColor("#00f2fe"))
                              : QColor("#f59e0b");
    p.setPen(QPen(QColor("#0a0e17"), 1));
    p.setBrush(dotColor);
    p.drawEllipse(21, 21, 9, 9);

    p.end();
    return QIcon(pix);
}

void EdaTrayWindow::buildUi() {
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    // Top Header Banner
    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLbl = new QLabel("aimon — Unified AI Quota Monitor & EDA/CAD Bridge", this);
    titleLbl->setStyleSheet("font-size: 15px; font-weight: bold; color: #00f2fe;");
    QLabel* verLbl = new QLabel(QString("v%1").arg(QString::fromStdString(_settings.version)), this);
    verLbl->setStyleSheet("color: #94a3b8; font-size: 12px;");
    headerLayout->addWidget(titleLbl);
    headerLayout->addStretch(1);
    headerLayout->addWidget(verLbl);
    rootLayout->addLayout(headerLayout);

    _tabWidget = new QTabWidget(this);
    rootLayout->addWidget(_tabWidget, 1);

    // =========================================================================
    // TAB 1: AI Quotas
    // =========================================================================
    QWidget* quotasTab = new QWidget(this);
    QVBoxLayout* quotasLayout = new QVBoxLayout(quotasTab);
    quotasLayout->setContentsMargins(10, 10, 10, 10);
    quotasLayout->setSpacing(8);

    _quotaConnectionBanner = new QLabel("Connecting to aimon server...", quotasTab);
    _quotaConnectionBanner->setStyleSheet(
        "padding: 6px 10px; border-radius: 4px; background-color: #1e293b; color: #38bdf8; font-weight: 600;"
    );
    quotasLayout->addWidget(_quotaConnectionBanner);

    QScrollArea* quotaScroll = new QScrollArea(quotasTab);
    quotaScroll->setWidgetResizable(true);
    quotaScroll->setFrameShape(QFrame::NoFrame);
    QWidget* quotaScrollContent = new QWidget(quotaScroll);
    QVBoxLayout* scrollVBox = new QVBoxLayout(quotaScrollContent);
    scrollVBox->setContentsMargins(2, 2, 2, 2);
    scrollVBox->setSpacing(10);

    // 1. Google Antigravity Group
    QGroupBox* agGroup = new QGroupBox("Google Antigravity", quotaScrollContent);
    QVBoxLayout* agLayout = new QVBoxLayout(agGroup);
    _agHeaderLabel = new QLabel("Status: probing...", agGroup);
    _agCreditsLabel = new QLabel("Prompt Credits: -- | Flow Credits: --", agGroup);
    _agCreditsLabel->setStyleSheet("color: #94a3b8; font-size: 12px;");
    _agModelsContainer = new QWidget(agGroup);
    _agModelsLayout = new QVBoxLayout(_agModelsContainer);
    _agModelsLayout->setContentsMargins(0, 4, 0, 0);
    _agModelsLayout->setSpacing(6);
    agLayout->addWidget(_agHeaderLabel);
    agLayout->addWidget(_agCreditsLabel);
    agLayout->addWidget(_agModelsContainer);
    scrollVBox->addWidget(agGroup);

    // 2. Cursor Group
    QGroupBox* curGroup = new QGroupBox("Cursor", quotaScrollContent);
    QVBoxLayout* curLayout = new QVBoxLayout(curGroup);
    _cursorHeaderLabel = new QLabel("Status: probing...", curGroup);
    _cursorSpendLabel = new QLabel("Spend: $0.00", curGroup);
    _cursorProgressBar = new QProgressBar(curGroup);
    _cursorProgressBar->setRange(0, 100);
    _cursorProgressBar->setValue(0);
    _cursorProgressBar->setStyleSheet(spendProgressBarStyle(0.0));
    _cursorResetLabel = new QLabel("Cycle Reset: --", curGroup);
    _cursorResetLabel->setStyleSheet("color: #94a3b8; font-size: 12px;");
    curLayout->addWidget(_cursorHeaderLabel);
    curLayout->addWidget(_cursorSpendLabel);
    curLayout->addWidget(_cursorProgressBar);
    curLayout->addWidget(_cursorResetLabel);
    scrollVBox->addWidget(curGroup);

    // 3. Claude Code Group
    QGroupBox* claudeGroup = new QGroupBox("Claude Code", quotaScrollContent);
    QVBoxLayout* claudeLayout = new QVBoxLayout(claudeGroup);
    _claudeHeaderLabel = new QLabel("Status: probing...", claudeGroup);
    _claudeAccountsContainer = new QWidget(claudeGroup);
    _claudeAccountsLayout = new QVBoxLayout(_claudeAccountsContainer);
    _claudeAccountsLayout->setContentsMargins(0, 4, 0, 0);
    _claudeAccountsLayout->setSpacing(6);
    claudeLayout->addWidget(_claudeHeaderLabel);
    claudeLayout->addWidget(_claudeAccountsContainer);
    scrollVBox->addWidget(claudeGroup);

    scrollVBox->addStretch(1);
    quotaScroll->setWidget(quotaScrollContent);
    quotasLayout->addWidget(quotaScroll, 1);

    // Action Bar at bottom of AI Quotas tab
    QHBoxLayout* quotaActionLayout = new QHBoxLayout();
    _quotaLastUpdatedLabel = new QLabel("Last updated: never", quotasTab);
    _quotaLastUpdatedLabel->setStyleSheet("color: #94a3b8; font-size: 12px;");
    _btnRefreshQuotas = new QPushButton("Refresh Quotas Now", quotasTab);
    _btnOpenWebDashboard = new QPushButton("Open Web Dashboard", quotasTab);

    QObject::connect(_btnRefreshQuotas, &QPushButton::clicked, [this]() {
        refreshAiQuotas(true);
    });
    QObject::connect(_btnOpenWebDashboard, &QPushButton::clicked, [this]() {
        openWebDashboard();
    });

    quotaActionLayout->addWidget(_quotaLastUpdatedLabel);
    quotaActionLayout->addStretch(1);
    quotaActionLayout->addWidget(_btnRefreshQuotas);
    quotaActionLayout->addWidget(_btnOpenWebDashboard);
    quotasLayout->addLayout(quotaActionLayout);

    _tabWidget->addTab(quotasTab, "AI Quotas");

    // =========================================================================
    // TAB 2: EDA / CAD Bridges & Settings
    // =========================================================================
    QWidget* bridgesTab = new QWidget(this);
    QVBoxLayout* bridgesLayout = new QVBoxLayout(bridgesTab);
    bridgesLayout->setContentsMargins(10, 10, 10, 10);
    bridgesLayout->setSpacing(8);

    // Live Bridge Status Group
    QGroupBox* statusGroup = new QGroupBox("Live Bridge & Companion Plugin Telemetry", bridgesTab);
    QGridLayout* statusGrid = new QGridLayout(statusGroup);
    _lblRpcStatus = new QLabel("RPC Listener: ...", statusGroup);
    _lblGatewayStatus = new QLabel("aimon Gateway: ...", statusGroup);
    _lblEasyEdaStatus = new QLabel("EasyEDA Pro: ...", statusGroup);
    _lblKiCadStatus = new QLabel("KiCad: ...", statusGroup);
    _lblFreeCadStatus = new QLabel("FreeCAD: ...", statusGroup);
    statusGrid->addWidget(_lblRpcStatus, 0, 0);
    statusGrid->addWidget(_lblGatewayStatus, 0, 1);
    statusGrid->addWidget(_lblEasyEdaStatus, 1, 0);
    statusGrid->addWidget(_lblKiCadStatus, 1, 1);
    statusGrid->addWidget(_lblFreeCadStatus, 2, 0, 1, 2);
    bridgesLayout->addWidget(statusGroup);

    // Persisted Checkboxes Group
    QGroupBox* togglesGroup = new QGroupBox("Persisted Bridge & System Tray Options", bridgesTab);
    QGridLayout* togglesGrid = new QGridLayout(togglesGroup);
    _chkEnableEasyEda = new QCheckBox("Enable EasyEDA Pro Bridge", togglesGroup);
    _chkEnableKiCad = new QCheckBox("Enable KiCad Bridge", togglesGroup);
    _chkEnableFreeCad = new QCheckBox("Enable FreeCAD Bridge", togglesGroup);
    _chkAutoDeployPlugins = new QCheckBox("Auto-deploy companion plugins on startup", togglesGroup);
    _chkStartMinimized = new QCheckBox("Start minimized to Windows System Tray", togglesGroup);
    _chkLaunchAtLogin = new QCheckBox("Launch automatically at Windows login", togglesGroup);
    _chkDisallowExports = new QCheckBox("Lock manufacturing & file exports (Read-only guard)", togglesGroup);

    togglesGrid->addWidget(_chkEnableEasyEda, 0, 0);
    togglesGrid->addWidget(_chkEnableKiCad, 0, 1);
    togglesGrid->addWidget(_chkEnableFreeCad, 1, 0);
    togglesGrid->addWidget(_chkAutoDeployPlugins, 1, 1);
    togglesGrid->addWidget(_chkStartMinimized, 2, 0);
    togglesGrid->addWidget(_chkLaunchAtLogin, 2, 1);
    togglesGrid->addWidget(_chkDisallowExports, 3, 0, 1, 2);
    bridgesLayout->addWidget(togglesGroup);

    auto onInstantToggleChanged = [this](bool) {
        if (_syncingUi) return;
        collectSettingsFromControls();
        applyRuntimeToggles();
        std::string err;
        EdaBridgeAppConfig::saveToFile(_configFilePath, _settings, err);
        refreshLiveStatus();
    };
    QObject::connect(_chkEnableEasyEda, &QCheckBox::toggled, onInstantToggleChanged);
    QObject::connect(_chkEnableKiCad, &QCheckBox::toggled, onInstantToggleChanged);
    QObject::connect(_chkEnableFreeCad, &QCheckBox::toggled, onInstantToggleChanged);
    QObject::connect(_chkDisallowExports, &QCheckBox::toggled, onInstantToggleChanged);

    // Connection & Paths Group
    QGroupBox* connGroup = new QGroupBox("aimon Server & Bridge Connection Settings", bridgesTab);
    QGridLayout* connGrid = new QGridLayout(connGroup);

    _edtAimonHost = new QLineEdit(connGroup);
    _spnAimonWebPort = new QSpinBox(connGroup);
    _spnAimonWebPort->setRange(1, 65535);
    _spnGatewayPort = new QSpinBox(connGroup);
    _spnGatewayPort->setRange(1, 65535);

    _cmbConnectionMode = new QComboBox(connGroup);
    _cmbConnectionMode->addItem("bidirectional");
    _cmbConnectionMode->addItem("listen");
    _cmbConnectionMode->addItem("gateway");

    _edtListenHost = new QLineEdit(connGroup);
    _spnRpcPort = new QSpinBox(connGroup);
    _spnRpcPort->setRange(1, 65535);
    _spnPluginPort = new QSpinBox(connGroup);
    _spnPluginPort->setRange(1, 65535);

    _edtAuthSecret = new QLineEdit(connGroup);
    _edtAuthSecret->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    _edtAllowedRoot = new QLineEdit(connGroup);
    _edtKiCadBin = new QLineEdit(connGroup);
    _edtFreeCadBin = new QLineEdit(connGroup);

    connGrid->addWidget(new QLabel("aimon Host:", connGroup), 0, 0);
    connGrid->addWidget(_edtAimonHost, 0, 1);
    connGrid->addWidget(new QLabel("Web Port:", connGroup), 0, 2);
    connGrid->addWidget(_spnAimonWebPort, 0, 3);

    connGrid->addWidget(new QLabel("Mode:", connGroup), 1, 0);
    connGrid->addWidget(_cmbConnectionMode, 1, 1);
    connGrid->addWidget(new QLabel("Gateway Port:", connGroup), 1, 2);
    connGrid->addWidget(_spnGatewayPort, 1, 3);

    connGrid->addWidget(new QLabel("Listen Host:", connGroup), 2, 0);
    connGrid->addWidget(_edtListenHost, 2, 1);
    connGrid->addWidget(new QLabel("RPC Port:", connGroup), 2, 2);
    connGrid->addWidget(_spnRpcPort, 2, 3);

    connGrid->addWidget(new QLabel("Plugin Port:", connGroup), 3, 0);
    connGrid->addWidget(_spnPluginPort, 3, 1);
    connGrid->addWidget(new QLabel("Auth Secret:", connGroup), 3, 2);
    connGrid->addWidget(_edtAuthSecret, 3, 3);

    connGrid->addWidget(new QLabel("Workspace Root:", connGroup), 4, 0);
    connGrid->addWidget(_edtAllowedRoot, 4, 1, 1, 3);

    connGrid->addWidget(new QLabel("kicad-cli Path:", connGroup), 5, 0);
    connGrid->addWidget(_edtKiCadBin, 5, 1, 1, 3);

    connGrid->addWidget(new QLabel("FreeCADCmd Path:", connGroup), 6, 0);
    connGrid->addWidget(_edtFreeCadBin, 6, 1, 1, 3);

    bridgesLayout->addWidget(connGroup);

    _lblSettingsStatus = new QLabel(
        QString("Config file: %1").arg(QString::fromStdString(_configFilePath)), bridgesTab
    );
    _lblSettingsStatus->setStyleSheet("color: #94a3b8; font-size: 11px;");
    bridgesLayout->addWidget(_lblSettingsStatus);

    QHBoxLayout* bridgeButtons = new QHBoxLayout();
    QPushButton* btnSave = new QPushButton("Apply & Save Settings", bridgesTab);
    QPushButton* btnDeploy = new QPushButton("Deploy KiCad & FreeCAD Plugins Now", bridgesTab);
    QPushButton* btnExportEext = new QPushButton("Export EasyEDA .eext Extension...", bridgesTab);

    QObject::connect(btnSave, &QPushButton::clicked, [this]() {
        applyAndSaveSettings();
    });
    QObject::connect(btnDeploy, &QPushButton::clicked, [this]() {
        onDeployPluginsClicked();
    });
    QObject::connect(btnExportEext, &QPushButton::clicked, [this]() {
        onExportEasyEdaExtensionClicked();
    });

    bridgeButtons->addWidget(btnSave);
    bridgeButtons->addWidget(btnDeploy);
    bridgeButtons->addWidget(btnExportEext);
    bridgesLayout->addLayout(bridgeButtons);

    _tabWidget->addTab(bridgesTab, "EDA / CAD Bridges & Settings");
}

void EdaTrayWindow::buildTrayIcon() {
    _trayIcon = new QSystemTrayIcon(this);
    _trayIcon->setIcon(createStatusTrayIcon(true, false));
    _trayIcon->setToolTip("aimon");
    setWindowIcon(_trayIcon->icon());

    _trayMenu = new QMenu(this);
    _trayMenu->setStyleSheet(
        "QMenu { background-color: #0f172a; color: #e2e8f0; border: 1px solid #334155; padding: 4px; } "
        "QMenu::item { padding: 5px 22px; } "
        "QMenu::item:selected { background-color: #1e293b; color: #00f2fe; } "
        "QMenu::separator { height: 1px; background: #334155; margin: 4px 0; }"
    );

    QAction* actHeader = _trayMenu->addAction("aimon — AI Quotas & EDA Bridges");
    actHeader->setEnabled(false);
    _actSummaryAg = _trayMenu->addAction("Antigravity: probing...");
    _actSummaryAg->setEnabled(false);
    _actSummaryCursor = _trayMenu->addAction("Cursor: probing...");
    _actSummaryCursor->setEnabled(false);
    _actSummaryClaude = _trayMenu->addAction("Claude Code: probing...");
    _actSummaryClaude->setEnabled(false);
    _actSummaryBridges = _trayMenu->addAction("Bridges: EasyEDA / KiCad / FreeCAD");
    _actSummaryBridges->setEnabled(false);

    _trayMenu->addSeparator();

    QAction* actShowQuotas = _trayMenu->addAction("Show AI Quotas...");
    QObject::connect(actShowQuotas, &QAction::triggered, [this]() {
        showQuotasTab();
    });

    QAction* actShowBridges = _trayMenu->addAction("EDA / CAD Bridge Settings...");
    QObject::connect(actShowBridges, &QAction::triggered, [this]() {
        showBridgesTab();
    });

    QAction* actRefreshQuotas = _trayMenu->addAction("Refresh AI Quotas Now");
    QObject::connect(actRefreshQuotas, &QAction::triggered, [this]() {
        refreshAiQuotas(true);
    });

    QAction* actOpenWeb = _trayMenu->addAction("Open Web Dashboard in Browser");
    QObject::connect(actOpenWeb, &QAction::triggered, [this]() {
        openWebDashboard();
    });

    _trayMenu->addSeparator();

    _actTrayEasyEda = _trayMenu->addAction("EasyEDA Pro Bridge");
    _actTrayEasyEda->setCheckable(true);
    QObject::connect(_actTrayEasyEda, &QAction::toggled, [this](bool checked) {
        if (_syncingUi) return;
        _chkEnableEasyEda->setChecked(checked);
    });

    _actTrayKiCad = _trayMenu->addAction("KiCad Bridge");
    _actTrayKiCad->setCheckable(true);
    QObject::connect(_actTrayKiCad, &QAction::toggled, [this](bool checked) {
        if (_syncingUi) return;
        _chkEnableKiCad->setChecked(checked);
    });

    _actTrayFreeCad = _trayMenu->addAction("FreeCAD Bridge");
    _actTrayFreeCad->setCheckable(true);
    QObject::connect(_actTrayFreeCad, &QAction::toggled, [this](bool checked) {
        if (_syncingUi) return;
        _chkEnableFreeCad->setChecked(checked);
    });

    _actTrayLockExports = _trayMenu->addAction("Lock Exports (Read-Only Guard)");
    _actTrayLockExports->setCheckable(true);
    QObject::connect(_actTrayLockExports, &QAction::toggled, [this](bool checked) {
        if (_syncingUi) return;
        _chkDisallowExports->setChecked(checked);
    });

    _trayMenu->addSeparator();

    QAction* actDeployPlugins = _trayMenu->addAction("Deploy Desktop Plugins Now");
    QObject::connect(actDeployPlugins, &QAction::triggered, [this]() {
        onDeployPluginsClicked();
    });

    QAction* actExportEext = _trayMenu->addAction("Export EasyEDA .eext Extension...");
    QObject::connect(actExportEext, &QAction::triggered, [this]() {
        onExportEasyEdaExtensionClicked();
    });

    _trayMenu->addSeparator();

    QAction* actQuit = _trayMenu->addAction("Quit aimon");
    QObject::connect(actQuit, &QAction::triggered, [this]() {
        quitApplication();
    });

    _trayIcon->setContextMenu(_trayMenu);

    QObject::connect(_trayIcon, &QSystemTrayIcon::activated,
                     [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger ||
            reason == QSystemTrayIcon::DoubleClick) {
            showQuotasTab();
        }
    });

    _trayIcon->show();
}

void EdaTrayWindow::syncControlsFromSettings() {
    _syncingUi = true;
    _chkEnableEasyEda->setChecked(_settings.enableEasyEda);
    _chkEnableKiCad->setChecked(_settings.enableKiCad);
    _chkEnableFreeCad->setChecked(_settings.enableFreeCad);
    _chkAutoDeployPlugins->setChecked(_settings.autoDeployPlugins);
    _chkStartMinimized->setChecked(_settings.startMinimizedToTray);
    _chkLaunchAtLogin->setChecked(_settings.launchAtLogin);
    _chkDisallowExports->setChecked(_settings.disallowExports);

    if (_actTrayEasyEda) _actTrayEasyEda->setChecked(_settings.enableEasyEda);
    if (_actTrayKiCad) _actTrayKiCad->setChecked(_settings.enableKiCad);
    if (_actTrayFreeCad) _actTrayFreeCad->setChecked(_settings.enableFreeCad);
    if (_actTrayLockExports) _actTrayLockExports->setChecked(_settings.disallowExports);

    _edtAimonHost->setText(QString::fromStdString(_settings.aimonHost));
    _spnAimonWebPort->setValue(_settings.aimonWebPort);
    _spnGatewayPort->setValue(_settings.gatewayPort);

    int modeIdx = _cmbConnectionMode->findText(QString::fromStdString(_settings.connectionMode));
    if (modeIdx >= 0) {
        _cmbConnectionMode->setCurrentIndex(modeIdx);
    }

    _edtListenHost->setText(QString::fromStdString(_settings.listenHost));
    _spnRpcPort->setValue(_settings.rpcPort);
    _spnPluginPort->setValue(_settings.pluginPort);
    _edtAuthSecret->setText(QString::fromStdString(_settings.authSecret));
    _edtAllowedRoot->setText(QString::fromStdString(_settings.allowedRoot));
    _edtKiCadBin->setText(QString::fromStdString(_settings.kicadBinaryPath));
    _edtFreeCadBin->setText(QString::fromStdString(_settings.freecadBinaryPath));
    _syncingUi = false;
}

void EdaTrayWindow::collectSettingsFromControls() {
    _settings.enableEasyEda = _chkEnableEasyEda->isChecked();
    _settings.enableKiCad = _chkEnableKiCad->isChecked();
    _settings.enableFreeCad = _chkEnableFreeCad->isChecked();
    _settings.autoDeployPlugins = _chkAutoDeployPlugins->isChecked();
    _settings.startMinimizedToTray = _chkStartMinimized->isChecked();
    _settings.launchAtLogin = _chkLaunchAtLogin->isChecked();
    _settings.disallowExports = _chkDisallowExports->isChecked();

    _settings.aimonHost = _edtAimonHost->text().trimmed().toStdString();
    if (_settings.aimonHost.empty()) {
        _settings.aimonHost = "127.0.0.1";
    }
    _settings.aimonWebPort = _spnAimonWebPort->value();
    _settings.gatewayPort = _spnGatewayPort->value();
    _settings.connectionMode = _cmbConnectionMode->currentText().toStdString();
    _settings.listenHost = _edtListenHost->text().trimmed().toStdString();
    _settings.rpcPort = _spnRpcPort->value();
    _settings.pluginPort = _spnPluginPort->value();
    _settings.authSecret = _edtAuthSecret->text().toStdString();
    _settings.allowedRoot = _edtAllowedRoot->text().trimmed().toStdString();
    _settings.kicadBinaryPath = _edtKiCadBin->text().trimmed().toStdString();
    _settings.freecadBinaryPath = _edtFreeCadBin->text().trimmed().toStdString();
}

void EdaTrayWindow::applyRuntimeToggles() {
    _server.setBridgeEnabled("easyeda", _settings.enableEasyEda);
    _server.setBridgeEnabled("kicad", _settings.enableKiCad);
    _server.setBridgeEnabled("freecad", _settings.enableFreeCad);
    _server.setDisallowExports(_settings.disallowExports);

    _syncingUi = true;
    if (_actTrayEasyEda) _actTrayEasyEda->setChecked(_settings.enableEasyEda);
    if (_actTrayKiCad) _actTrayKiCad->setChecked(_settings.enableKiCad);
    if (_actTrayFreeCad) _actTrayFreeCad->setChecked(_settings.enableFreeCad);
    if (_actTrayLockExports) _actTrayLockExports->setChecked(_settings.disallowExports);
    _syncingUi = false;
}

void EdaTrayWindow::updateWindowsRunAtLoginRegistry(bool enable) {
#ifdef _WIN32
    HKEY hKey = nullptr;
    const wchar_t* runSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, runSubKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH] = {0};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring cmd = L"\"";
            cmd += exePath;
            cmd += L"\"";
            RegSetValueExW(
                hKey,
                L"aimon",
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(cmd.c_str()),
                static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))
            );
        } else {
            RegDeleteValueW(hKey, L"aimon");
        }
        RegCloseKey(hKey);
    }
#else
    (void)enable;
#endif
}

void EdaTrayWindow::applyAndSaveSettings() {
    collectSettingsFromControls();
    applyRuntimeToggles();
    updateWindowsRunAtLoginRegistry(_settings.launchAtLogin);

    if (_settings.connectionMode == "gateway" || _settings.connectionMode == "bidirectional") {
        if (!_server.isGatewayConnected()) {
            _server.connectToGateway(_settings.aimonHost, _settings.gatewayPort);
        }
    } else {
        if (_server.isGatewayConnected()) {
            _server.disconnectGateway();
        }
    }

    std::string err;
    if (EdaBridgeAppConfig::saveToFile(_configFilePath, _settings, err)) {
        _lblSettingsStatus->setText(
            QString("Saved settings to %1").arg(QString::fromStdString(_configFilePath))
        );
        _lblSettingsStatus->setStyleSheet("color: #10b981; font-size: 11px;");
    } else {
        _lblSettingsStatus->setText(
            QString("Error saving settings: %1").arg(QString::fromStdString(err))
        );
        _lblSettingsStatus->setStyleSheet("color: #ff4d4f; font-size: 11px;");
    }

    refreshLiveStatus();
    refreshAiQuotas(false);
}

void EdaTrayWindow::refreshLiveStatus() {
    const bool rpcRunning = _server.isRunning();
    const bool gwConn = _server.isGatewayConnected();
    const bool eeConn = _server.isPluginConnected("easyeda");
    const bool kcConn = _server.isPluginConnected("kicad");
    const bool fcConn = _server.isPluginConnected("freecad");

    _lblRpcStatus->setText(
        rpcRunning
            ? QString("RPC Listener: ONLINE (:%1 / plugin :%2)")
                  .arg(_server.boundPort())
                  .arg(_server.boundPluginPort())
            : QString("RPC Listener: OFFLINE")
    );
    _lblRpcStatus->setStyleSheet(rpcRunning ? "color: #10b981; font-weight: 600;"
                                            : "color: #ff4d4f; font-weight: 600;");

    _lblGatewayStatus->setText(
        gwConn
            ? QString("aimon Gateway: CONNECTED (%1:%2)")
                  .arg(QString::fromStdString(_settings.aimonHost))
                  .arg(_settings.gatewayPort)
            : QString("aimon Gateway: STANDBY (%1:%2)")
                  .arg(QString::fromStdString(_settings.aimonHost))
                  .arg(_settings.gatewayPort)
    );
    _lblGatewayStatus->setStyleSheet(gwConn ? "color: #10b981; font-weight: 600;"
                                            : "color: #94a3b8;");

    auto formatAppStatus = [](const char* label, bool enabled, bool pluginLive) -> QString {
        if (!enabled) {
            return QString("%1: DISABLED").arg(label);
        }
        return pluginLive ? QString("%1: LIVE GUI CONNECTED").arg(label)
                          : QString("%1: READY (Standby / CLI)").arg(label);
    };

    _lblEasyEdaStatus->setText(formatAppStatus("EasyEDA Pro", _settings.enableEasyEda, eeConn));
    _lblEasyEdaStatus->setStyleSheet(
        !_settings.enableEasyEda ? "color: #64748b;" : (eeConn ? "color: #00f2fe; font-weight: 600;" : "color: #e2e8f0;")
    );

    _lblKiCadStatus->setText(formatAppStatus("KiCad", _settings.enableKiCad, kcConn));
    _lblKiCadStatus->setStyleSheet(
        !_settings.enableKiCad ? "color: #64748b;" : (kcConn ? "color: #00f2fe; font-weight: 600;" : "color: #e2e8f0;")
    );

    _lblFreeCadStatus->setText(formatAppStatus("FreeCAD", _settings.enableFreeCad, fcConn));
    _lblFreeCadStatus->setStyleSheet(
        !_settings.enableFreeCad ? "color: #64748b;" : (fcConn ? "color: #00f2fe; font-weight: 600;" : "color: #e2e8f0;")
    );

    const bool anyPlugin = eeConn || kcConn || fcConn;
    const bool healthy = rpcRunning && _lastQuotaSnap.reachable;
    if (_trayIcon) {
        _trayIcon->setIcon(createStatusTrayIcon(healthy, anyPlugin));
        const std::string tooltip = EdaBridgeAppConfig::formatTrayTooltipSummary(
            _lastQuotaSnap, eeConn, kcConn, fcConn
        );
        _trayIcon->setToolTip(QString::fromStdString(tooltip));
    }

    if (_actSummaryBridges) {
        _actSummaryBridges->setText(
            QString("Bridges: EE=%1 | KC=%2 | FC=%3")
                .arg(!_settings.enableEasyEda ? "Off" : (eeConn ? "Live" : "Ready"))
                .arg(!_settings.enableKiCad ? "Off" : (kcConn ? "Live" : "Ready"))
                .arg(!_settings.enableFreeCad ? "Off" : (fcConn ? "Live" : "Ready"))
        );
    }
}

void EdaTrayWindow::refreshAiQuotas(bool triggerRemoteRefresh) {
    AimonQuotaSnapshot snap;
    EdaBridgeAppConfig::fetchQuotaSnapshot(
        _settings.aimonHost, _settings.aimonWebPort, triggerRemoteRefresh, snap, 2500
    );
    _lastQuotaSnap = snap;
    populateQuotasUi(snap);
    refreshLiveStatus();
}

void EdaTrayWindow::populateQuotasUi(const AimonQuotaSnapshot& snap) {
    if (!snap.reachable) {
        _quotaConnectionBanner->setText(
            QString("aimon server unreachable at http://%1:%2 — %3")
                .arg(QString::fromStdString(_settings.aimonHost))
                .arg(_settings.aimonWebPort)
                .arg(QString::fromStdString(snap.errorMessage))
        );
        _quotaConnectionBanner->setStyleSheet(
            "padding: 6px 10px; border-radius: 4px; background-color: #3f1d24; color: #ff4d4f; font-weight: 600;"
        );
    } else {
        _quotaConnectionBanner->setText(
            QString("Connected to aimon server at http://%1:%2")
                .arg(QString::fromStdString(_settings.aimonHost))
                .arg(_settings.aimonWebPort)
        );
        _quotaConnectionBanner->setStyleSheet(
            "padding: 6px 10px; border-radius: 4px; background-color: #064e3b; color: #10b981; font-weight: 600;"
        );
    }

    // 1. Antigravity
    clearLayoutWidgets(_agModelsLayout);
    if (snap.agRunning) {
        _agHeaderLabel->setText(
            QString("Status: ACTIVE  |  Plan: %1").arg(QString::fromStdString(snap.agPlanTier))
        );
        _agHeaderLabel->setStyleSheet("color: #10b981; font-weight: 600;");
        _agCreditsLabel->setText(
            QString("Prompt Credits: %1   |   Flow Credits: %2")
                .arg(snap.agPromptCredits)
                .arg(snap.agFlowCredits)
        );

        double minPct = 100.0;
        std::string minModel;
        for (const auto& m : snap.agModels) {
            if (m.remainingPct < minPct || minModel.empty()) {
                minPct = m.remainingPct;
                minModel = m.name;
            }
            QWidget* rowWidget = new QWidget(_agModelsContainer);
            QHBoxLayout* rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(8);

            QLabel* nameLbl = new QLabel(QString::fromStdString(m.name), rowWidget);
            nameLbl->setMinimumWidth(210);
            QProgressBar* bar = new QProgressBar(rowWidget);
            bar->setRange(0, 100);
            bar->setValue(static_cast<int>(m.remainingPct + 0.5));
            bar->setFormat(QString("%1%").arg(static_cast<int>(m.remainingPct + 0.5)));
            bar->setStyleSheet(progressBarStyleForPercent(m.remainingPct));

            QLabel* resetLbl = new QLabel(
                m.resetSeconds > 0
                    ? QString("resets in %1").arg(QString::fromStdString(formatDurationCompact(m.resetSeconds)))
                    : QString("ready"),
                rowWidget
            );
            resetLbl->setMinimumWidth(105);
            resetLbl->setStyleSheet("color: #94a3b8; font-size: 11px;");

            rowLayout->addWidget(nameLbl);
            rowLayout->addWidget(bar, 1);
            rowLayout->addWidget(resetLbl);
            _agModelsLayout->addWidget(rowWidget);
        }
        if (_actSummaryAg) {
            if (!minModel.empty()) {
                _actSummaryAg->setText(
                    QString("Antigravity (%1): %2% min (%3)")
                        .arg(QString::fromStdString(snap.agPlanTier))
                        .arg(static_cast<int>(minPct + 0.5))
                        .arg(QString::fromStdString(minModel))
                );
            } else {
                _actSummaryAg->setText(
                    QString("Antigravity (%1): Online").arg(QString::fromStdString(snap.agPlanTier))
                );
            }
        }
    } else {
        _agHeaderLabel->setText(
            snap.agError.empty()
                ? QString("Status: Offline / No active Antigravity language_server")
                : QString("Status: %1").arg(QString::fromStdString(snap.agError))
        );
        _agHeaderLabel->setStyleSheet("color: #94a3b8;");
        _agCreditsLabel->setText("Prompt Credits: -- | Flow Credits: --");
        if (_actSummaryAg) {
            _actSummaryAg->setText("Antigravity: Offline");
        }
    }

    // 2. Cursor
    if (snap.cursorAuthenticated) {
        _cursorHeaderLabel->setText(
            QString("Status: AUTHENTICATED  |  Plan: %1  |  Mode: %2")
                .arg(QString::fromStdString(snap.cursorPlanTier))
                .arg(QString::fromStdString(snap.cursorUsageMode))
        );
        _cursorHeaderLabel->setStyleSheet("color: #10b981; font-weight: 600;");

        std::ostringstream spendOss;
        spendOss << std::fixed << std::setprecision(2)
                 << "Total Spend: $" << snap.cursorTotalSpendUsd
                 << " (On-Demand: $" << snap.cursorOnDemandSpendUsd << ")"
                 << "  |  Fast Requests: " << snap.cursorRequestsUsed
                 << " / " << snap.cursorRequestsLimit;
        _cursorSpendLabel->setText(QString::fromStdString(spendOss.str()));

        double usedPct = 0.0;
        if (snap.cursorRequestsLimit > 0) {
            usedPct = (100.0 * static_cast<double>(snap.cursorRequestsUsed)) /
                      static_cast<double>(snap.cursorRequestsLimit);
            if (usedPct > 100.0) usedPct = 100.0;
            _cursorProgressBar->setValue(static_cast<int>(usedPct + 0.5));
            _cursorProgressBar->setFormat(
                QString("%1 / %2 requests (%3%)")
                    .arg(snap.cursorRequestsUsed)
                    .arg(snap.cursorRequestsLimit)
                    .arg(static_cast<int>(usedPct + 0.5))
            );
        } else {
            _cursorProgressBar->setValue(0);
            _cursorProgressBar->setFormat(
                QString("$%1 cycle spend").arg(snap.cursorTotalSpendUsd, 0, 'f', 2)
            );
        }
        _cursorProgressBar->setStyleSheet(spendProgressBarStyle(usedPct));
        _cursorResetLabel->setText(
            QString("Cycle Reset: %1").arg(
                snap.cursorCycleResetIso.empty()
                    ? QString("--")
                    : QString::fromStdString(snap.cursorCycleResetIso)
            )
        );
        if (_actSummaryCursor) {
            _actSummaryCursor->setText(
                QString("Cursor (%1): $%2 | %3/%4 req")
                    .arg(QString::fromStdString(snap.cursorPlanTier))
                    .arg(snap.cursorTotalSpendUsd, 0, 'f', 2)
                    .arg(snap.cursorRequestsUsed)
                    .arg(snap.cursorRequestsLimit)
            );
        }
    } else {
        _cursorHeaderLabel->setText(
            snap.cursorError.empty()
                ? QString("Status: Unauthenticated / Offline")
                : QString("Status: %1").arg(QString::fromStdString(snap.cursorError))
        );
        _cursorHeaderLabel->setStyleSheet("color: #94a3b8;");
        _cursorSpendLabel->setText("Spend: --");
        _cursorProgressBar->setValue(0);
        _cursorProgressBar->setFormat("N/A");
        _cursorResetLabel->setText("Cycle Reset: --");
        if (_actSummaryCursor) {
            _actSummaryCursor->setText("Cursor: Offline");
        }
    }

    // 3. Claude Code
    clearLayoutWidgets(_claudeAccountsLayout);
    if (snap.claudeEnabled && !snap.claudeAccounts.empty()) {
        _claudeHeaderLabel->setText(
            QString("Active Accounts: %1").arg(snap.claudeAccounts.size())
        );
        _claudeHeaderLabel->setStyleSheet("color: #10b981; font-weight: 600;");

        for (const auto& acct : snap.claudeAccounts) {
            QFrame* card = new QFrame(_claudeAccountsContainer);
            card->setStyleSheet(
                "QFrame { border: 1px solid #1e293b; border-radius: 4px; background-color: #0a0e17; padding: 4px; }"
            );
            QVBoxLayout* cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(6, 6, 6, 6);
            cardLayout->setSpacing(4);

            std::ostringstream line1;
            line1 << acct.name << " [" << acct.tier << "] — "
                  << "5h: " << formatTokensCompact(acct.tokens5h)
                  << " ($" << std::fixed << std::setprecision(2) << acct.cost5hUsd << ")  |  "
                  << "7d: " << formatTokensCompact(acct.tokens7d)
                  << " ($" << acct.cost7dUsd << ")  |  "
                  << "30d: " << formatTokensCompact(acct.tokensWindow)
                  << " ($" << acct.costWindowUsd << ")";
            QLabel* lblSummary = new QLabel(QString::fromStdString(line1.str()), card);
            cardLayout->addWidget(lblSummary);

            if (acct.cycleConfigured && acct.spendLimitUsd > 0.0) {
                QProgressBar* bar = new QProgressBar(card);
                bar->setRange(0, 100);
                double pct = acct.estPctOfLimit;
                if (pct < 0.0) pct = 0.0;
                if (pct > 100.0) pct = 100.0;
                bar->setValue(static_cast<int>(pct + 0.5));
                bar->setFormat(
                    QString("Cycle: $%1 / $%2 (%3%)")
                        .arg(acct.costCycleUsd, 0, 'f', 2)
                        .arg(acct.spendLimitUsd, 0, 'f', 2)
                        .arg(static_cast<int>(pct + 0.5))
                );
                bar->setStyleSheet(spendProgressBarStyle(pct));
                cardLayout->addWidget(bar);
            }
            _claudeAccountsLayout->addWidget(card);
        }

        const auto& first = snap.claudeAccounts.front();
        if (_actSummaryClaude) {
            if (first.cycleConfigured && first.spendLimitUsd > 0.0) {
                _actSummaryClaude->setText(
                    QString("Claude (%1): $%2 / $%3")
                        .arg(QString::fromStdString(first.name))
                        .arg(first.costCycleUsd, 0, 'f', 2)
                        .arg(first.spendLimitUsd, 0, 'f', 0)
                );
            } else {
                _actSummaryClaude->setText(
                    QString("Claude (%1): 5h $%2 | 7d $%3")
                        .arg(QString::fromStdString(first.name))
                        .arg(first.cost5hUsd, 0, 'f', 2)
                        .arg(first.cost7dUsd, 0, 'f', 2)
                );
            }
        }
    } else {
        _claudeHeaderLabel->setText("Status: No Claude Code accounts reported");
        _claudeHeaderLabel->setStyleSheet("color: #94a3b8;");
        if (_actSummaryClaude) {
            _actSummaryClaude->setText("Claude Code: N/A");
        }
    }

    QString tsStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    _quotaLastUpdatedLabel->setText(QString("Last updated: %1").arg(tsStr));
}

void EdaTrayWindow::onDeployPluginsClicked() {
    std::string appData;
#ifdef _WIN32
    if (const char* env = std::getenv("APPDATA")) {
        appData = env;
    }
#else
    if (const char* env = std::getenv("HOME")) {
        appData = std::string(env) + "/.config";
    }
#endif
    if (appData.empty()) {
        QMessageBox::warning(this, "aimon — Deploy Plugins",
                             "Could not determine %APPDATA% directory.");
        return;
    }

    std::string kcDir;
    std::string fcDir;
    std::string err1;
    std::string err2;
    const bool okKc = EdaBridgeAppConfig::deployKiCadPlugin(appData, kcDir, err1);
    const bool okFc = EdaBridgeAppConfig::deployFreeCadPlugin(appData, fcDir, err2);

    fs::path eextPath = fs::path(_configFilePath).parent_path() / "aimon-easyeda-bridge.eext";
    std::string err3;
    const bool okEe = EdaBridgeAppConfig::writeEasyEdaEextFile(eextPath.string(), err3);

    if (okKc && okFc && okEe) {
        QMessageBox::information(
            this,
            "aimon — Companion Plugins Deployed",
            QString("Successfully deployed companion plugins:\n\n"
                    "• KiCad Plugin:\n  %1\n\n"
                    "• FreeCAD Plugin:\n  %2\n\n"
                    "• EasyEDA Pro Extension:\n  %3")
                .arg(QString::fromStdString(kcDir))
                .arg(QString::fromStdString(fcDir))
                .arg(QString::fromStdString(eextPath.string()))
        );
    } else {
        QMessageBox::warning(
            this,
            "aimon — Plugin Deployment Warning",
            QString("Plugin deployment reported issues:\nKiCad: %1\nFreeCAD: %2\nEasyEDA: %3")
                .arg(QString::fromStdString(okKc ? "OK" : err1))
                .arg(QString::fromStdString(okFc ? "OK" : err2))
                .arg(QString::fromStdString(okEe ? "OK" : err3))
        );
    }
}

void EdaTrayWindow::onExportEasyEdaExtensionClicked() {
    QString defaultPath = QString::fromStdString(
        (fs::path(_configFilePath).parent_path() / "aimon-easyeda-bridge.eext").string()
    );
    QString target = QFileDialog::getSaveFileName(
        this,
        "Export EasyEDA Pro Extension (.eext)",
        defaultPath,
        "EasyEDA Pro Extension (*.eext);;All Files (*)"
    );
    if (target.isEmpty()) {
        return;
    }
    std::string err;
    if (EdaBridgeAppConfig::writeEasyEdaEextFile(target.toStdString(), err)) {
        QMessageBox::information(
            this,
            "aimon — EasyEDA Extension Exported",
            QString("Saved EasyEDA Pro extension archive to:\n%1\n\n"
                    "Import in EasyEDA Pro via: Advanced -> Extensions -> Extension Manager -> Import.")
                .arg(target)
        );
    } else {
        QMessageBox::critical(
            this,
            "aimon — Export Failed",
            QString("Failed to write .eext archive: %1").arg(QString::fromStdString(err))
        );
    }
}

void EdaTrayWindow::openWebDashboard() {
    QString url = QString("http://%1:%2")
                      .arg(QString::fromStdString(_settings.aimonHost))
                      .arg(_settings.aimonWebPort);
    QDesktopServices::openUrl(QUrl(url));
}

void EdaTrayWindow::quitApplication() {
    _quitting = true;
    if (_trayIcon) {
        _trayIcon->hide();
    }
    QApplication::quit();
}

std::string EdaTrayWindow::formatDurationCompact(int64_t seconds) {
    if (seconds <= 0) return "0m";
    int64_t d = seconds / 86400;
    int64_t h = (seconds % 86400) / 3600;
    int64_t m = (seconds % 3600) / 60;
    std::ostringstream oss;
    if (d > 0) {
        oss << d << "d " << h << "h";
    } else if (h > 0) {
        oss << h << "h " << m << "m";
    } else {
        oss << m << "m";
    }
    return oss.str();
}

std::string EdaTrayWindow::formatTokensCompact(int64_t tokens) {
    std::ostringstream oss;
    if (tokens >= 1000000) {
        oss << std::fixed << std::setprecision(1) << (static_cast<double>(tokens) / 1000000.0) << "M tok";
    } else if (tokens >= 1000) {
        oss << std::fixed << std::setprecision(1) << (static_cast<double>(tokens) / 1000.0) << "k tok";
    } else {
        oss << tokens << " tok";
    }
    return oss.str();
}

} // namespace aimon

#endif // AIMON_EDA_BRIDGE_WITH_QT

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
