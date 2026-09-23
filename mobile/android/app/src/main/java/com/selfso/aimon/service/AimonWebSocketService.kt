/*
 * AimonWebSocketService.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.service

import android.app.Service
import android.content.Intent
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.selfso.aimon.AimonApplication
import com.selfso.aimon.data.ApprovalRequest
import com.selfso.aimon.notification.AimonNotificationManager
import kotlinx.coroutines.*
import okhttp3.*
import org.json.JSONObject
import java.util.concurrent.TimeUnit

class AimonWebSocketService : Service() {

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var webSocket: WebSocket? = null
    private var isRunning = false

    private val httpClient = OkHttpClient.Builder()
        .pingInterval(15, TimeUnit.SECONDS)
        .build()

    private lateinit var notificationManager: AimonNotificationManager

    override fun onCreate() {
        super.onCreate()
        notificationManager = AimonNotificationManager(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = NotificationCompat.Builder(this, AimonApplication.CHANNEL_SERVICE)
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setContentTitle("aimon Mobile Gateway")
            .setContentText("Connected to aimon daemon")
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()

        startForeground(1001, notification)

        if (!isRunning) {
            isRunning = true
            connectEventStream()
        }

        return START_STICKY
    }

    private fun connectEventStream() {
        serviceScope.launch {
            val repository = AimonApplication.instance.repository
            val pairingManager = AimonApplication.instance.pairingManager

            while (isRunning) {
                val config = pairingManager.config.value
                if (!config.isPaired || config.token.isEmpty()) {
                    repository.setConnected(false)
                    delay(3000)
                    continue
                }

                val cleanHost = config.host.trim().removePrefix("http://").removePrefix("https://").removeSuffix("/")
                val sseUrl = "http://$cleanHost:${config.port}/sse?profile=mobile"
                val request = Request.Builder()
                    .url(sseUrl)
                    .addHeader("Accept", "text/event-stream")
                    .addHeader("Authorization", "Bearer ${config.token}")
                    .build()

                try {
                    val sseClient = httpClient.newBuilder()
                        .readTimeout(0, TimeUnit.MILLISECONDS)
                        .build()

                    val response = sseClient.newCall(request).execute()
                    if (response.isSuccessful) {
                        repository.setConnected(true)
                        serviceScope.launch {
                            repository.refreshQuotas()
                            repository.refreshApprovals()
                            repository.refreshTelemetry()
                        }

                        val source = response.body?.source()
                        if (source != null) {
                            while (isRunning && !source.exhausted()) {
                                val line = source.readUtf8Line() ?: break
                                if (line.startsWith("data: ")) {
                                    val data = line.removePrefix("data: ").trim()
                                    if (data.startsWith("{")) {
                                        handleMessage(data)
                                    }
                                }
                            }
                        }
                    }
                } catch (_: Exception) {
                } finally {
                    repository.setConnected(false)
                }

                delay(3000)
            }
        }
    }

    private fun handleMessage(text: String) {
        try {
            val json = JSONObject(text)
            val type = json.optString("type")
            val repository = AimonApplication.instance.repository

            when (type) {
                "approval_request" -> {
                    val reqJson = json.optJSONObject("payload") ?: return
                    val argsMap = mutableMapOf<String, Any?>()
                    reqJson.optJSONObject("tool_args")?.let { argsObj ->
                        val keys = argsObj.keys()
                        while (keys.hasNext()) {
                            val k = keys.next()
                            argsMap[k] = argsObj.opt(k)
                        }
                    }
                    val approval = ApprovalRequest(
                        approvalId = reqJson.optString("approval_id"),
                        agentType = reqJson.optString("agent_type", "antigravity"),
                        toolName = reqJson.optString("tool_name", ""),
                        workspace = reqJson.optString("workspace", ""),
                        toolArgs = argsMap,
                        reason = reqJson.optString("reason", ""),
                        requestedAt = reqJson.optLong("requested_at", System.currentTimeMillis() / 1000),
                        timeoutSeconds = reqJson.optInt("timeout_seconds", 120),
                        remainingSeconds = reqJson.optInt("remaining_seconds", reqJson.optInt("timeout_seconds", 120))
                    )
                    repository.handleIncomingApproval(approval)
                    notificationManager.showApprovalNotification(approval)
                }
                "approval_resolved" -> {
                    val reqJson = json.optJSONObject("payload") ?: return
                    val approvalId = reqJson.optString("approval_id")
                    repository.removeApproval(approvalId)
                    notificationManager.dismissApprovalNotification(approvalId)
                }
                "quota_update" -> {
                    serviceScope.launch { repository.refreshQuotas() }
                }
                "telemetry_sample" -> {
                    serviceScope.launch { repository.refreshTelemetry() }
                }
            }
        } catch (_: Exception) {}
    }

    override fun onDestroy() {
        super.onDestroy()
        isRunning = false
        webSocket?.close(1000, "Service destroyed")
        serviceScope.cancel()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
