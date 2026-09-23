/*
 * AimonRepository.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.data

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.TimeUnit

class AimonRepository(
    private val context: Context,
    private val pairingManager: PairingManager
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .writeTimeout(10, TimeUnit.SECONDS)
        .build()

    private val _pendingApprovals = MutableStateFlow<List<ApprovalRequest>>(emptyList())
    val pendingApprovals: StateFlow<List<ApprovalRequest>> = _pendingApprovals.asStateFlow()

    private val _quotaStatus = MutableStateFlow<QuotaStatus?>(null)
    val quotaStatus: StateFlow<QuotaStatus?> = _quotaStatus.asStateFlow()

    private val _telemetry = MutableStateFlow<TelemetryOverview?>(null)
    val telemetry: StateFlow<TelemetryOverview?> = _telemetry.asStateFlow()

    private val _isConnected = MutableStateFlow(false)
    val isConnected: StateFlow<Boolean> = _isConnected.asStateFlow()

    suspend fun pairWithDaemon(host: String, port: Int, secret: String): Result<String> {
        val deviceId = pairingManager.getDeviceId()
        val deviceName = pairingManager.getDeviceName()

        val jsonBody = JSONObject().apply {
            put("secret", secret)
            put("device_id", deviceId)
            put("device_name", deviceName)
        }.toString()

        val request = Request.Builder()
            .url("http://$host:$port/api/mobile/pair")
            .post(jsonBody.toRequestBody("application/json".toMediaType()))
            .build()

        return try {
            val response = httpClient.newCall(request).execute()
            if (response.isSuccessful) {
                val respStr = response.body?.string() ?: ""
                val respJson = JSONObject(respStr)
                val token = respJson.optString("token", "")
                if (token.isNotEmpty()) {
                    pairingManager.savePairing(host, port, secret, token)
                    Result.success(token)
                } else {
                    Result.failure(Exception("Missing token in pairing response"))
                }
            } else {
                Result.failure(Exception("Pairing failed with HTTP ${response.code}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun refreshQuotas(): Result<QuotaStatus> {
        val config = pairingManager.config.value
        val request = Request.Builder()
            .url("${config.httpBaseUrl}/api/status")
            .get()
            .build()

        return try {
            val response = httpClient.newCall(request).execute()
            if (response.isSuccessful) {
                val respStr = response.body?.string() ?: ""
                val json = JSONObject(respStr)
                val status = parseQuotaStatus(json)
                _quotaStatus.value = status
                Result.success(status)
            } else {
                Result.failure(Exception("HTTP ${response.code}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun refreshApprovals(): Result<List<ApprovalRequest>> {
        val config = pairingManager.config.value
        val request = Request.Builder()
            .url("${config.httpBaseUrl}/api/approvals/pending")
            .get()
            .build()

        return try {
            val response = httpClient.newCall(request).execute()
            if (response.isSuccessful) {
                val respStr = response.body?.string() ?: "[]"
                val jsonArr = JSONArray(respStr)
                val list = mutableListOf<ApprovalRequest>()
                for (i in 0 until jsonArr.length()) {
                    val item = jsonArr.getJSONObject(i)
                    list.add(
                        ApprovalRequest(
                            approvalId = item.optString("approval_id"),
                            agentType = item.optString("agent_type", "antigravity"),
                            toolName = item.optString("tool_name", ""),
                            workspace = item.optString("workspace", ""),
                            toolArgs = parseArgs(item.optJSONObject("tool_args")),
                            reason = item.optString("reason", ""),
                            requestedAt = item.optLong("requested_at", 0),
                            timeoutSeconds = item.optInt("timeout_seconds", 120),
                            remainingSeconds = item.optInt("remaining_seconds", 120)
                        )
                    )
                }
                _pendingApprovals.value = list
                Result.success(list)
            } else {
                Result.failure(Exception("HTTP ${response.code}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun submitDecision(approvalId: String, decision: String): Result<Boolean> {
        val config = pairingManager.config.value
        val jsonBody = JSONObject().apply {
            put("approval_id", approvalId)
            put("decision", decision)
        }.toString()

        val request = Request.Builder()
            .url("${config.httpBaseUrl}/api/approvals/decision")
            .post(jsonBody.toRequestBody("application/json".toMediaType()))
            .build()

        return try {
            val response = httpClient.newCall(request).execute()
            if (response.isSuccessful) {
                refreshApprovals()
                Result.success(true)
            } else {
                Result.failure(Exception("Decision HTTP ${response.code}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun refreshTelemetry(): Result<TelemetryOverview> {
        val config = pairingManager.config.value
        val request = Request.Builder()
            .url("${config.httpBaseUrl}/api/telemetry/overview")
            .get()
            .build()

        return try {
            val response = httpClient.newCall(request).execute()
            if (response.isSuccessful) {
                val respStr = response.body?.string() ?: "{}"
                val json = JSONObject(respStr)
                val telem = TelemetryOverview(
                    activeSessions = json.optInt("active_sessions", 0),
                    totalToolCalls = json.optLong("total_tool_calls", 0),
                    avgTurnLatencyMs = json.optDouble("avg_turn_latency_ms", 0.0),
                    errorRatePct = json.optDouble("error_rate_pct", 0.0)
                )
                _telemetry.value = telem
                Result.success(telem)
            } else {
                Result.failure(Exception("HTTP ${response.code}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    fun setConnected(connected: Boolean) {
        _isConnected.value = connected
    }

    fun handleIncomingApproval(approval: ApprovalRequest) {
        val current = _pendingApprovals.value.toMutableList()
        current.removeAll { it.approvalId == approval.approvalId }
        current.add(0, approval)
        _pendingApprovals.value = current
    }

    fun removeApproval(approvalId: String) {
        val current = _pendingApprovals.value.toMutableList()
        current.removeAll { it.approvalId == approvalId }
        _pendingApprovals.value = current
    }

    private fun parseQuotaStatus(json: JSONObject): QuotaStatus {
        val agJson = json.optJSONObject("antigravity")
        val curJson = json.optJSONObject("cursor")

        val antigravity = agJson?.let {
            val groupsArr = it.optJSONArray("quota_groups") ?: JSONArray()
            val groupsList = mutableListOf<QuotaGroup>()
            for (i in 0 until groupsArr.length()) {
                val g = groupsArr.getJSONObject(i)
                groupsList.add(
                    QuotaGroup(
                        name = g.optString("name"),
                        description = g.optString("description"),
                        weeklyLimitPct = g.optDouble("weekly_limit_remaining_pct", 100.0),
                        weeklyResetCountdown = g.optString("weekly_reset_countdown", ""),
                        fiveHourLimitPct = g.optDouble("five_hour_limit_remaining_pct", 100.0),
                        fiveHourResetCountdown = g.optString("five_hour_reset_countdown", "")
                    )
                )
            }

            AntigravityStatus(
                availableCredits = it.optLong("available_credits", 0),
                planName = it.optString("plan_name", "Google AI Ultra"),
                modelGroups = groupsList
            )
        }

        val cursor = curJson?.let {
            val spendArr = it.optJSONArray("spend_by_model") ?: JSONArray()
            val spendList = mutableListOf<CursorModelSpend>()
            for (i in 0 until spendArr.length()) {
                val s = spendArr.getJSONObject(i)
                spendList.add(
                    CursorModelSpend(
                        modelName = s.optString("model_name"),
                        spendAmount = s.optDouble("spend_amount", 0.0),
                        percentOfTotal = s.optDouble("percent_of_total", 0.0)
                    )
                )
            }

            CursorStatus(
                fastRequestsUsed = it.optLong("fast_requests_used", 0),
                fastRequestsLimit = it.optLong("fast_requests_limit", 0),
                percentUsed = it.optDouble("percent_used", 0.0),
                billingCycleReset = it.optString("billing_cycle_reset", ""),
                daysRemaining = it.optInt("days_remaining", 0),
                totalSpend = it.optDouble("total_spend", 0.0),
                spendByModel = spendList
            )
        }

        return QuotaStatus(antigravity = antigravity, cursor = cursor)
    }

    private fun parseArgs(json: JSONObject?): Map<String, Any?> {
        if (json == null) return emptyMap()
        val map = mutableMapOf<String, Any?>()
        val keys = json.keys()
        while (keys.hasNext()) {
            val k = keys.next()
            map[k] = json.opt(k)
        }
        return map
    }
}
