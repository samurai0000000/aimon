/*
 * EdaTrayWindow.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifdef AIMON_EDA_BRIDGE_WITH_QT

#include "EdaTrayWindow.hxx"
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
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

QPixmap renderConceptBBrandPixmap(int size, bool withStatusDot, const QColor& dotColor) {
    QPixmap pix(size, size);
    pix.fill(Qt::transparent);

    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.save();
    const double scale = static_cast<double>(size) / 256.0;
    p.scale(scale, scale);

    // 1. Dark matte squircle (#0B0F19 fill with subtle #1E293B border)
    QPainterPath squircle;
    squircle.addRoundedRect(QRectF(12.0, 12.0, 232.0, 232.0), 54.0, 54.0);
    p.setPen(QPen(QColor("#1E293B"), 6.0));
    p.setBrush(QColor("#0B0F19"));
    p.drawPath(squircle);

    // 2. Bold Swiss-style geometric white 'A' (#F8FAFC) silhouette
    QPainterPath outerA;
    outerA.moveTo(110.0, 48.0);
    outerA.lineTo(146.0, 48.0);
    outerA.lineTo(202.0, 204.0);
    outerA.lineTo(162.0, 204.0);
    outerA.lineTo(149.0, 166.0);
    outerA.lineTo(107.0, 166.0);
    outerA.lineTo(94.0, 204.0);
    outerA.lineTo(54.0, 204.0);
    outerA.closeSubpath();

    QPainterPath innerHole;
    innerHole.moveTo(128.0, 84.0);
    innerHole.lineTo(111.0, 134.0);
    innerHole.lineTo(145.0, 134.0);
    innerHole.closeSubpath();

    QPainterPath letterA = outerA.subtracted(innerHole);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#F8FAFC"));
    p.drawPath(letterA);

    // 3. High-contrast cyan (#00F2FE) horizontal crossbar with central telemetry pulse peak
    QPainterPath pulsePath;
    pulsePath.moveTo(64.0, 148.0);
    pulsePath.lineTo(108.0, 148.0);
    pulsePath.lineTo(121.0, 120.0);
    pulsePath.lineTo(135.0, 164.0);
    pulsePath.lineTo(145.0, 148.0);
    pulsePath.lineTo(192.0, 148.0);

    QPen cutoutPen(QColor("#0B0F19"), 20.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(cutoutPen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(pulsePath);

    QPen cyanPen(QColor("#00F2FE"), 10.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(cyanPen);
    p.drawPath(pulsePath);

    // 4. Optional status indicator dot in bottom-right corner
    if (withStatusDot) {
        p.setPen(QPen(QColor("#0B0F19"), 12.0));
        p.setBrush(dotColor);
        p.drawEllipse(QPointF(204.0, 204.0), 34.0, 34.0);
    }

    p.restore();
    p.end();
    return pix;
}

std::pair<QString, QString> splitModelNameAndTier(const std::string& fullName) {
    QString s = QString::fromStdString(fullName).trimmed();
    if (s.endsWith(')')) {
        int idx = s.lastIndexOf('(');
        if (idx > 0) {
            QString tier = s.mid(idx + 1, s.length() - idx - 2).trimmed();
            QString baseName = s.left(idx).trimmed();
            return {baseName, tier};
        }
    }
    return {s, QString()};
}

QString progressBarStyleForPercent(double remainingPct) {
    QString chunkGradient =
        "qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0284c7, stop:1 #00f2fe)";
    if (remainingPct < 20.0) {
        chunkGradient =
            "qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #dc2626, stop:1 #f87171)";
    } else if (remainingPct < 50.0) {
        chunkGradient =
            "qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #d97706, stop:1 #fbbf24)";
    }
    return QString(
        "QProgressBar { border: 1px solid #1e293b; border-radius: 4px; "
        "background-color: #090d16; min-height: 6px; max-height: 6px; } "
        "QProgressBar::chunk { background: %1; border-radius: 3px; }"
    ).arg(chunkGradient);
}

QString colorForRemainingPercent(double remainingPct) {
    if (remainingPct < 20.0) {
        return "#f87171";
    }
    if (remainingPct < 50.0) {
        return "#fbbf24";
    }
    return "#00f2fe";
}

QString bucketColorForPercent(double remainingPct) {
    if (remainingPct <= 20.0) {
        return "#ef4444";
    }
    if (remainingPct <= 50.0) {
        return "#f59e0b";
    }
    return "#10b981";
}

QString bucketProgressBarStyle(double remainingPct) {
    return QString(
        "QProgressBar { border: none; border-radius: 3px; "
        "background-color: rgba(255, 255, 255, 0.08); min-height: 7px; max-height: 7px; } "
        "QProgressBar::chunk { background-color: %1; border-radius: 3px; }"
    ).arg(bucketColorForPercent(remainingPct));
}

QString spendProgressBarStyle(double usedPct) {
    QString chunkGradient =
        "qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0284c7, stop:1 #00f2fe)";
    if (usedPct >= 85.0) {
        chunkGradient =
            "qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #dc2626, stop:1 #f87171)";
    } else if (usedPct >= 60.0) {
        chunkGradient =
            "qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #d97706, stop:1 #fbbf24)";
    }
    return QString(
        "QProgressBar { border: 1px solid #1e293b; border-radius: 4px; "
        "background-color: #090d16; min-height: 6px; max-height: 6px; } "
        "QProgressBar::chunk { background: %1; border-radius: 3px; }"
    ).arg(chunkGradient);
}

QString colorForSpendPercent(double usedPct) {
    if (usedPct >= 85.0) {
        return "#f87171";
    }
    if (usedPct >= 60.0) {
        return "#fbbf24";
    }
    return "#00f2fe";
}

QString primaryGradientButtonStyle() {
    return QString(
        "QPushButton { "
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0284c7, stop:1 #0369a1); "
        "color: #ffffff; border: 1px solid rgba(56, 189, 248, 0.45); "
        "border-radius: 8px; font-weight: 700; font-size: 12px; padding: 8px 20px; "
        "} "
        "QPushButton:hover { "
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0369a1, stop:1 #0ea5e9); "
        "border-color: #38bdf8; "
        "} "
        "QPushButton:pressed { "
        "background: #0284c7; "
        "}"
    );
}

QString secondaryGhostButtonStyle() {
    return QString(
        "QPushButton { "
        "background-color: #0f172a; color: #e2e8f0; border: 1px solid #1e293b; "
        "border-radius: 8px; font-weight: 600; font-size: 12px; padding: 8px 18px; "
        "} "
        "QPushButton:hover { "
        "background-color: #1e293b; border-color: #334155; color: #ffffff; "
        "} "
        "QPushButton:pressed { "
        "background-color: #090d16; "
        "}"
    );
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
    setWindowIcon(createBrandAppIcon());
    setMinimumSize(680, 620);
    resize(740, 680);

    setStyleSheet(
        "QDialog { background-color: #0a0e17; color: #e2e8f0; font-family: 'Segoe UI', -apple-system, sans-serif; } "
        "QScrollArea, #quotaScroll, #quotaViewport, #quotaScrollContent, "
        "#settingsScroll, #settingsViewport, #settingsScrollContent, "
        "QScrollArea > QWidget, QScrollArea > QWidget > QWidget { "
        "background-color: #0a0e17; border: none; } "
        "QScrollBar:vertical { background: #0a0e17; width: 6px; margin: 0px; border-radius: 3px; } "
        "QScrollBar::handle:vertical { background: #1e293b; min-height: 28px; border-radius: 3px; } "
        "QScrollBar::handle:vertical:hover { background: #334155; } "
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: none; } "
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; } "
        "QScrollBar:horizontal { background: #0a0e17; height: 6px; margin: 0px; border-radius: 3px; } "
        "QScrollBar::handle:horizontal { background: #1e293b; min-width: 28px; border-radius: 3px; } "
        "QScrollBar::handle:horizontal:hover { background: #334155; } "
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: none; } "
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; } "
        "QTabWidget::pane { border: none; background: transparent; top: 6px; } "
        "QTabBar { background: #090d16; border: 1px solid #1e293b; border-radius: 20px; padding: 3px; } "
        "QTabBar::tab { background: transparent; color: #94a3b8; border: none; border-radius: 16px; "
        "padding: 7px 20px; margin: 0px 2px; font-weight: 600; font-size: 12px; } "
        "QTabBar::tab:hover:!selected { background: rgba(255, 255, 255, 0.04); color: #e2e8f0; } "
        "QTabBar::tab:selected { "
        "background: #1e293b; color: #f8fafc; font-weight: 700; border: 1px solid #334155; } "
        "QLabel { color: #e2e8f0; background: transparent; } "
        "QCheckBox { color: #e2e8f0; spacing: 8px; font-size: 12px; background: transparent; } "
        "QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px; "
        "border: 1px solid #334155; background-color: #090d16; } "
        "QCheckBox::indicator:hover { border-color: #38bdf8; } "
        "QCheckBox::indicator:checked { background-color: #0284c7; border-color: #00f2fe; } "
        "QLineEdit, QSpinBox, QComboBox { background-color: #090d16; color: #f8fafc; "
        "border: 1px solid #1e293b; border-radius: 6px; padding: 5px 10px; selection-background-color: #0284c7; } "
        "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid #38bdf8; background-color: #0f172a; } "
        "QComboBox::drop-down { border: none; width: 20px; } "
        "QComboBox QAbstractItemView { background-color: #0f172a; color: #f8fafc; "
        "border: 1px solid #1e293b; selection-background-color: #1e293b; selection-color: #38bdf8; outline: 0; } "
        "QPushButton { background-color: #111827; color: #e2e8f0; border: 1px solid #1e293b; "
        "border-radius: 7px; padding: 7px 16px; font-weight: 600; font-size: 12px; } "
        "QPushButton:hover { background-color: #1e293b; border-color: #38bdf8; color: #f8fafc; } "
        "QPushButton:pressed { background-color: #0f172a; }"
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

QIcon EdaTrayWindow::createBrandAppIcon() const {
    QIcon icon;
    const QString icoPath = QCoreApplication::applicationDirPath() + "/aimon.ico";
    if (QFileInfo::exists(icoPath)) {
        icon = QIcon(icoPath);
    }
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    for (int sz : sizes) {
        icon.addPixmap(renderConceptBBrandPixmap(sz, false, QColor()));
    }
    return icon;
}

QIcon EdaTrayWindow::createStatusTrayIcon(bool healthy, bool activePlugin) const {
    const QColor dotColor = healthy
        ? (activePlugin ? QColor("#10b981") : QColor("#00f2fe"))
        : QColor("#f59e0b");
    QIcon icon;
    icon.addPixmap(renderConceptBBrandPixmap(32, true, dotColor));
    icon.addPixmap(renderConceptBBrandPixmap(64, true, dotColor));
    return icon;
}

void EdaTrayWindow::buildUi() {
    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(14, 12, 14, 12);
    rootLayout->setSpacing(10);

    // Top Header Banner (Concept B emblem + bold title + subtitle + right-aligned connection & version pills)
    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(2, 0, 2, 2);
    headerLayout->setSpacing(10);

    QLabel* brandIconLbl = new QLabel(this);
    brandIconLbl->setPixmap(renderConceptBBrandPixmap(28, false, QColor()));
    brandIconLbl->setFixedSize(28, 28);

    QLabel* titleLbl = new QLabel("aimon", this);
    titleLbl->setStyleSheet(
        "font-size: 19px; font-weight: 800; color: #f8fafc; letter-spacing: 0.5px;"
    );

    QLabel* subtitleLbl = new QLabel("AI Quota Monitor & EDA / CAD Desktop Bridge", this);
    subtitleLbl->setStyleSheet("color: #64748b; font-size: 12px; padding-top: 3px;");

    _quotaConnectionBanner = new QLabel("● Connecting...", this);
    _quotaConnectionBanner->setStyleSheet(
        "background-color: rgba(56, 189, 248, 0.12); color: #38bdf8; "
        "border: 1px solid rgba(56, 189, 248, 0.35); border-radius: 10px; "
        "padding: 3px 10px; font-size: 11px; font-weight: 700;"
    );

    _settings.version = AIMON_VERSION_STRING;
    QLabel* verLbl = new QLabel(QString("v%1").arg(QString::fromUtf8(AIMON_VERSION_STRING)), this);
    verLbl->setStyleSheet(
        "background-color: rgba(56, 189, 248, 0.12); color: #38bdf8; "
        "border: 1px solid rgba(56, 189, 248, 0.35); border-radius: 10px; "
        "padding: 3px 10px; font-size: 11px; font-weight: 700;"
    );

    headerLayout->addWidget(brandIconLbl);
    headerLayout->addWidget(titleLbl);
    headerLayout->addWidget(subtitleLbl);
    headerLayout->addStretch(1);
    headerLayout->addWidget(_quotaConnectionBanner);
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

    QScrollArea* quotaScroll = new QScrollArea(quotasTab);
    quotaScroll->setObjectName("quotaScroll");
    quotaScroll->setWidgetResizable(true);
    quotaScroll->setFrameShape(QFrame::NoFrame);
    if (quotaScroll->viewport()) {
        quotaScroll->viewport()->setObjectName("quotaViewport");
        quotaScroll->viewport()->setAutoFillBackground(false);
    }
    QWidget* quotaScrollContent = new QWidget(quotaScroll);
    quotaScrollContent->setObjectName("quotaScrollContent");
    QVBoxLayout* scrollVBox = new QVBoxLayout(quotaScrollContent);
    scrollVBox->setContentsMargins(2, 2, 6, 2);
    scrollVBox->setSpacing(12);

    const QString statBoxStyle =
        "QFrame { background-color: #0f172a; border: 1px solid #1e293b; "
        "border-radius: 8px; padding: 6px 10px; }";
    const QString statCaptionStyle =
        "color: #64748b; font-size: 10px; font-weight: 700; letter-spacing: 0.5px; border: none; padding: 0;";

    // 1. Google Antigravity Card
    QFrame* agCard = new QFrame(quotaScrollContent);
    agCard->setObjectName("agCard");
    agCard->setStyleSheet(
        "#agCard { background-color: #0b1120; border: 1px solid #1e293b; border-radius: 12px; }"
    );
    QVBoxLayout* agLayout = new QVBoxLayout(agCard);
    agLayout->setContentsMargins(14, 14, 14, 14);
    agLayout->setSpacing(12);

    QHBoxLayout* agHeaderRow = new QHBoxLayout();
    agHeaderRow->setContentsMargins(0, 0, 0, 0);
    agHeaderRow->setSpacing(8);

    _agStatusDot = new QLabel("●", agCard);
    _agStatusDot->setStyleSheet("color: #64748b; font-size: 13px;");

    QLabel* agTitleLbl = new QLabel("Google Antigravity", agCard);
    agTitleLbl->setStyleSheet("color: #f8fafc; font-size: 15px; font-weight: 800; letter-spacing: 0.3px;");

    _agHeaderLabel = new QLabel("○ PROBING", agCard);
    _agHeaderLabel->setStyleSheet(
        "background-color: rgba(148, 163, 184, 0.12); color: #94a3b8; "
        "border: 1px solid #334155; border-radius: 6px; "
        "padding: 2px 8px; font-size: 11px; font-weight: 700;"
    );

    _agPlanPill = new QLabel("GOOGLE AI ULTRA", agCard);
    _agPlanPill->setObjectName("agPlanPill");
    _agPlanPill->setStyleSheet(
        "background-color: rgba(0, 242, 254, 0.12); color: #00f2fe; "
        "border: 1px solid rgba(0, 242, 254, 0.30); border-radius: 6px; "
        "padding: 3px 10px; font-size: 11px; font-weight: 700; letter-spacing: 0.4px;"
    );

    agHeaderRow->addWidget(_agStatusDot);
    agHeaderRow->addWidget(agTitleLbl);
    agHeaderRow->addStretch(1);
    agHeaderRow->addWidget(_agHeaderLabel);
    agHeaderRow->addWidget(_agPlanPill);
    agLayout->addLayout(agHeaderRow);

    // Credits & Allowances Strip (2 Equal Columns)
    QHBoxLayout* agStatsRow = new QHBoxLayout();
    agStatsRow->setSpacing(10);

    const QString creditCardQss =
        "#agCreditCard { background-color: rgba(255, 255, 255, 0.03); "
        "border: 1px solid #1e293b; border-radius: 10px; }";

    // Left Card (GOOGLE ONE AI CREDITS)
    QFrame* agGoogleOneCard = new QFrame(agCard);
    agGoogleOneCard->setObjectName("agCreditCard");
    agGoogleOneCard->setStyleSheet(creditCardQss);
    QVBoxLayout* agGoogleOneLayout = new QVBoxLayout(agGoogleOneCard);
    agGoogleOneLayout->setContentsMargins(12, 10, 12, 10);
    agGoogleOneLayout->setSpacing(6);

    QHBoxLayout* agGoTopRow = new QHBoxLayout();
    agGoTopRow->setContentsMargins(0, 0, 0, 0);
    agGoTopRow->setSpacing(4);
    QLabel* agGoTitle = new QLabel("GOOGLE ONE AI CREDITS", agGoogleOneCard);
    agGoTitle->setStyleSheet("color: #94a3b8; font-size: 10px; font-weight: 700; letter-spacing: 0.4px;");
    _agCreditMinPill = new QLabel("Min 50 / req", agGoogleOneCard);
    _agCreditMinPill->setStyleSheet(
        "background-color: rgba(255, 255, 255, 0.05); color: #94a3b8; "
        "border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: 600;"
    );
    agGoTopRow->addWidget(agGoTitle);
    agGoTopRow->addStretch(1);
    agGoTopRow->addWidget(_agCreditMinPill);
    agGoogleOneLayout->addLayout(agGoTopRow);

    _agGoogleOneCreditsVal = new QLabel("--", agGoogleOneCard);
    _agGoogleOneCreditsVal->setStyleSheet(
        "color: #00f2fe; font-family: 'Consolas', 'JetBrains Mono', monospace; "
        "font-size: 20px; font-weight: 800;"
    );
    agGoogleOneLayout->addWidget(_agGoogleOneCreditsVal);

    // Right Card (PROMPT & FLOW BALANCE)
    QFrame* agPromptFlowCard = new QFrame(agCard);
    agPromptFlowCard->setObjectName("agCreditCard");
    agPromptFlowCard->setStyleSheet(creditCardQss);
    QVBoxLayout* agPromptFlowLayout = new QVBoxLayout(agPromptFlowCard);
    agPromptFlowLayout->setContentsMargins(12, 10, 12, 10);
    agPromptFlowLayout->setSpacing(6);

    QHBoxLayout* agPfTopRow = new QHBoxLayout();
    agPfTopRow->setContentsMargins(0, 0, 0, 0);
    agPfTopRow->setSpacing(4);
    QLabel* agPfTitle = new QLabel("PROMPT & FLOW BALANCE", agPromptFlowCard);
    agPfTitle->setStyleSheet("color: #94a3b8; font-size: 10px; font-weight: 700; letter-spacing: 0.4px;");
    _agPromptFlowTierPill = new QLabel("Monthly", agPromptFlowCard);
    _agPromptFlowTierPill->setStyleSheet(
        "background-color: rgba(255, 255, 255, 0.05); color: #94a3b8; "
        "border-radius: 4px; padding: 2px 6px; font-size: 10px; font-weight: 600;"
    );
    agPfTopRow->addWidget(agPfTitle);
    agPfTopRow->addStretch(1);
    agPfTopRow->addWidget(_agPromptFlowTierPill);
    agPromptFlowLayout->addLayout(agPfTopRow);

    _agPromptFlowVal = new QLabel("--", agPromptFlowCard);
    _agPromptFlowVal->setStyleSheet(
        "color: #f8fafc; font-family: 'Consolas', 'JetBrains Mono', monospace; "
        "font-size: 20px; font-weight: 800;"
    );
    agPromptFlowLayout->addWidget(_agPromptFlowVal);

    agStatsRow->addWidget(agGoogleOneCard, 1);
    agStatsRow->addWidget(agPromptFlowCard, 1);
    agLayout->addLayout(agStatsRow);

    // Model Quota Limits Section
    QLabel* agQuotaSectionTitle = new QLabel("MODEL QUOTA LIMITS", agCard);
    agQuotaSectionTitle->setStyleSheet(
        "color: #94a3b8; font-size: 11px; font-weight: 700; "
        "letter-spacing: 0.5px; margin-top: 2px;"
    );
    _agQuotaGroupsContainer = new QWidget(agCard);
    _agQuotaGroupsLayout = new QVBoxLayout(_agQuotaGroupsContainer);
    _agQuotaGroupsLayout->setContentsMargins(0, 0, 0, 0);
    _agQuotaGroupsLayout->setSpacing(10);
    agLayout->addWidget(agQuotaSectionTitle);
    agLayout->addWidget(_agQuotaGroupsContainer);

    // Collapsible Individual Model Capacities Section
    QFrame* agDivider = new QFrame(agCard);
    agDivider->setFixedHeight(1);
    agDivider->setStyleSheet("background-color: rgba(255, 255, 255, 0.06); border: none;");
    agLayout->addWidget(agDivider);

    _agModelsToggleBtn = new QPushButton("Individual Model Capacities (0)  ˅", agCard);
    _agModelsToggleBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; color: #94a3b8; "
        "font-size: 12px; font-weight: 600; text-align: left; padding: 4px 2px; } "
        "QPushButton:hover { color: #f8fafc; background: transparent; border: none; }"
    );
    agLayout->addWidget(_agModelsToggleBtn);

    _agModelsContainer = new QWidget(agCard);
    _agModelsLayout = new QVBoxLayout(_agModelsContainer);
    _agModelsLayout->setContentsMargins(0, 4, 0, 0);
    _agModelsLayout->setSpacing(6);
    _agModelsContainer->setVisible(false);
    agLayout->addWidget(_agModelsContainer);

    QObject::connect(_agModelsToggleBtn, &QPushButton::clicked, [this]() {
        _agModelsExpanded = !_agModelsExpanded;
        _agModelsContainer->setVisible(_agModelsExpanded && !_lastQuotaSnap.agModels.empty());
        _agModelsToggleBtn->setText(
            QString("Individual Model Capacities (%1)  %2")
                .arg(_lastQuotaSnap.agModels.size())
                .arg(_agModelsExpanded ? "˄" : "˅")
        );
    });

    scrollVBox->addWidget(agCard);

    // 2. Cursor Card
    QFrame* curCard = new QFrame(quotaScrollContent);
    curCard->setObjectName("curCard");
    curCard->setStyleSheet(
        "#curCard { background-color: #0b1120; border: 1px solid #1e293b; border-radius: 12px; }"
    );
    QVBoxLayout* curLayout = new QVBoxLayout(curCard);
    curLayout->setContentsMargins(14, 14, 14, 14);
    curLayout->setSpacing(12);

    QHBoxLayout* curHeaderRow = new QHBoxLayout();
    curHeaderRow->setContentsMargins(0, 0, 0, 0);
    curHeaderRow->setSpacing(8);

    QLabel* curTitleLbl = new QLabel("✦  Cursor", curCard);
    curTitleLbl->setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: 800; letter-spacing: 0.3px;");

    _cursorHeaderLabel = new QLabel("○ PROBING", curCard);
    _cursorHeaderLabel->setStyleSheet(
        "background-color: rgba(148, 163, 184, 0.12); color: #94a3b8; "
        "border: 1px solid #334155; border-radius: 6px; "
        "padding: 2px 8px; font-size: 11px; font-weight: 700;"
    );

    QLabel* curPlanPill = new QLabel("Pro", curCard);
    curPlanPill->setObjectName("cursorPlanPill");
    curPlanPill->setStyleSheet(
        "background-color: rgba(56, 189, 248, 0.14); color: #38bdf8; "
        "border: 1px solid rgba(56, 189, 248, 0.35); border-radius: 6px; "
        "padding: 2px 8px; font-size: 11px; font-weight: 700;"
    );
    QLabel* curModePill = new QLabel("spend", curCard);
    curModePill->setObjectName("cursorModePill");
    curModePill->setStyleSheet(
        "background-color: rgba(168, 85, 247, 0.14); color: #c084fc; "
        "border: 1px solid rgba(168, 85, 247, 0.35); border-radius: 6px; "
        "padding: 2px 8px; font-size: 11px; font-weight: 600;"
    );
    curHeaderRow->addWidget(curTitleLbl);
    curHeaderRow->addStretch(1);
    curHeaderRow->addWidget(_cursorHeaderLabel);
    curHeaderRow->addWidget(curPlanPill);
    curHeaderRow->addWidget(curModePill);
    curLayout->addLayout(curHeaderRow);

    QHBoxLayout* curStatsRow = new QHBoxLayout();
    curStatsRow->setSpacing(10);

    QFrame* curSpendBox = new QFrame(curCard);
    curSpendBox->setStyleSheet(statBoxStyle);
    QVBoxLayout* curSpendBoxLayout = new QVBoxLayout(curSpendBox);
    curSpendBoxLayout->setContentsMargins(6, 4, 6, 4);
    curSpendBoxLayout->setSpacing(2);
    QLabel* curSpendCap = new QLabel("TOTAL SPEND", curSpendBox);
    curSpendCap->setStyleSheet(statCaptionStyle);
    _cursorSpendLabel = new QLabel("$0.00", curSpendBox);
    _cursorSpendLabel->setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: 700; border: none; padding: 0;");
    curSpendBoxLayout->addWidget(curSpendCap);
    curSpendBoxLayout->addWidget(_cursorSpendLabel);

    QFrame* curOnDemandBox = new QFrame(curCard);
    curOnDemandBox->setStyleSheet(statBoxStyle);
    QVBoxLayout* curOnDemandLayout = new QVBoxLayout(curOnDemandBox);
    curOnDemandLayout->setContentsMargins(6, 4, 6, 4);
    curOnDemandLayout->setSpacing(2);
    QLabel* curOnDemandCap = new QLabel("ON-DEMAND SPEND", curOnDemandBox);
    curOnDemandCap->setStyleSheet(statCaptionStyle);
    QLabel* curOnDemandVal = new QLabel("$0.00", curOnDemandBox);
    curOnDemandVal->setObjectName("cursorOnDemandValue");
    curOnDemandVal->setStyleSheet("color: #38bdf8; font-size: 14px; font-weight: 700; border: none; padding: 0;");
    curOnDemandLayout->addWidget(curOnDemandCap);
    curOnDemandLayout->addWidget(curOnDemandVal);

    QFrame* curResetBox = new QFrame(curCard);
    curResetBox->setStyleSheet(statBoxStyle);
    QVBoxLayout* curResetBoxLayout = new QVBoxLayout(curResetBox);
    curResetBoxLayout->setContentsMargins(6, 4, 6, 4);
    curResetBoxLayout->setSpacing(2);
    QLabel* curResetCap = new QLabel("CYCLE RESET", curResetBox);
    curResetCap->setStyleSheet(statCaptionStyle);
    _cursorResetLabel = new QLabel("--", curResetBox);
    _cursorResetLabel->setStyleSheet("color: #e2e8f0; font-size: 14px; font-weight: 700; border: none; padding: 0;");
    curResetBoxLayout->addWidget(curResetCap);
    curResetBoxLayout->addWidget(_cursorResetLabel);

    curStatsRow->addWidget(curSpendBox, 1);
    curStatsRow->addWidget(curOnDemandBox, 1);
    curStatsRow->addWidget(curResetBox, 1);
    curLayout->addLayout(curStatsRow);

    QHBoxLayout* curReqHeaderRow = new QHBoxLayout();
    QLabel* curReqTitle = new QLabel("Fast Requests Quota", curCard);
    curReqTitle->setStyleSheet("color: #94a3b8; font-size: 12px; font-weight: 600;");
    QLabel* curReqCounter = new QLabel("0 / 0", curCard);
    curReqCounter->setObjectName("cursorReqCounterLabel");
    curReqCounter->setStyleSheet("color: #e2e8f0; font-size: 12px; font-weight: 700;");
    curReqHeaderRow->addWidget(curReqTitle);
    curReqHeaderRow->addStretch(1);
    curReqHeaderRow->addWidget(curReqCounter);
    curLayout->addLayout(curReqHeaderRow);

    QHBoxLayout* curBarRow = new QHBoxLayout();
    curBarRow->setSpacing(10);
    _cursorProgressBar = new QProgressBar(curCard);
    _cursorProgressBar->setRange(0, 100);
    _cursorProgressBar->setValue(0);
    _cursorProgressBar->setTextVisible(false);
    _cursorProgressBar->setFixedHeight(6);
    _cursorProgressBar->setStyleSheet(spendProgressBarStyle(0.0));
    QLabel* curPctLbl = new QLabel("0%", curCard);
    curPctLbl->setObjectName("cursorPctReadout");
    curPctLbl->setMinimumWidth(42);
    curPctLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    curPctLbl->setStyleSheet("color: #00f2fe; font-size: 12px; font-weight: 700;");
    curBarRow->addWidget(_cursorProgressBar, 1);
    curBarRow->addWidget(curPctLbl);
    curLayout->addLayout(curBarRow);

    scrollVBox->addWidget(curCard);

    // 3. Claude Code Card
    QFrame* claudeCard = new QFrame(quotaScrollContent);
    claudeCard->setObjectName("claudeCard");
    claudeCard->setStyleSheet(
        "#claudeCard { background-color: #0b1120; border: 1px solid #1e293b; border-radius: 12px; }"
    );
    QVBoxLayout* claudeLayout = new QVBoxLayout(claudeCard);
    claudeLayout->setContentsMargins(14, 14, 14, 14);
    claudeLayout->setSpacing(12);

    QHBoxLayout* claudeHeaderRow = new QHBoxLayout();
    claudeHeaderRow->setContentsMargins(0, 0, 0, 0);
    claudeHeaderRow->setSpacing(8);

    QLabel* claudeTitleLbl = new QLabel("✦  Claude Code", claudeCard);
    claudeTitleLbl->setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: 800; letter-spacing: 0.3px;");

    _claudeHeaderLabel = new QLabel("○ PROBING", claudeCard);
    _claudeHeaderLabel->setStyleSheet(
        "background-color: rgba(148, 163, 184, 0.12); color: #94a3b8; "
        "border: 1px solid #334155; border-radius: 6px; "
        "padding: 2px 8px; font-size: 11px; font-weight: 700;"
    );

    claudeHeaderRow->addWidget(claudeTitleLbl);
    claudeHeaderRow->addStretch(1);
    claudeHeaderRow->addWidget(_claudeHeaderLabel);
    claudeLayout->addLayout(claudeHeaderRow);

    _claudeAccountsContainer = new QWidget(claudeCard);
    _claudeAccountsLayout = new QVBoxLayout(_claudeAccountsContainer);
    _claudeAccountsLayout->setContentsMargins(0, 4, 0, 0);
    _claudeAccountsLayout->setSpacing(8);
    claudeLayout->addWidget(_claudeAccountsContainer);
    scrollVBox->addWidget(claudeCard);

    scrollVBox->addStretch(1);
    quotaScroll->setWidget(quotaScrollContent);
    quotasLayout->addWidget(quotaScroll, 1);

    // Action Bar at bottom of AI Quotas tab
    QHBoxLayout* quotaActionLayout = new QHBoxLayout();
    quotaActionLayout->setSpacing(10);
    _quotaLastUpdatedLabel = new QLabel("Last updated: never", quotasTab);
    _quotaLastUpdatedLabel->setStyleSheet("color: #94a3b8; font-size: 12px;");
    _btnRefreshQuotas = new QPushButton("Refresh Quotas Now", quotasTab);
    _btnRefreshQuotas->setStyleSheet(secondaryGhostButtonStyle());
    _btnOpenWebDashboard = new QPushButton("Open Web Dashboard", quotasTab);
    _btnOpenWebDashboard->setStyleSheet(primaryGradientButtonStyle());

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

    QScrollArea* settingsScroll = new QScrollArea(bridgesTab);
    settingsScroll->setObjectName("settingsScroll");
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setFrameShape(QFrame::NoFrame);
    if (settingsScroll->viewport()) {
        settingsScroll->viewport()->setObjectName("settingsViewport");
        settingsScroll->viewport()->setAutoFillBackground(false);
    }
    QWidget* settingsContent = new QWidget(settingsScroll);
    settingsContent->setObjectName("settingsScrollContent");
    QVBoxLayout* settingsVBox = new QVBoxLayout(settingsContent);
    settingsVBox->setContentsMargins(2, 2, 6, 2);
    settingsVBox->setSpacing(12);

    // Live Bridge Status Card
    QFrame* statusCard = new QFrame(settingsContent);
    statusCard->setObjectName("statusCard");
    statusCard->setStyleSheet(
        "#statusCard { background-color: #0b1120; border: 1px solid #1e293b; border-radius: 12px; }"
    );
    QVBoxLayout* statusCardLayout = new QVBoxLayout(statusCard);
    statusCardLayout->setContentsMargins(14, 14, 14, 14);
    statusCardLayout->setSpacing(10);
    QLabel* statusTitle = new QLabel("✦  Live Bridge & Companion Plugin Telemetry", statusCard);
    statusTitle->setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: 800; letter-spacing: 0.3px;");
    statusCardLayout->addWidget(statusTitle);

    QGridLayout* statusGrid = new QGridLayout();
    statusGrid->setSpacing(8);
    _lblRpcStatus = new QLabel("RPC Listener: ...", statusCard);
    _lblGatewayStatus = new QLabel("aimon Gateway: ...", statusCard);
    _lblEasyEdaStatus = new QLabel("EasyEDA Pro: ...", statusCard);
    _lblKiCadStatus = new QLabel("KiCad: ...", statusCard);
    _lblFreeCadStatus = new QLabel("FreeCAD: ...", statusCard);
    statusGrid->addWidget(_lblRpcStatus, 0, 0);
    statusGrid->addWidget(_lblGatewayStatus, 0, 1);
    statusGrid->addWidget(_lblEasyEdaStatus, 1, 0);
    statusGrid->addWidget(_lblKiCadStatus, 1, 1);
    statusGrid->addWidget(_lblFreeCadStatus, 2, 0, 1, 2);
    statusCardLayout->addLayout(statusGrid);
    settingsVBox->addWidget(statusCard);

    // Persisted Checkboxes Card
    QFrame* togglesCard = new QFrame(settingsContent);
    togglesCard->setObjectName("togglesCard");
    togglesCard->setStyleSheet(
        "#togglesCard { background-color: #0b1120; border: 1px solid #1e293b; border-radius: 12px; }"
    );
    QVBoxLayout* togglesCardLayout = new QVBoxLayout(togglesCard);
    togglesCardLayout->setContentsMargins(14, 14, 14, 14);
    togglesCardLayout->setSpacing(10);
    QLabel* togglesTitle = new QLabel("✦  Persisted Bridge & System Tray Options", togglesCard);
    togglesTitle->setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: 800; letter-spacing: 0.3px;");
    togglesCardLayout->addWidget(togglesTitle);

    QGridLayout* togglesGrid = new QGridLayout();
    togglesGrid->setSpacing(8);
    _chkEnableEasyEda = new QCheckBox("Enable EasyEDA Pro Bridge", togglesCard);
    _chkEnableKiCad = new QCheckBox("Enable KiCad Bridge", togglesCard);
    _chkEnableFreeCad = new QCheckBox("Enable FreeCAD Bridge", togglesCard);
    _chkAutoDeployPlugins = new QCheckBox("Auto-deploy companion plugins on startup", togglesCard);
    _chkStartMinimized = new QCheckBox("Start minimized to Windows System Tray", togglesCard);
    _chkLaunchAtLogin = new QCheckBox("Launch automatically at Windows login", togglesCard);

    togglesGrid->addWidget(_chkEnableEasyEda, 0, 0);
    togglesGrid->addWidget(_chkEnableKiCad, 0, 1);
    togglesGrid->addWidget(_chkEnableFreeCad, 1, 0);
    togglesGrid->addWidget(_chkAutoDeployPlugins, 1, 1);
    togglesGrid->addWidget(_chkStartMinimized, 2, 0);
    togglesGrid->addWidget(_chkLaunchAtLogin, 2, 1);
    togglesCardLayout->addLayout(togglesGrid);
    settingsVBox->addWidget(togglesCard);

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

    // Connection & Paths Card
    QFrame* connCard = new QFrame(settingsContent);
    connCard->setObjectName("connCard");
    connCard->setStyleSheet(
        "#connCard { background-color: #0b1120; border: 1px solid #1e293b; border-radius: 12px; }"
    );
    QVBoxLayout* connCardLayout = new QVBoxLayout(connCard);
    connCardLayout->setContentsMargins(14, 14, 14, 14);
    connCardLayout->setSpacing(10);
    QLabel* connTitle = new QLabel("✦  aimon Server & Bridge Connection Settings", connCard);
    connTitle->setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: 800; letter-spacing: 0.3px;");
    connCardLayout->addWidget(connTitle);

    QGridLayout* connGrid = new QGridLayout();
    connGrid->setSpacing(8);

    _edtAimonHost = new QLineEdit(connCard);
    _spnAimonWebPort = new QSpinBox(connCard);
    _spnAimonWebPort->setRange(1, 65535);
    _spnGatewayPort = new QSpinBox(connCard);
    _spnGatewayPort->setRange(1, 65535);

    _edtAuthSecret = new QLineEdit(connCard);
    _edtAuthSecret->setEchoMode(QLineEdit::PasswordEchoOnEdit);

    connGrid->addWidget(new QLabel("aimon Host:", connCard), 0, 0);
    connGrid->addWidget(_edtAimonHost, 0, 1);
    connGrid->addWidget(new QLabel("Web Port:", connCard), 0, 2);
    connGrid->addWidget(_spnAimonWebPort, 0, 3);

    connGrid->addWidget(new QLabel("Gateway Port:", connCard), 1, 0);
    connGrid->addWidget(_spnGatewayPort, 1, 1);
    connGrid->addWidget(new QLabel("Auth Secret:", connCard), 1, 2);
    connGrid->addWidget(_edtAuthSecret, 1, 3);

    connCardLayout->addLayout(connGrid);
    settingsVBox->addWidget(connCard);
    settingsVBox->addStretch(1);
    settingsScroll->setWidget(settingsContent);
    bridgesLayout->addWidget(settingsScroll, 1);

    _lblSettingsStatus = new QLabel(
        QString("Config file: %1").arg(QString::fromStdString(_configFilePath)), bridgesTab
    );
    _lblSettingsStatus->setStyleSheet("color: #94a3b8; font-size: 11px;");
    bridgesLayout->addWidget(_lblSettingsStatus);

    QHBoxLayout* bridgeButtons = new QHBoxLayout();
    bridgeButtons->setSpacing(10);
    QPushButton* btnSave = new QPushButton("Apply & Save Settings", bridgesTab);
    btnSave->setStyleSheet(primaryGradientButtonStyle());
    QPushButton* btnDeploy = new QPushButton("Deploy KiCad & FreeCAD Plugins Now", bridgesTab);
    btnDeploy->setStyleSheet(secondaryGhostButtonStyle());
    QPushButton* btnExportEext = new QPushButton("Export EasyEDA .eext Extension...", bridgesTab);
    btnExportEext->setStyleSheet(secondaryGhostButtonStyle());

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

    if (_actTrayEasyEda) _actTrayEasyEda->setChecked(_settings.enableEasyEda);
    if (_actTrayKiCad) _actTrayKiCad->setChecked(_settings.enableKiCad);
    if (_actTrayFreeCad) _actTrayFreeCad->setChecked(_settings.enableFreeCad);

    _edtAimonHost->setText(QString::fromStdString(_settings.aimonHost));
    _spnAimonWebPort->setValue(_settings.aimonWebPort);
    _spnGatewayPort->setValue(_settings.gatewayPort);
    _edtAuthSecret->setText(QString::fromStdString(_settings.authSecret));
    _syncingUi = false;
}

void EdaTrayWindow::collectSettingsFromControls() {
    _settings.enableEasyEda = _chkEnableEasyEda->isChecked();
    _settings.enableKiCad = _chkEnableKiCad->isChecked();
    _settings.enableFreeCad = _chkEnableFreeCad->isChecked();
    _settings.autoDeployPlugins = _chkAutoDeployPlugins->isChecked();
    _settings.startMinimizedToTray = _chkStartMinimized->isChecked();
    _settings.launchAtLogin = _chkLaunchAtLogin->isChecked();
    _settings.disallowExports = false;

    _settings.aimonHost = _edtAimonHost->text().trimmed().toStdString();
    if (_settings.aimonHost.empty()) {
        _settings.aimonHost = "127.0.0.1";
    }
    _settings.aimonWebPort = _spnAimonWebPort->value();
    _settings.gatewayPort = _spnGatewayPort->value();
    _settings.connectionMode = "bidirectional";
    _settings.listenHost = "0.0.0.0";
    _settings.rpcPort = 3892;
    _settings.pluginPort = 3893;
    _settings.authSecret = _edtAuthSecret->text().toStdString();
    _settings.allowedRoot = "";
}

void EdaTrayWindow::applyRuntimeToggles() {
    _server.setBridgeEnabled("easyeda", _settings.enableEasyEda);
    _server.setBridgeEnabled("kicad", _settings.enableKiCad);
    _server.setBridgeEnabled("freecad", _settings.enableFreeCad);
    _server.setDisallowExports(_settings.disallowExports);
    _server.setAllowedRoot(_settings.allowedRoot);

    _syncingUi = true;
    if (_actTrayEasyEda) _actTrayEasyEda->setChecked(_settings.enableEasyEda);
    if (_actTrayKiCad) _actTrayKiCad->setChecked(_settings.enableKiCad);
    if (_actTrayFreeCad) _actTrayFreeCad->setChecked(_settings.enableFreeCad);
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

    const QString greenPillStyle =
        "background-color: rgba(16, 185, 129, 0.14); color: #10b981; "
        "border: 1px solid rgba(16, 185, 129, 0.35); border-radius: 7px; "
        "padding: 6px 10px; font-weight: 700; font-size: 12px;";
    const QString skyBluePillStyle =
        "background-color: rgba(56, 189, 248, 0.12); color: #38bdf8; "
        "border: 1px solid rgba(56, 189, 248, 0.32); border-radius: 7px; "
        "padding: 6px 10px; font-weight: 600; font-size: 12px;";
    const QString amberPillStyle =
        "background-color: rgba(245, 158, 11, 0.12); color: #fbbf24; "
        "border: 1px solid rgba(245, 158, 11, 0.35); border-radius: 7px; "
        "padding: 6px 10px; font-weight: 600; font-size: 12px;";
    const QString mutedSlatePillStyle =
        "background-color: #0b1120; color: #64748b; "
        "border: 1px solid #1e293b; border-radius: 7px; "
        "padding: 6px 10px; font-weight: 600; font-size: 12px;";
    const QString redPillStyle =
        "background-color: rgba(239, 68, 68, 0.14); color: #f87171; "
        "border: 1px solid rgba(239, 68, 68, 0.35); border-radius: 7px; "
        "padding: 6px 10px; font-weight: 700; font-size: 12px;";

    _lblRpcStatus->setText(
        rpcRunning
            ? QString("●  RPC Listener: ONLINE (:%1 / plugin :%2)")
                  .arg(_server.boundPort())
                  .arg(_server.boundPluginPort())
            : QString("○  RPC Listener: OFFLINE")
    );
    _lblRpcStatus->setStyleSheet(rpcRunning ? greenPillStyle : redPillStyle);

    _lblGatewayStatus->setText(
        gwConn
            ? QString("●  aimon Gateway: CONNECTED (%1:%2)")
                  .arg(QString::fromStdString(_settings.aimonHost))
                  .arg(_settings.gatewayPort)
            : QString("◐  aimon Gateway: STANDBY (%1:%2)")
                  .arg(QString::fromStdString(_settings.aimonHost))
                  .arg(_settings.gatewayPort)
    );
    _lblGatewayStatus->setStyleSheet(gwConn ? greenPillStyle : amberPillStyle);

    auto formatAppStatus = [](const char* label, bool enabled, bool pluginLive) -> QString {
        if (!enabled) {
            return QString("○  %1: DISABLED").arg(label);
        }
        return pluginLive ? QString("●  %1: LIVE GUI CONNECTED").arg(label)
                          : QString("◐  %1: READY (Standby / CLI)").arg(label);
    };

    _lblEasyEdaStatus->setText(formatAppStatus("EasyEDA Pro", _settings.enableEasyEda, eeConn));
    _lblEasyEdaStatus->setStyleSheet(
        !_settings.enableEasyEda ? mutedSlatePillStyle : (eeConn ? greenPillStyle : skyBluePillStyle)
    );

    _lblKiCadStatus->setText(formatAppStatus("KiCad", _settings.enableKiCad, kcConn));
    _lblKiCadStatus->setStyleSheet(
        !_settings.enableKiCad ? mutedSlatePillStyle : (kcConn ? greenPillStyle : skyBluePillStyle)
    );

    _lblFreeCadStatus->setText(formatAppStatus("FreeCAD", _settings.enableFreeCad, fcConn));
    _lblFreeCadStatus->setStyleSheet(
        !_settings.enableFreeCad ? mutedSlatePillStyle : (fcConn ? greenPillStyle : skyBluePillStyle)
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
    updateQuotaCountdowns();
}

void EdaTrayWindow::refreshAiQuotas(bool triggerRemoteRefresh) {
    AimonQuotaSnapshot snap;
    EdaBridgeAppConfig::fetchQuotaSnapshot(
        _settings.aimonHost, _settings.aimonWebPort, triggerRemoteRefresh, snap, 2500
    );
    _lastQuotaFetchSteadySec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    _lastQuotaSnap = snap;
    populateQuotasUi(snap);
    refreshLiveStatus();
}

void EdaTrayWindow::populateQuotasUi(const AimonQuotaSnapshot& snap) {
    if (!snap.reachable) {
        _quotaConnectionBanner->setText(
            QString("● Offline: %1:%2").arg(QString::fromStdString(_settings.aimonHost)).arg(_settings.aimonWebPort)
        );
        _quotaConnectionBanner->setToolTip(
            QString("aimon server unreachable at http://%1:%2 — %3")
                .arg(QString::fromStdString(_settings.aimonHost))
                .arg(_settings.aimonWebPort)
                .arg(QString::fromStdString(snap.errorMessage))
        );
        _quotaConnectionBanner->setStyleSheet(
            "background-color: rgba(239, 68, 68, 0.14); color: #f87171; "
            "border: 1px solid rgba(239, 68, 68, 0.40); border-radius: 10px; "
            "padding: 3px 10px; font-size: 11px; font-weight: 700;"
        );
    } else {
        _quotaConnectionBanner->setText(
            QString("● %1:%2").arg(QString::fromStdString(_settings.aimonHost)).arg(_settings.aimonWebPort)
        );
        _quotaConnectionBanner->setToolTip(
            QString("Connected to aimon server at http://%1:%2")
                .arg(QString::fromStdString(_settings.aimonHost))
                .arg(_settings.aimonWebPort)
        );
        _quotaConnectionBanner->setStyleSheet(
            "background-color: rgba(16, 185, 129, 0.14); color: #10b981; "
            "border: 1px solid rgba(16, 185, 129, 0.35); border-radius: 10px; "
            "padding: 3px 10px; font-size: 11px; font-weight: 700;"
        );
    }

    const QString activeStatusPillStyle =
        "background-color: rgba(16, 185, 129, 0.14); color: #10b981; "
        "border: 1px solid rgba(16, 185, 129, 0.35); border-radius: 6px; "
        "padding: 2px 8px; font-size: 11px; font-weight: 700;";
    const QString offlineStatusPillStyle =
        "background-color: rgba(148, 163, 184, 0.12); color: #94a3b8; "
        "border: 1px solid #334155; border-radius: 6px; "
        "padding: 2px 8px; font-size: 11px; font-weight: 700;";

    // 1. Google Antigravity
    _agCountdownBindings.clear();
    clearLayoutWidgets(_agQuotaGroupsLayout);
    clearLayoutWidgets(_agModelsLayout);

    if (snap.agRunning) {
        _agStatusDot->setStyleSheet("color: #10b981; font-size: 13px;");
        _agHeaderLabel->setVisible(false);
        _agPlanPill->setText(
            QString::fromStdString(snap.agPlanTier.empty() ? "Google AI Ultra" : snap.agPlanTier).toUpper()
        );
        _agPlanPill->setVisible(true);

        if (snap.agHasGoogleOneCredits) {
            _agGoogleOneCreditsVal->setText(
                QString::fromStdString(formatNumberWithCommas(snap.agGoogleOneCredits))
            );
            _agGoogleOneCreditsVal->setStyleSheet(
                "color: #00f2fe; font-family: 'Consolas', 'JetBrains Mono', monospace; font-size: 20px; font-weight: 800;"
            );
            _agCreditMinPill->setText(
                QString("Min %1 / req").arg(snap.agMinCreditPerReq > 0 ? snap.agMinCreditPerReq : 50)
            );
        } else {
            _agGoogleOneCreditsVal->setText("0");
            _agGoogleOneCreditsVal->setStyleSheet(
                "color: #f8fafc; font-family: 'Consolas', 'JetBrains Mono', monospace; font-size: 20px; font-weight: 800;"
            );
            _agCreditMinPill->setText("Min 50 / req");
        }

        if (snap.agPromptCredits > 0 || snap.agFlowCredits > 0 ||
            snap.agMonthlyPromptCredits > 0 || snap.agMonthlyFlowCredits > 0) {
            _agPromptFlowVal->setText(
                QString("%1 / %2")
                    .arg(QString::fromStdString(formatNumberWithCommas(snap.agPromptCredits)))
                    .arg(QString::fromStdString(formatNumberWithCommas(snap.agFlowCredits)))
            );
            if (snap.agMonthlyPromptCredits > 0 || snap.agMonthlyFlowCredits > 0) {
                _agPromptFlowTierPill->setText(
                    QString("Mo: %1k / %2k")
                        .arg(snap.agMonthlyPromptCredits / 1000)
                        .arg(snap.agMonthlyFlowCredits / 1000)
                );
            } else {
                _agPromptFlowTierPill->setText("Monthly");
            }
        } else {
            _agPromptFlowVal->setText("--");
            _agPromptFlowTierPill->setText("Monthly");
        }

        if (snap.agQuotaGroups.empty()) {
            QLabel* noGroupsLbl = new QLabel("No quota groups reported", _agQuotaGroupsContainer);
            noGroupsLbl->setStyleSheet("color: #64748b; font-size: 11px;");
            _agQuotaGroupsLayout->addWidget(noGroupsLbl);
        } else {
            for (const auto& group : snap.agQuotaGroups) {
                QFrame* groupCard = new QFrame(_agQuotaGroupsContainer);
                groupCard->setObjectName("agQuotaGroupCard");
                groupCard->setStyleSheet(
                    "#agQuotaGroupCard { background-color: rgba(255, 255, 255, 0.025); border: 1px solid #1e293b; border-radius: 12px; }"
                );
                QVBoxLayout* grpLayout = new QVBoxLayout(groupCard);
                grpLayout->setContentsMargins(12, 10, 12, 10);
                grpLayout->setSpacing(8);

                QLabel* grpTitleLbl = new QLabel(QString::fromStdString(group.displayName), groupCard);
                grpTitleLbl->setStyleSheet("color: #f8fafc; font-size: 14px; font-weight: 700;");
                grpLayout->addWidget(grpTitleLbl);

                if (!group.description.empty()) {
                    QLabel* grpDescLbl = new QLabel(QString::fromStdString(group.description), groupCard);
                    grpDescLbl->setStyleSheet("color: #94a3b8; font-size: 11px;");
                    grpDescLbl->setWordWrap(true);
                    grpLayout->addWidget(grpDescLbl);
                }

                QGridLayout* bucketsGrid = new QGridLayout();
                bucketsGrid->setSpacing(10);
                bucketsGrid->setColumnStretch(0, 1);
                bucketsGrid->setColumnStretch(1, 1);

                for (size_t bIdx = 0; bIdx < group.buckets.size(); ++bIdx) {
                    const auto& b = group.buckets[bIdx];
                    QFrame* bucketCard = new QFrame(groupCard);
                    bucketCard->setObjectName("agBucketCard");
                    bucketCard->setStyleSheet(
                        "#agBucketCard { background-color: rgba(0, 0, 0, 0.22); border: 1px solid rgba(255, 255, 255, 0.06); border-radius: 8px; }"
                    );
                    QVBoxLayout* bktLayout = new QVBoxLayout(bucketCard);
                    bktLayout->setContentsMargins(10, 8, 10, 8);
                    bktLayout->setSpacing(6);

                    // Top row
                    QHBoxLayout* bktTopRow = new QHBoxLayout();
                    bktTopRow->setContentsMargins(0, 0, 0, 0);
                    bktTopRow->setSpacing(4);
                    QLabel* bktNameLbl = new QLabel(QString::fromStdString(b.displayName), bucketCard);
                    bktNameLbl->setStyleSheet("color: #94a3b8; font-size: 11px; font-weight: 600;");
                    const int roundedPct = static_cast<int>(std::round(b.remainingPct));
                    QLabel* bktPctLbl = new QLabel(QString("%1%").arg(roundedPct), bucketCard);
                    bktPctLbl->setStyleSheet(
                        QString("color: %1; font-family: 'Consolas', 'JetBrains Mono', monospace; font-size: 13px; font-weight: 800;")
                            .arg(bucketColorForPercent(b.remainingPct))
                    );
                    bktTopRow->addWidget(bktNameLbl);
                    bktTopRow->addStretch(1);
                    bktTopRow->addWidget(bktPctLbl);
                    bktLayout->addLayout(bktTopRow);

                    // Middle: Progress bar
                    QProgressBar* bktBar = new QProgressBar(bucketCard);
                    bktBar->setRange(0, 100);
                    bktBar->setValue(roundedPct);
                    bktBar->setTextVisible(false);
                    bktBar->setFixedHeight(7);
                    bktBar->setStyleSheet(bucketProgressBarStyle(b.remainingPct));
                    bktLayout->addWidget(bktBar);

                    // Bottom row
                    QHBoxLayout* bktBottomRow = new QHBoxLayout();
                    bktBottomRow->setContentsMargins(0, 0, 0, 0);
                    bktBottomRow->setSpacing(4);

                    QString winBadgeText;
                    if (b.window == "weekly") {
                        winBadgeText = "WEEKLY";
                    } else if (b.window == "5h" || b.window == "five_hour") {
                        winBadgeText = "5-HOUR";
                    } else if (!b.window.empty()) {
                        winBadgeText = QString::fromStdString(b.window).toUpper();
                    } else {
                        winBadgeText = "LIMIT";
                    }

                    QLabel* winBadge = new QLabel(winBadgeText, bucketCard);
                    winBadge->setStyleSheet(
                        "background-color: rgba(255, 255, 255, 0.06); color: #94a3b8; "
                        "border-radius: 4px; padding: 1px 6px; font-size: 9px; font-weight: 700; letter-spacing: 0.4px;"
                    );

                    QLabel* timerLbl = new QLabel(
                        QString::fromStdString(formatBucketTimerText(b.remainingPct, b.resetSeconds, b.description)),
                        bucketCard
                    );
                    timerLbl->setStyleSheet(
                        "color: #00f2fe; font-family: 'Consolas', 'JetBrains Mono', monospace; font-size: 11px; font-weight: 600;"
                    );

                    _agCountdownBindings.push_back({timerLbl, b.remainingPct, b.resetSeconds, b.description, true});

                    bktBottomRow->addWidget(winBadge);
                    bktBottomRow->addStretch(1);
                    bktBottomRow->addWidget(timerLbl);
                    bktLayout->addLayout(bktBottomRow);

                    bucketsGrid->addWidget(bucketCard, static_cast<int>(bIdx / 2), static_cast<int>(bIdx % 2));
                }
                grpLayout->addLayout(bucketsGrid);
                _agQuotaGroupsLayout->addWidget(groupCard);
            }
        }

        for (const auto& m : snap.agModels) {
            auto [baseModelName, tierStr] = splitModelNameAndTier(m.name);

            QWidget* rowWidget = new QWidget(_agModelsContainer);
            QHBoxLayout* rowLayout = new QHBoxLayout(rowWidget);
            rowLayout->setContentsMargins(0, 2, 0, 2);
            rowLayout->setSpacing(8);

            QString providerText = "AI";
            QString providerStyle =
                "background-color: rgba(148, 163, 184, 0.14); color: #cbd5e1; "
                "border: 1px solid #334155; border-radius: 5px; "
                "padding: 1px 6px; font-size: 10px; font-weight: 700;";
            if (baseModelName.contains("Claude", Qt::CaseInsensitive)) {
                providerText = "Claude";
                providerStyle =
                    "background-color: rgba(168, 85, 247, 0.16); color: #c084fc; "
                    "border: 1px solid rgba(168, 85, 247, 0.35); border-radius: 5px; "
                    "padding: 1px 6px; font-size: 10px; font-weight: 700;";
            } else if (baseModelName.contains("Gemini", Qt::CaseInsensitive)) {
                providerText = "Gemini";
                providerStyle =
                    "background-color: rgba(56, 189, 248, 0.16); color: #38bdf8; "
                    "border: 1px solid rgba(56, 189, 248, 0.35); border-radius: 5px; "
                    "padding: 1px 6px; font-size: 10px; font-weight: 700;";
            } else if (baseModelName.contains("GPT", Qt::CaseInsensitive) ||
                       baseModelName.contains("OpenAI", Qt::CaseInsensitive)) {
                providerText = "GPT";
                providerStyle =
                    "background-color: rgba(16, 185, 129, 0.16); color: #34d399; "
                    "border: 1px solid rgba(16, 185, 129, 0.35); border-radius: 5px; "
                    "padding: 1px 6px; font-size: 10px; font-weight: 700;";
            }

            QLabel* providerBadge = new QLabel(providerText, rowWidget);
            providerBadge->setFixedWidth(54);
            providerBadge->setAlignment(Qt::AlignCenter);
            providerBadge->setStyleSheet(providerStyle);

            QWidget* nameContainer = new QWidget(rowWidget);
            QHBoxLayout* nameLayout = new QHBoxLayout(nameContainer);
            nameLayout->setContentsMargins(0, 0, 0, 0);
            nameLayout->setSpacing(6);

            QLabel* nameLbl = new QLabel(baseModelName, nameContainer);
            nameLbl->setStyleSheet("color: #f8fafc; font-weight: 600; font-size: 12px;");
            nameLayout->addWidget(nameLbl);

            if (!tierStr.isEmpty()) {
                QLabel* tierBadge = new QLabel(tierStr, nameContainer);
                tierBadge->setStyleSheet(
                    "background-color: #0f172a; color: #94a3b8; "
                    "border: 1px solid #1e293b; border-radius: 4px; "
                    "padding: 1px 5px; font-size: 10px; font-weight: 600;"
                );
                nameLayout->addWidget(tierBadge);
            }
            nameContainer->setMinimumWidth(180);

            const int roundedPct = static_cast<int>(std::round(m.remainingPct));
            QProgressBar* bar = new QProgressBar(rowWidget);
            bar->setRange(0, 100);
            bar->setValue(roundedPct);
            bar->setTextVisible(false);
            bar->setFixedHeight(6);
            bar->setStyleSheet(progressBarStyleForPercent(m.remainingPct));

            QLabel* pctLbl = new QLabel(QString("%1%").arg(roundedPct), rowWidget);
            pctLbl->setMinimumWidth(44);
            pctLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            pctLbl->setStyleSheet(
                QString("color: %1; font-weight: 700; font-size: 12px;")
                    .arg(colorForRemainingPercent(m.remainingPct))
            );

            QLabel* resetLbl = new QLabel(
                QString::fromStdString(formatModelTimerText(m.remainingPct, m.resetSeconds)),
                rowWidget
            );
            resetLbl->setMinimumWidth(80);
            resetLbl->setAlignment(Qt::AlignCenter);
            resetLbl->setStyleSheet(
                "background-color: #0f172a; border: 1px solid #1e293b; "
                "border-radius: 6px; padding: 2px 6px; color: #94a3b8; font-size: 11px;"
            );

            _agCountdownBindings.push_back({resetLbl, m.remainingPct, m.resetSeconds, "", false});

            rowLayout->addWidget(providerBadge);
            rowLayout->addWidget(nameContainer);
            rowLayout->addWidget(bar, 1);
            rowLayout->addWidget(pctLbl);
            rowLayout->addWidget(resetLbl);
            _agModelsLayout->addWidget(rowWidget);
        }

        _agModelsToggleBtn->setText(
            QString("Individual Model Capacities (%1)  %2")
                .arg(snap.agModels.size())
                .arg(_agModelsExpanded ? "˄" : "˅")
        );
        _agModelsContainer->setVisible(_agModelsExpanded && !snap.agModels.empty());

        if (_actSummaryAg) {
            AimonQuotaSnapshot agOnly = snap;
            agOnly.reachable = true;
            agOnly.cursorAuthenticated = false;
            agOnly.claudeEnabled = false;
            _actSummaryAg->setText(QString::fromStdString(EdaBridgeAppConfig::formatTrayTooltipSummary(agOnly)));
        }
    } else {
        _agStatusDot->setStyleSheet("color: #ef4444; font-size: 13px;");
        _agHeaderLabel->setText(
            snap.agError.empty()
                ? QString("○ OFFLINE")
                : QString("○ %1").arg(QString::fromStdString(snap.agError))
        );
        _agHeaderLabel->setStyleSheet(offlineStatusPillStyle);
        _agHeaderLabel->setVisible(true);
        _agPlanPill->setVisible(false);
        _agGoogleOneCreditsVal->setText("--");
        _agPromptFlowVal->setText("--");
        _agModelsToggleBtn->setText("Individual Model Capacities (0)  ˅");
        _agModelsContainer->setVisible(false);
        if (_actSummaryAg) {
            _actSummaryAg->setText("Antigravity: Offline");
        }
    }

    // 2. Cursor
    QLabel* curPlanPill = findChild<QLabel*>("cursorPlanPill");
    QLabel* curModePill = findChild<QLabel*>("cursorModePill");
    QLabel* curOnDemandVal = findChild<QLabel*>("cursorOnDemandValue");
    QLabel* curReqCounter = findChild<QLabel*>("cursorReqCounterLabel");
    QLabel* curPctReadout = findChild<QLabel*>("cursorPctReadout");

    if (snap.cursorAuthenticated) {
        _cursorHeaderLabel->setText("● AUTHENTICATED");
        _cursorHeaderLabel->setStyleSheet(activeStatusPillStyle);
        if (curPlanPill) {
            curPlanPill->setText(
                snap.cursorPlanTier.empty()
                    ? QString("Pro")
                    : QString::fromStdString(snap.cursorPlanTier)
            );
            curPlanPill->setVisible(true);
        }
        if (curModePill) {
            curModePill->setText(
                snap.cursorUsageMode.empty()
                    ? QString("spend")
                    : QString::fromStdString(snap.cursorUsageMode)
            );
            curModePill->setVisible(true);
        }

        _cursorSpendLabel->setText(QString("$%1").arg(snap.cursorTotalSpendUsd, 0, 'f', 2));
        if (curOnDemandVal) {
            curOnDemandVal->setText(QString("$%1").arg(snap.cursorOnDemandSpendUsd, 0, 'f', 2));
        }

        double usedPct = 0.0;
        if (snap.cursorRequestsLimit > 0) {
            usedPct = (100.0 * static_cast<double>(snap.cursorRequestsUsed)) /
                      static_cast<double>(snap.cursorRequestsLimit);
            if (usedPct > 100.0) usedPct = 100.0;
            const int roundedUsedPct = static_cast<int>(usedPct + 0.5);
            _cursorProgressBar->setValue(roundedUsedPct);
            if (curReqCounter) {
                curReqCounter->setText(
                    QString("%1 / %2")
                        .arg(QString::fromStdString(formatNumberWithCommas(snap.cursorRequestsUsed)))
                        .arg(QString::fromStdString(formatNumberWithCommas(snap.cursorRequestsLimit)))
                );
            }
            if (curPctReadout) {
                curPctReadout->setText(QString("%1%").arg(roundedUsedPct));
                curPctReadout->setStyleSheet(
                    QString("color: %1; font-size: 12px; font-weight: 700;")
                        .arg(colorForSpendPercent(usedPct))
                );
            }
        } else {
            _cursorProgressBar->setValue(0);
            if (curReqCounter) {
                curReqCounter->setText(
                    QString("$%1 cycle spend").arg(snap.cursorTotalSpendUsd, 0, 'f', 2)
                );
            }
            if (curPctReadout) {
                curPctReadout->setText("0%");
                curPctReadout->setStyleSheet("color: #00f2fe; font-size: 12px; font-weight: 700;");
            }
        }
        _cursorProgressBar->setStyleSheet(spendProgressBarStyle(usedPct));
        _cursorResetLabel->setText(
            QString::fromStdString(formatHumanDate(snap.cursorCycleResetIso))
        );
        if (_actSummaryCursor) {
            AimonQuotaSnapshot curOnly = snap;
            curOnly.reachable = true;
            curOnly.agRunning = false;
            curOnly.claudeEnabled = false;
            _actSummaryCursor->setText(QString::fromStdString(EdaBridgeAppConfig::formatTrayTooltipSummary(curOnly)));
        }
    } else {
        _cursorHeaderLabel->setText(
            snap.cursorError.empty()
                ? QString("○ NOT SIGNED IN")
                : QString("○ %1").arg(QString::fromStdString(snap.cursorError))
        );
        _cursorHeaderLabel->setStyleSheet(offlineStatusPillStyle);
        if (curPlanPill) curPlanPill->setVisible(false);
        if (curModePill) curModePill->setVisible(false);
        _cursorSpendLabel->setText("--");
        if (curOnDemandVal) curOnDemandVal->setText("--");
        _cursorProgressBar->setValue(0);
        if (curReqCounter) curReqCounter->setText("N/A");
        if (curPctReadout) curPctReadout->setText("--");
        _cursorResetLabel->setText("--");
        if (_actSummaryCursor) {
            _actSummaryCursor->setText("Cursor: Offline");
        }
    }

    // 3. Claude Code
    clearLayoutWidgets(_claudeAccountsLayout);
    if (snap.claudeEnabled && !snap.claudeAccounts.empty()) {
        _claudeHeaderLabel->setText(
            QString("● %1 ACTIVE %2")
                .arg(snap.claudeAccounts.size())
                .arg(snap.claudeAccounts.size() == 1 ? "ACCOUNT" : "ACCOUNTS")
        );
        _claudeHeaderLabel->setStyleSheet(activeStatusPillStyle);

        for (const auto& acct : snap.claudeAccounts) {
            QFrame* card = new QFrame(_claudeAccountsContainer);
            card->setStyleSheet(
                "QFrame { background-color: #0b1120; border: 1px solid #1e293b; "
                "border-radius: 8px; padding: 10px; }"
            );
            QVBoxLayout* cardLayout = new QVBoxLayout(card);
            cardLayout->setContentsMargins(6, 6, 6, 6);
            cardLayout->setSpacing(8);

            // Account header row (Name + Tier pill)
            QHBoxLayout* acctHeader = new QHBoxLayout();
            acctHeader->setSpacing(8);
            QLabel* acctNameLbl = new QLabel(QString::fromStdString(acct.name), card);
            acctNameLbl->setStyleSheet(
                "color: #f8fafc; font-size: 13px; font-weight: 700; border: none; padding: 0;"
            );
            QLabel* acctTierPill = new QLabel(
                acct.tier.empty() ? QString("Claude") : QString::fromStdString(acct.tier),
                card
            );
            acctTierPill->setStyleSheet(
                "background-color: rgba(168, 85, 247, 0.16); color: #c084fc; "
                "border: 1px solid rgba(168, 85, 247, 0.35); border-radius: 6px; "
                "padding: 2px 8px; font-size: 11px; font-weight: 700;"
            );
            acctHeader->addWidget(acctNameLbl);
            acctHeader->addWidget(acctTierPill);
            acctHeader->addStretch(1);
            cardLayout->addLayout(acctHeader);

            // 3-column stat pills for 5h, 7d, and 30d windows
            QHBoxLayout* winRow = new QHBoxLayout();
            winRow->setSpacing(8);

            auto makeWindowStatPill = [&](const char* title, int64_t tokens, double costUsd) -> QFrame* {
                QFrame* pill = new QFrame(card);
                pill->setStyleSheet(
                    "QFrame { background-color: #111827; border: 1px solid #1e293b; "
                    "border-radius: 6px; padding: 6px 8px; }"
                );
                QVBoxLayout* pillLayout = new QVBoxLayout(pill);
                pillLayout->setContentsMargins(4, 2, 4, 2);
                pillLayout->setSpacing(2);
                QLabel* capLbl = new QLabel(title, pill);
                capLbl->setStyleSheet(
                    "color: #64748b; font-size: 10px; font-weight: 700; border: none; padding: 0;"
                );
                QLabel* tokLbl = new QLabel(
                    QString("%1 tok").arg(QString::fromStdString(formatNumberWithCommas(tokens))),
                    pill
                );
                tokLbl->setStyleSheet(
                    "color: #e2e8f0; font-size: 12px; font-weight: 700; border: none; padding: 0;"
                );
                QLabel* costLbl = new QLabel(
                    QString("$%1").arg(costUsd, 0, 'f', 2),
                    pill
                );
                costLbl->setStyleSheet(
                    "color: #38bdf8; font-size: 11px; font-weight: 600; border: none; padding: 0;"
                );
                pillLayout->addWidget(capLbl);
                pillLayout->addWidget(tokLbl);
                pillLayout->addWidget(costLbl);
                return pill;
            };

            winRow->addWidget(makeWindowStatPill("5H WINDOW", acct.tokens5h, acct.cost5hUsd), 1);
            winRow->addWidget(makeWindowStatPill("7D WINDOW", acct.tokens7d, acct.cost7dUsd), 1);
            winRow->addWidget(makeWindowStatPill("30D WINDOW", acct.tokensWindow, acct.costWindowUsd), 1);
            cardLayout->addLayout(winRow);

            if (acct.cycleConfigured && acct.spendLimitUsd > 0.0) {
                double pct = acct.estPctOfLimit;
                if (pct < 0.0) pct = 0.0;
                if (pct > 100.0) pct = 100.0;
                const int roundedPct = static_cast<int>(pct + 0.5);

                QHBoxLayout* cycHeader = new QHBoxLayout();
                QLabel* cycTitle = new QLabel("Cycle Budget", card);
                cycTitle->setStyleSheet(
                    "color: #94a3b8; font-size: 11px; font-weight: 600; border: none; padding: 0;"
                );
                QLabel* cycSummary = new QLabel(
                    QString("$%1 / $%2 (%3%)")
                        .arg(acct.costCycleUsd, 0, 'f', 2)
                        .arg(acct.spendLimitUsd, 0, 'f', 2)
                        .arg(roundedPct),
                    card
                );
                cycSummary->setStyleSheet(
                    QString("color: %1; font-size: 11px; font-weight: 700; border: none; padding: 0;")
                        .arg(colorForSpendPercent(pct))
                );
                cycHeader->addWidget(cycTitle);
                cycHeader->addStretch(1);
                cycHeader->addWidget(cycSummary);
                cardLayout->addLayout(cycHeader);

                QProgressBar* bar = new QProgressBar(card);
                bar->setRange(0, 100);
                bar->setValue(roundedPct);
                bar->setTextVisible(false);
                bar->setFixedHeight(6);
                bar->setStyleSheet(spendProgressBarStyle(pct));
                cardLayout->addWidget(bar);
            }
            _claudeAccountsLayout->addWidget(card);
        }

        if (_actSummaryClaude) {
            AimonQuotaSnapshot claudeOnly = snap;
            claudeOnly.reachable = true;
            claudeOnly.agRunning = false;
            claudeOnly.cursorAuthenticated = false;
            _actSummaryClaude->setText(QString::fromStdString(EdaBridgeAppConfig::formatTrayTooltipSummary(claudeOnly)));
        }
    } else {
        _claudeHeaderLabel->setText("○ NO CLAUDE CODE ACCOUNTS");
        _claudeHeaderLabel->setStyleSheet(offlineStatusPillStyle);
        if (_actSummaryClaude) {
            _actSummaryClaude->setText("Claude Code: N/A");
        }
    }

    QString tsStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    _quotaLastUpdatedLabel->setText(QString("Last updated: %1").arg(tsStr));
    updateQuotaCountdowns();
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
    bool kcAlready = false;
    bool fcAlready = false;

    bool okKc = EdaBridgeAppConfig::deployKiCadPlugin(
        appData, kcDir, err1, _settings.kicadPluginDir, false, &kcAlready
    );
    if (!okKc) {
        QString chosenKc = QFileDialog::getExistingDirectory(
            this,
            "KiCad Plugin Directory Not Found — Select KiCad 'scripting/plugins' Folder",
            QString::fromStdString(appData)
        );
        if (!chosenKc.isEmpty()) {
            _settings.kicadPluginDir = chosenKc.toStdString();
            std::string saveErr;
            EdaBridgeAppConfig::saveToFile(_configFilePath, _settings, saveErr);
            okKc = EdaBridgeAppConfig::deployKiCadPlugin(
                appData, kcDir, err1, _settings.kicadPluginDir, false, &kcAlready
            );
        }
    }

    bool okFc = EdaBridgeAppConfig::deployFreeCadPlugin(
        appData, fcDir, err2, _settings.freecadModDir, false, &fcAlready
    );
    if (!okFc) {
        QString chosenFc = QFileDialog::getExistingDirectory(
            this,
            "FreeCAD Mod Directory Not Found — Select FreeCAD 'Mod' Folder",
            QString::fromStdString(appData)
        );
        if (!chosenFc.isEmpty()) {
            _settings.freecadModDir = chosenFc.toStdString();
            std::string saveErr;
            EdaBridgeAppConfig::saveToFile(_configFilePath, _settings, saveErr);
            okFc = EdaBridgeAppConfig::deployFreeCadPlugin(
                appData, fcDir, err2, _settings.freecadModDir, false, &fcAlready
            );
        }
    }

    fs::path eextPath = fs::path(_configFilePath).parent_path() / "aimon-easyeda-bridge.eext";
    std::string err3;
    const bool okEe = EdaBridgeAppConfig::writeEasyEdaEextFile(eextPath.string(), err3);

    const std::string kcStatus = !okKc
        ? ("Error: " + err1)
        : (kcAlready ? ("Already deployed (not overwritten): " + kcDir)
                     : ("Deployed to " + kcDir));
    const std::string fcStatus = !okFc
        ? ("Error: " + err2)
        : (fcAlready ? ("Already deployed (not overwritten): " + fcDir)
                     : ("Deployed to " + fcDir));

    if (_lblSettingsStatus) {
        _lblSettingsStatus->setText(
            QString("KiCad: %1 | FreeCAD: %2")
                .arg(QString::fromStdString(kcStatus))
                .arg(QString::fromStdString(fcStatus))
        );
    }

    if (okKc && okFc && okEe) {
        QMessageBox::information(
            this,
            "aimon — Companion Plugins Status",
            QString("Companion plugins status:\n\n"
                    "• KiCad Plugin:\n  %1\n\n"
                    "• FreeCAD Plugin:\n  %2\n\n"
                    "• EasyEDA Pro Extension:\n  %3")
                .arg(QString::fromStdString(kcStatus))
                .arg(QString::fromStdString(fcStatus))
                .arg(QString::fromStdString(eextPath.string()))
        );
    } else {
        QMessageBox::warning(
            this,
            "aimon — Plugin Deployment Warning",
            QString("Plugin deployment reported issues:\nKiCad: %1\nFreeCAD: %2\nEasyEDA: %3")
                .arg(QString::fromStdString(kcStatus))
                .arg(QString::fromStdString(fcStatus))
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

std::string EdaTrayWindow::formatHumanDate(const std::string& iso) {
    if (iso.empty()) {
        return "--";
    }
    if (iso.size() >= 10 && iso[4] == '-' && iso[7] == '-') {
        auto isDigitRange = [&](size_t start, size_t len) -> bool {
            for (size_t i = start; i < start + len; ++i) {
                if (iso[i] < '0' || iso[i] > '9') return false;
            }
            return true;
        };
        if (isDigitRange(0, 4) && isDigitRange(5, 2) && isDigitRange(8, 2)) {
            const int year = std::stoi(iso.substr(0, 4));
            const int month = std::stoi(iso.substr(5, 2));
            const int day = std::stoi(iso.substr(8, 2));
            static const char* kMonths[12] = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
            };
            if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
                std::ostringstream oss;
                oss << kMonths[month - 1] << " " << day << ", " << year;
                return oss.str();
            }
        }
    }
    return iso;
}

std::string EdaTrayWindow::formatNumberWithCommas(int64_t value) {
    const bool negative = value < 0;
    uint64_t absVal = negative
        ? static_cast<uint64_t>(-(value + 1)) + 1ULL
        : static_cast<uint64_t>(value);
    std::string raw = std::to_string(absVal);
    std::string out;
    out.reserve(raw.size() + raw.size() / 3 + 2);
    if (negative) {
        out.push_back('-');
    }
    const size_t len = raw.size();
    for (size_t i = 0; i < len; ++i) {
        if (i > 0 && ((len - i) % 3 == 0)) {
            out.push_back(',');
        }
        out.push_back(raw[i]);
    }
    return out;
}

void EdaTrayWindow::updateQuotaCountdowns() {
    const int64_t nowSteady = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
    const int64_t elapsed = (_lastQuotaFetchSteadySec > 0 && nowSteady > _lastQuotaFetchSteadySec)
        ? (nowSteady - _lastQuotaFetchSteadySec)
        : 0;
    for (const auto& b : _agCountdownBindings) {
        if (!b.label) {
            continue;
        }
        const int64_t rem = (b.initialResetSeconds > 0)
            ? std::max<int64_t>(0, b.initialResetSeconds - elapsed)
            : b.initialResetSeconds;
        b.label->setText(QString::fromStdString(
            b.isBucket ? formatBucketTimerText(b.remainingPct, rem, b.description)
                       : formatModelTimerText(b.remainingPct, rem)
        ));
    }
}

std::string EdaTrayWindow::formatCountdown(int64_t totalSeconds) {
    if (totalSeconds < 0) {
        totalSeconds = 0;
    }
    const int64_t days = totalSeconds / 86400;
    const int64_t hours = (totalSeconds % 86400) / 3600;
    const int64_t minutes = (totalSeconds % 3600) / 60;
    const int64_t seconds = totalSeconds % 60;

    std::ostringstream oss;
    if (days > 0) {
        oss << days << "d " << hours << "h "
            << std::setfill('0') << std::setw(2) << minutes << "m";
    } else if (hours > 0) {
        oss << hours << "h "
            << std::setfill('0') << std::setw(2) << minutes << "m "
            << std::setfill('0') << std::setw(2) << seconds << "s";
    } else {
        oss << minutes << "m "
            << std::setfill('0') << std::setw(2) << seconds << "s";
    }
    return oss.str();
}

std::string EdaTrayWindow::formatBucketTimerText(double remainingPct,
                                                 int64_t remainingSeconds,
                                                 const std::string& description) {
    if (remainingPct >= 99.9 && remainingSeconds <= 0) {
        return "Active";
    }
    if (remainingSeconds > 8 * 86400 || remainingSeconds < 0) {
        std::string lowerDesc = description;
        for (char& c : lowerDesc) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const std::string needle = "refresh in ";
        const auto pos = lowerDesc.find(needle);
        if (pos != std::string::npos) {
            std::string extracted = description.substr(pos + needle.size());
            while (!extracted.empty() &&
                   (extracted.back() == '.' || std::isspace(static_cast<unsigned char>(extracted.back())))) {
                extracted.pop_back();
            }
            while (!extracted.empty() && std::isspace(static_cast<unsigned char>(extracted.front()))) {
                extracted.erase(extracted.begin());
            }
            if (!extracted.empty()) {
                return "Resets in " + extracted;
            }
        }
    }
    if (remainingSeconds > 0) {
        return "Resets in " + formatCountdown(remainingSeconds);
    }
    return (remainingPct >= 99.9) ? "Active" : "Ready to reset";
}

std::string EdaTrayWindow::formatModelTimerText(double remainingPct,
                                                int64_t remainingSeconds) {
    if (remainingPct >= 99.9 && remainingSeconds <= 0) {
        return "Active";
    }
    if (remainingSeconds > 0) {
        return "Resets in " + formatCountdown(remainingSeconds);
    }
    return (remainingPct >= 99.9) ? "Active" : "Ready";
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
