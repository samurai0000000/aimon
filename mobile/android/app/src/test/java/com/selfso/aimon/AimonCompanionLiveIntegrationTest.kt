/*
 * AimonCompanionLiveIntegrationTest.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon

import com.selfso.aimon.data.AimonRepository
import com.selfso.aimon.data.PairingManager
import kotlinx.coroutines.runBlocking
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class AimonCompanionLiveIntegrationTest {

    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .build()

    @Test
    fun testFullLiveDaemonCompanionLifecycle() = runBlocking {
        // 1. Fetch real QR code / pairing secret from live running aimon daemon
        val qrReq = Request.Builder()
            .url("http://127.0.0.1:3883/api/mobile/qr")
            .get()
            .build()

        val qrResp = httpClient.newCall(qrReq).execute()
        assertTrue("Daemon /api/mobile/qr should return 200 OK", qrResp.isSuccessful)
        val qrBody = qrResp.body?.string() ?: ""
        val qrJson = JSONObject(qrBody)
        val secret = qrJson.optString("secret", "")
        assertTrue("Secret should be non-empty 128-bit hex", secret.length == 32)

        // 2. Initialize Android PairingManager & AimonRepository in test mode
        val pairingManager = PairingManager(context = null)
        val repository = AimonRepository(context = null, pairingManager = pairingManager)

        // 3. Test Pairing Flow
        val pairResult = repository.pairWithDaemon("127.0.0.1", 3883, secret)
        assertTrue("pairWithDaemon should succeed: ${pairResult.exceptionOrNull()?.message}", pairResult.isSuccess)
        val token = pairResult.getOrNull()
        assertNotNull(token)
        assertTrue("Token should be 256-bit hex", token!!.length == 64)
        assertTrue("PairingManager should be paired", pairingManager.config.value.isPaired)

        // 4. Test Quota Balance Fetching and Parsing
        val quotaResult = repository.refreshQuotas()
        assertTrue("refreshQuotas should succeed: ${quotaResult.exceptionOrNull()?.message}", quotaResult.isSuccess)
        val quota = quotaResult.getOrNull()
        assertNotNull(quota)

        // Validate Antigravity Quotas
        val ag = quota?.antigravity
        assertNotNull("Antigravity quotas must not be null", ag)
        assertEquals("Google AI Ultra", ag?.planName)
        assertTrue("Available credits must be positive", ag?.availableCredits ?: 0 > 0)
        assertTrue("Model groups must not be empty", ag?.modelGroups?.isNotEmpty() == true)

        val geminiGroup = ag?.modelGroups?.find { it.name.contains("Gemini") }
        assertNotNull("Gemini quota group should exist", geminiGroup)
        assertTrue("Weekly limit percentage must be > 0", (geminiGroup?.weeklyLimitPct ?: 0.0) > 0.0)
        assertTrue("5H limit percentage must be > 0", (geminiGroup?.fiveHourLimitPct ?: 0.0) > 0.0)

        // Validate Cursor Quotas
        val cursor = quota?.cursor
        assertNotNull("Cursor quotas must not be null", cursor)
        assertTrue("Fast requests used should be > 0", (cursor?.fastRequestsUsed ?: 0) > 0)
        assertTrue("Fast requests limit should be > 0", (cursor?.fastRequestsLimit ?: 0) > 0)
        assertTrue("Total spend should be positive", (cursor?.totalSpend ?: 0.0) > 0.0)
        assertTrue("Spend by model categories must not be empty", cursor?.spendByModel?.isNotEmpty() == true)

        // 5. Test Live Action Approval and Decision Resolution
        val executor = Executors.newSingleThreadExecutor()
        val approvalPayload = JSONObject().apply {
            put("agent_type", "antigravity")
            put("tool_name", "run_command")
            put("workspace", "~/work/aimon")
            put("tool_args", JSONObject().apply { put("CommandLine", "make test") })
            put("reason", "Automated Android Companion QA Test")
            put("timeout_seconds", 30)
        }.toString()

        val approvalFuture = executor.submit<String> {
            val req = Request.Builder()
                .url("http://127.0.0.1:3883/api/approvals/request")
                .post(approvalPayload.toRequestBody("application/json".toMediaType()))
                .build()
            val resp = httpClient.newCall(req).execute()
            resp.body?.string() ?: ""
        }

        // Allow daemon 100ms to register approval in latch
        kotlinx.coroutines.delay(100)

        // Refresh approvals in repository
        val approvalsResult = repository.refreshApprovals()
        assertTrue("refreshApprovals should succeed", approvalsResult.isSuccess)
        val pendingList = approvalsResult.getOrNull() ?: emptyList()
        assertTrue("Pending approvals list should contain at least 1 item", pendingList.isNotEmpty())

        val testApproval = pendingList.first()
        assertEquals("run_command", testApproval.toolName)
        assertEquals("antigravity", testApproval.agentType)

        // Submit Approval decision from Android Repository
        val decisionResult = repository.submitDecision(testApproval.approvalId, "allow")
        assertTrue("submitDecision should succeed", decisionResult.isSuccess)

        // Wait for daemon response
        val daemonResponseStr = approvalFuture.get(5, TimeUnit.SECONDS)
        val daemonRespJson = JSONObject(daemonResponseStr)
        assertEquals("APPROVED", daemonRespJson.optString("verdict"))

        // Verify pending approvals is now empty
        val updatedApprovals = repository.refreshApprovals().getOrNull() ?: emptyList()
        assertTrue("Pending list should now be empty", updatedApprovals.isEmpty())

        // 6. Test Telemetry Overview
        val telemResult = repository.refreshTelemetry()
        assertTrue("refreshTelemetry should succeed: ${telemResult.exceptionOrNull()?.message}", telemResult.isSuccess)
        assertNotNull(telemResult.getOrNull())

        // 7. Cleanup & Revoke
        val deviceId = pairingManager.getDeviceId()
        val revokeReq = Request.Builder()
            .url("http://127.0.0.1:3883/api/mobile/revoke")
            .post(JSONObject().apply { put("device_id", deviceId) }.toString().toRequestBody("application/json".toMediaType()))
            .build()
        val revokeResp = httpClient.newCall(revokeReq).execute()
        assertTrue("Revoke should succeed", revokeResp.isSuccessful)

        pairingManager.clearPairing()
        assertFalse("PairingManager should now be unpaired", pairingManager.config.value.isPaired)
    }
}
