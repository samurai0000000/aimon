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

import kotlinx.coroutines.withContext

class AimonRepository(
    private val context: Context? = null,
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

    suspend fun pairWithDaemon(host: String, port: Int, secret: String): Result<String> = withContext(Dispatchers.IO) {
        val cleanHost = host.trim()
            .removePrefix("http://")
            .removePrefix("https://")
            .removeSuffix("/")
        val cleanSecret = secret.trim()

        val deviceId = pairingManager.getDeviceId()
        val deviceName = pairingManager.getDeviceName()

        val jsonBody = JSONObject().apply {
            put("secret", cleanSecret)
            put("device_id", deviceId)
            put("device_name", deviceName)
        }.toString()

        val url = "http://$cleanHost:$port/api/mobile/pair"
        val request = Request.Builder()
            .url(url)
            .post(jsonBody.toRequestBody("application/json".toMediaType()))
            .build()

        try {
            val response = httpClient.newCall(request).execute()
            val respStr = response.body?.string() ?: ""
            if (response.isSuccessful) {
                val respJson = JSONObject(respStr)
                val token = respJson.optString("token", "")
                if (token.isNotEmpty()) {
                    pairingManager.savePairing(cleanHost, port, cleanSecret, token)
                    setConnected(true)
                    Result.success(token)
                } else {
                    Result.failure(Exception("Missing token in daemon response"))
                }
            } else {
                val errReason = try {
                    JSONObject(respStr).optString("error", "")
                } catch (_: Exception) {
                    ""
                }
                val msg = if (errReason.isNotEmpty()) errReason else "HTTP ${response.code}: ${response.message}"
                Result.failure(Exception(msg))
            }
        } catch (e: Exception) {
            val errMsg = when (e) {
                is java.net.ConnectException -> "Connection refused ($cleanHost:$port). Check IP/port and verify aimon is running."
                is java.net.SocketTimeoutException -> "Connection timed out connecting to $cleanHost:$port."
                is java.net.UnknownHostException -> "Could not resolve hostname '$cleanHost'."
                else -> e.localizedMessage ?: e.javaClass.simpleName
            }
            Result.failure(Exception(errMsg, e))
        }
    }

    suspend fun refreshQuotas(): Result<QuotaStatus> = withContext(Dispatchers.IO) {
        val config = pairingManager.config.value
        val cleanHost = config.cleanHost
        val url = "http://$cleanHost:${config.port}/api/status"
        val request = Request.Builder()
            .url(url)
            .addHeader("Authorization", "Bearer ${config.token}")
            .get()
            .build()

        try {
            val response = httpClient.newCall(request).execute()
            if (response.isSuccessful) {
                val respStr = response.body?.string() ?: ""
                val json = JSONObject(respStr)
                val status = parseQuotaStatus(json)
                _quotaStatus.value = status
                Result.success(status)
            } else {
                Result.failure(Exception("HTTP ${response.code}: ${response.message}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun refreshApprovals(): Result<List<ApprovalRequest>> = withContext(Dispatchers.IO) {
        val config = pairingManager.config.value
        val cleanHost = config.cleanHost
        val url = "http://$cleanHost:${config.port}/api/approvals/pending"
        val request = Request.Builder()
            .url(url)
            .addHeader("Authorization", "Bearer ${config.token}")
            .get()
            .build()

        try {
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
                Result.failure(Exception("HTTP ${response.code}: ${response.message}"))
            }
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun submitDecision(approvalId: String, decision: String): Result<Boolean> = withContext(Dispatchers.IO) {
        removeApproval(approvalId)
        val config = pairingManager.config.value
        val cleanHost = config.cleanHost
        val url = "http://$cleanHost:${config.port}/api/approvals/decision"

        val jsonBody = JSONObject().apply {
            put("approval_id", approvalId)
            put("decision", decision)
        }.toString()

        val request = Request.Builder()
            .url(url)
            .addHeader("Authorization", "Bearer ${config.token}")
            .post(jsonBody.toRequestBody("application/json".toMediaType()))
            .build()

        try {
            val response = httpClient.newCall(request).execute()
            if (response.isSuccessful) {
                refreshApprovals()
                Result.success(true)
            } else {
                refreshApprovals()
                Result.failure(Exception("Decision HTTP ${response.code}"))
            }
        } catch (e: Exception) {
            refreshApprovals()
            Result.failure(e)
        }
    }

    suspend fun refreshTelemetry(): Result<TelemetryOverview> = withContext(Dispatchers.IO) {
        val config = pairingManager.config.value
        val cleanHost = config.cleanHost
        val url = "http://$cleanHost:${config.port}/api/telemetry/overview"
        val request = Request.Builder()
            .url(url)
            .addHeader("Authorization", "Bearer ${config.token}")
            .get()
            .build()

        try {
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
                val gName = g.optString("display_name", g.optString("name", "Group $i"))
                val gDesc = g.optString("description", "")

                var weeklyPct = 100.0
                var weeklyReset = ""
                var fiveHPct = 100.0
                var fiveHReset = ""

                val bucketsArr = g.optJSONArray("buckets")
                if (bucketsArr != null) {
                    for (bIdx in 0 until bucketsArr.length()) {
                        val b = bucketsArr.getJSONObject(bIdx)
                        val bucketId = b.optString("bucket_id", "")
                        val window = b.optString("window", "")
                        val frac = b.optDouble("remaining_fraction", 1.0)
                        val resetIso = b.optString("reset_time_iso", "")
                        val desc = b.optString("description", "")
                        val remSec = b.optLong("reset_time_remaining_seconds", 0L)
                        val countdown = formatResetDuration(remSec, desc, resetIso, frac)

                        if (bucketId.contains("weekly") || window == "weekly") {
                            weeklyPct = frac * 100.0
                            weeklyReset = countdown
                        } else if (bucketId.contains("5h") || window == "5h") {
                            fiveHPct = frac * 100.0
                            fiveHReset = countdown
                        }
                    }
                } else {
                    weeklyPct = g.optDouble("weekly_limit_remaining_pct", 100.0)
                    weeklyReset = g.optString("weekly_reset_countdown", "")
                    fiveHPct = g.optDouble("five_hour_limit_remaining_pct", 100.0)
                    fiveHReset = g.optString("five_hour_reset_countdown", "")
                }

                groupsList.add(
                    QuotaGroup(
                        name = gName,
                        description = gDesc,
                        weeklyLimitPct = weeklyPct,
                        weeklyResetCountdown = weeklyReset,
                        fiveHourLimitPct = fiveHPct,
                        fiveHourResetCountdown = fiveHReset
                    )
                )
            }

            val modelsArr = it.optJSONArray("models") ?: JSONArray()
            val modelsList = mutableListOf<ModelCapacity>()
            for (mIdx in 0 until modelsArr.length()) {
                val m = modelsArr.getJSONObject(mIdx)
                val mName = m.optString("model_name", m.optString("model_id", "Model $mIdx"))
                val mId = m.optString("model_id", "")
                val remFrac = m.optDouble("remaining_fraction", 1.0)
                val resetIso = m.optString("reset_time_iso", "")
                val remSec = m.optLong("reset_time_remaining_seconds", 0L)
                val countdown = formatResetDuration(remSec, "", resetIso, remFrac)
                modelsList.add(
                    ModelCapacity(
                        modelId = mId,
                        displayName = mName,
                        remainingPct = remFrac * 100.0,
                        resetCountdown = countdown
                    )
                )
            }

            val credArr = it.optJSONArray("available_credits")
            var totalCredits = 0L
            if (credArr != null && credArr.length() > 0) {
                for (cIdx in 0 until credArr.length()) {
                    totalCredits += credArr.getJSONObject(cIdx).optLong("credit_amount", 0)
                }
            } else {
                totalCredits = it.optLong("available_credits", 0)
            }
            if (totalCredits == 0L) {
                val flow = it.optLong("available_flow_credits", it.optLong("monthly_flow_credits", 0))
                val prompt = it.optLong("available_prompt_credits", it.optLong("monthly_prompt_credits", 0))
                totalCredits = flow + prompt
            }

            AntigravityStatus(
                availableCredits = totalCredits,
                planName = it.optString("plan_tier", it.optString("plan_name", "Google AI Ultra")),
                modelGroups = groupsList,
                individualModels = modelsList
            )
        }

        val cursor = curJson?.let {
            val spendArr = it.optJSONArray("spend_by_category") ?: it.optJSONArray("spend_by_model") ?: JSONArray()
            val spendList = mutableListOf<CursorModelSpend>()
            for (i in 0 until spendArr.length()) {
                val s = spendArr.getJSONObject(i)
                spendList.add(
                    CursorModelSpend(
                        modelName = s.optString("category", s.optString("model_name", "Model $i")),
                        spendAmount = s.optDouble("spend_usd", s.optDouble("spend_amount", 0.0)),
                        percentOfTotal = s.optDouble("percentage", s.optDouble("percent_of_total", 0.0))
                    )
                )
            }

            val used = it.optLong("fast_requests_used", 0)
            val limit = it.optLong("fast_requests_limit", 1)
            val pctUsed = if (limit > 0) (used.toDouble() / limit.toDouble() * 100.0) else it.optDouble("percent_used", 0.0)
            val totalSpend = it.optDouble("total_spend_usd", it.optDouble("total_spend", 0.0))
            val cycleReset = it.optString("cycle_reset_iso", it.optString("billing_cycle_reset", ""))

            CursorStatus(
                fastRequestsUsed = used,
                fastRequestsLimit = limit,
                percentUsed = pctUsed,
                billingCycleReset = cycleReset,
                daysRemaining = it.optInt("days_remaining", 0),
                totalSpend = totalSpend,
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

    private fun formatResetDuration(
        remainingSeconds: Long,
        description: String,
        resetIso: String,
        remainingFraction: Double
    ): String {
        if (remainingFraction >= 0.999 && remainingSeconds <= 0) {
            return "Active"
        }
        if (remainingSeconds > 0) {
            if (remainingSeconds > 8 * 86400) {
                if (description.contains("refresh in ")) {
                    return description.substringAfter("refresh in ").trimEnd('.')
                }
            }
            val days = remainingSeconds / 86400
            val hours = (remainingSeconds % 86400) / 3600
            val mins = (remainingSeconds % 3600) / 60
            return when {
                days > 0 -> "${days}d ${hours}h ${mins}m"
                hours > 0 -> "${hours}h ${mins}m"
                mins > 0 -> "${mins}m"
                else -> "<1m"
            }
        }
        if (description.contains("refresh in ")) {
            return description.substringAfter("refresh in ").trimEnd('.')
        }
        return if (remainingFraction >= 0.999) "Active" else "Ready"
    }
}
