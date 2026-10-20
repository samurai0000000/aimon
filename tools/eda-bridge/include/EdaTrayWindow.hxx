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
    void populateTelemetryUi(const EdaBridgeTelemetrySnapshot& snap, bool forceRebuild = false);
    void updateTelemetryRelativeTimes();
    static std::string formatRelativeAgo(int64_t epochSec, int64_t nowEpochSec);

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
    void updateQuotaCountdowns();
    static std::string formatCountdown(int64_t totalSeconds);
    static std::string formatBucketTimerText(double remainingPct,
                                             int64_t remainingSeconds,
                                             const std::string& description);
    static std::string formatModelTimerText(double remainingPct,
                                            int64_t remainingSeconds);

    EdaRpcServer& _server;
    EdaBridgeAppSettings _settings;
    std::string _configFilePath;
    AimonQuotaSnapshot _lastQuotaSnap;
    bool _quitting = false;
    bool _syncingUi = false;

    QTabWidget* _tabWidget = nullptr;

    struct LiveCountdownBinding {
        QLabel* label = nullptr;
        double remainingPct = 100.0;
        int64_t initialResetSeconds = 0;
        std::string description;
        bool isBucket = true;
    };

    int64_t _lastQuotaFetchSteadySec = 0;
    bool _agModelsExpanded = false;
    std::vector<LiveCountdownBinding> _agCountdownBindings;

    // Tab 1: AI Quotas — Google Antigravity
    QLabel* _quotaConnectionBanner = nullptr;
    QLabel* _agStatusDot = nullptr;
    QLabel* _agHeaderLabel = nullptr;
    QLabel* _agPlanPill = nullptr;
    QLabel* _agGoogleOneCreditsVal = nullptr;
    QLabel* _agCreditMinPill = nullptr;
    QLabel* _agPromptFlowVal = nullptr;
    QLabel* _agPromptFlowTierPill = nullptr;
    QWidget* _agQuotaGroupsContainer = nullptr;
    QVBoxLayout* _agQuotaGroupsLayout = nullptr;
    QPushButton* _agModelsToggleBtn = nullptr;
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

    // Tab 2: EDA/CAD
    struct LiveRelativeTimeBinding {
        QLabel* label = nullptr;
        int64_t callEpoch = 0;
        std::string prefix;
    };

    struct BridgeDomainCardWidgets {
        QLabel* callsBadge = nullptr;
        QProgressBar* shareBar = nullptr;
        QLabel* detailLabel = nullptr;
    };

    std::vector<LiveRelativeTimeBinding> _edaRelativeTimeBindings;
    int64_t _telemetryResetEpoch = 0;
    uint64_t _lastRenderedTelTotalCalls = UINT64_MAX;
    uint64_t _lastRenderedTelSuccessCalls = 0;
    uint64_t _lastRenderedTelFailedCalls = 0;
    int64_t _lastRenderedTelLastEpoch = -1;
    size_t _lastRenderedTelFuncCount = 0;
    size_t _lastRenderedTelRecentCount = 0;
    bool _edaRecentCallsExpanded = true;

    QLabel* _lblRpcStatus = nullptr;
    QLabel* _lblGatewayStatus = nullptr;
    QLabel* _lblEasyEdaStatus = nullptr;
    QLabel* _lblKiCadStatus = nullptr;
    QLabel* _lblFreeCadStatus = nullptr;

    QLabel* _lblTelemetryStatusPill = nullptr;
    QPushButton* _btnResetTelemetry = nullptr;
    QLabel* _lblTelTotalCalls = nullptr;
    QLabel* _lblTelTotalSub = nullptr;
    QLabel* _lblTelSuccessRate = nullptr;
    QLabel* _lblTelSuccessSub = nullptr;
    QLabel* _lblTelAvgLatency = nullptr;
    QLabel* _lblTelLatencySub = nullptr;
    QLabel* _lblTelTopFunc = nullptr;
    QLabel* _lblTelTopFuncSub = nullptr;

    BridgeDomainCardWidgets _domCardEasyEda;
    BridgeDomainCardWidgets _domCardKiCad;
    BridgeDomainCardWidgets _domCardFreeCad;
    BridgeDomainCardWidgets _domCardAccelerators;

    QLabel* _lblFunctionsHeader = nullptr;
    QWidget* _edaFunctionsContainer = nullptr;
    QVBoxLayout* _edaFunctionsLayout = nullptr;

    QPushButton* _btnToggleRecentCalls = nullptr;
    QWidget* _edaRecentCallsContainer = nullptr;
    QVBoxLayout* _edaRecentCallsLayout = nullptr;

    QCheckBox* _chkEnableEasyEda = nullptr;
    QCheckBox* _chkEnableKiCad = nullptr;
    QCheckBox* _chkEnableFreeCad = nullptr;
    QCheckBox* _chkAutoDeployPlugins = nullptr;
    QCheckBox* _chkStartMinimized = nullptr;
    QCheckBox* _chkLaunchAtLogin = nullptr;

    QLineEdit* _edtAimonHost = nullptr;
    QSpinBox* _spnAimonWebPort = nullptr;
    QSpinBox* _spnGatewayPort = nullptr;
    QLineEdit* _edtAuthSecret = nullptr;
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
