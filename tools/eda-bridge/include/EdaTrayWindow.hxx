/*
 * EdaTrayWindow.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef AIMON_EDA_TRAY_WINDOW_HXX
#define AIMON_EDA_TRAY_WINDOW_HXX

#ifdef AIMON_EDA_BRIDGE_WITH_QT

#include <cstdint>
#include <string>
#include <vector>
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include "EdaBridgeAppConfig.hxx"
#include "EdaRpcServer.hxx"

namespace aimon {

class EdaTrayWindow : public QDialog {
public:
    EdaTrayWindow(EdaRpcServer& server,
                  const EdaBridgeAppSettings& initialSettings,
                  const std::string& configFilePath,
                  QWidget* parent = nullptr);
    ~EdaTrayWindow() override;

    void showQuotasTab();
    void showBridgesTab();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildUi();
    void buildTrayIcon();
    QIcon createBrandAppIcon() const;
    QIcon createStatusTrayIcon(bool healthy, bool activePlugin) const;

    void refreshLiveStatus();
    void refreshAiQuotas(bool triggerRemoteRefresh);
    void populateQuotasUi(const AimonQuotaSnapshot& snap);

    void syncControlsFromSettings();
    void collectSettingsFromControls();
    void applyRuntimeToggles();
    void applyAndSaveSettings();
    void updateWindowsRunAtLoginRegistry(bool enable);

    void onDeployPluginsClicked();
    void onExportEasyEdaExtensionClicked();
    void openWebDashboard();
    void quitApplication();

    static std::string formatDurationCompact(int64_t seconds);
    static std::string formatTokensCompact(int64_t tokens);
    static std::string formatHumanDate(const std::string& iso);
    static std::string formatNumberWithCommas(int64_t value);

    EdaRpcServer& _server;
    EdaBridgeAppSettings _settings;
    std::string _configFilePath;
    AimonQuotaSnapshot _lastQuotaSnap;
    bool _quitting = false;
    bool _syncingUi = false;

    QTabWidget* _tabWidget = nullptr;

    // Tab 1: AI Quotas
    QLabel* _quotaConnectionBanner = nullptr;
    QLabel* _agHeaderLabel = nullptr;
    QLabel* _agCreditsLabel = nullptr;
    QWidget* _agModelsContainer = nullptr;
    QVBoxLayout* _agModelsLayout = nullptr;

    QLabel* _cursorHeaderLabel = nullptr;
    QLabel* _cursorSpendLabel = nullptr;
    QProgressBar* _cursorProgressBar = nullptr;
    QLabel* _cursorResetLabel = nullptr;

    QLabel* _claudeHeaderLabel = nullptr;
    QWidget* _claudeAccountsContainer = nullptr;
    QVBoxLayout* _claudeAccountsLayout = nullptr;

    QLabel* _quotaLastUpdatedLabel = nullptr;
    QPushButton* _btnRefreshQuotas = nullptr;
    QPushButton* _btnOpenWebDashboard = nullptr;

    // Tab 2: EDA / CAD Bridges & Settings
    QLabel* _lblRpcStatus = nullptr;
    QLabel* _lblGatewayStatus = nullptr;
    QLabel* _lblEasyEdaStatus = nullptr;
    QLabel* _lblKiCadStatus = nullptr;
    QLabel* _lblFreeCadStatus = nullptr;

    QCheckBox* _chkEnableEasyEda = nullptr;
    QCheckBox* _chkEnableKiCad = nullptr;
    QCheckBox* _chkEnableFreeCad = nullptr;
    QCheckBox* _chkAutoDeployPlugins = nullptr;
    QCheckBox* _chkStartMinimized = nullptr;
    QCheckBox* _chkLaunchAtLogin = nullptr;
    QCheckBox* _chkDisallowExports = nullptr;

    QLineEdit* _edtAimonHost = nullptr;
    QSpinBox* _spnAimonWebPort = nullptr;
    QSpinBox* _spnGatewayPort = nullptr;
    QComboBox* _cmbConnectionMode = nullptr;
    QLineEdit* _edtListenHost = nullptr;
    QSpinBox* _spnRpcPort = nullptr;
    QSpinBox* _spnPluginPort = nullptr;
    QLineEdit* _edtAuthSecret = nullptr;
    QLineEdit* _edtAllowedRoot = nullptr;
    QLineEdit* _edtKiCadBin = nullptr;
    QLineEdit* _edtFreeCadBin = nullptr;
    QLabel* _lblSettingsStatus = nullptr;

    // System Tray Icon & Menu
    QSystemTrayIcon* _trayIcon = nullptr;
    QMenu* _trayMenu = nullptr;
    QAction* _actSummaryAg = nullptr;
    QAction* _actSummaryCursor = nullptr;
    QAction* _actSummaryClaude = nullptr;
    QAction* _actSummaryBridges = nullptr;
    QAction* _actTrayEasyEda = nullptr;
    QAction* _actTrayKiCad = nullptr;
    QAction* _actTrayFreeCad = nullptr;
    QAction* _actTrayLockExports = nullptr;

    QTimer* _statusTimer = nullptr;
    QTimer* _quotaTimer = nullptr;
};

} // namespace aimon

#endif // AIMON_EDA_BRIDGE_WITH_QT

#endif // AIMON_EDA_TRAY_WINDOW_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
