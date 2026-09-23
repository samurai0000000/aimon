/*
 * Models.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.data

data class ApprovalRequest(
    val approvalId: String,
    val agentType: String,
    val toolName: String,
    val workspace: String,
    val toolArgs: Map<String, Any?>,
    val reason: String,
    val requestedAt: Long,
    val timeoutSeconds: Int,
    val remainingSeconds: Int = timeoutSeconds
)

data class QuotaStatus(
    val antigravity: AntigravityStatus? = null,
    val cursor: CursorStatus? = null,
    val lastUpdated: Long = System.currentTimeMillis()
)

data class AntigravityStatus(
    val availableCredits: Long = 0,
    val planName: String = "Google AI Ultra",
    val modelGroups: List<QuotaGroup> = emptyList(),
    val individualModels: List<ModelCapacity> = emptyList()
)

data class QuotaGroup(
    val name: String,
    val description: String,
    val weeklyLimitPct: Double,
    val weeklyResetCountdown: String,
    val fiveHourLimitPct: Double,
    val fiveHourResetCountdown: String
)

data class ModelCapacity(
    val modelId: String,
    val displayName: String,
    val remainingPct: Double,
    val resetCountdown: String
)

data class CursorStatus(
    val fastRequestsUsed: Long = 0,
    val fastRequestsLimit: Long = 0,
    val percentUsed: Double = 0.0,
    val billingCycleReset: String = "",
    val daysRemaining: Int = 0,
    val totalSpend: Double = 0.0,
    val spendByModel: List<CursorModelSpend> = emptyList()
)

data class CursorModelSpend(
    val modelName: String,
    val spendAmount: Double,
    val percentOfTotal: Double
)

data class TelemetryOverview(
    val activeSessions: Int = 0,
    val totalToolCalls: Long = 0,
    val avgTurnLatencyMs: Double = 0.0,
    val errorRatePct: Double = 0.0
)

data class ConnectionConfig(
    val host: String = "127.0.0.1",
    val port: Int = 3883,
    val secret: String = "",
    val token: String = "",
    val isPaired: Boolean = false
) {
    val httpBaseUrl: String
        get() = "http://$host:$port"

    val wsUrl: String
        get() = "ws://$host:$port/ws/mobile"
}
