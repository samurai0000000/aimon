/*
 * AimonServerIntegrationTest.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon

import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit

class AimonServerIntegrationTest {

    private val httpClient = OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .build()

    private val baseUrl = "http://127.0.0.1:3883"
    private var authToken: String = ""
    private val testDeviceId = "android-junit-test-runner"

    @Before
    fun setUp() {
        // 1. Fetch pairing secret
        val qrReq = Request.Builder().url("${baseUrl}/api/mobile/qr").get().build()
        val qrResp = httpClient.newCall(qrReq).execute()
        assertTrue("GET /api/mobile/qr should succeed", qrResp.isSuccessful)
        val qrJson = JSONObject(qrResp.body?.string() ?: "{}")
        val secret = qrJson.optString("secret")
        assertTrue("Pairing secret must not be empty", secret.isNotEmpty())

        // 2. Pair test device
        val pairBody = JSONObject().apply {
            put("secret", secret)
            put("device_id", testDeviceId)
            put("device_name", "JUnit Integration Client")
        }.toString()

        val pairReq = Request.Builder()
            .url("${baseUrl}/api/mobile/pair")
            .post(pairBody.toRequestBody("application/json".toMediaType()))
            .build()
        val pairResp = httpClient.newCall(pairReq).execute()
        assertTrue("POST /api/mobile/pair should succeed", pairResp.isSuccessful)
        val pairJson = JSONObject(pairResp.body?.string() ?: "{}")
        assertEquals("status should be paired", "paired", pairJson.optString("status"))
        authToken = pairJson.optString("token")
        assertTrue("Token must not be empty", authToken.isNotEmpty())
    }

    @Test
    fun testLiveQuotaBalancesParsing() {
        val req = Request.Builder()
            .url("${baseUrl}/api/status")
            .addHeader("Authorization", "Bearer $authToken")
            .get()
            .build()

        val resp = httpClient.newCall(req).execute()
        assertTrue("GET /api/status should return HTTP 200", resp.isSuccessful)
        val jsonStr = resp.body?.string() ?: ""
        val json = JSONObject(jsonStr)

        val agJson = json.optJSONObject("antigravity")
        assertNotNull("Antigravity JSON must be present", agJson)
        val planTier = agJson?.optString("plan_tier")
        assertEquals("Google AI Ultra", planTier)

        // Parse available credits
        val credArr = agJson?.optJSONArray("available_credits")
        var totalCredits = 0L
        if (credArr != null && credArr.length() > 0) {
            for (i in 0 until credArr.length()) {
                totalCredits += credArr.getJSONObject(i).optLong("credit_amount", 0)
            }
        }
        if (totalCredits == 0L) {
            totalCredits = agJson?.optLong("monthly_flow_credits", 0)!! + agJson?.optLong("monthly_prompt_credits", 0)!!
        }
        assertTrue("Total Google One / AI credits must be > 0 (found $totalCredits)", totalCredits > 0)

        // Parse Quota Groups
        val groupsArr = agJson?.optJSONArray("quota_groups")
        assertNotNull("Quota groups array must exist", groupsArr)
        assertTrue("Must have at least 1 quota group", groupsArr!!.length() >= 1)

        val firstGroup = groupsArr.getJSONObject(0)
        val gName = firstGroup.optString("display_name")
        assertTrue("Quota group must have display_name", gName.isNotEmpty())
        val buckets = firstGroup.optJSONArray("buckets")
        assertNotNull("Group buckets must exist", buckets)
        assertTrue("Group must have buckets", buckets!!.length() > 0)

        // Parse Cursor JSON
        val curJson = json.optJSONObject("cursor")
        assertNotNull("Cursor JSON must be present", curJson)
        val fastUsed = curJson?.optLong("fast_requests_used", 0) ?: 0
        val fastLimit = curJson?.optLong("fast_requests_limit", 0) ?: 0
        val totalSpend = curJson?.optDouble("total_spend_usd", 0.0) ?: 0.0
        assertTrue("Fast requests limit must be > 0", fastLimit > 0)
        assertTrue("Total spend must be >= 0", totalSpend >= 0.0)

        val spendArr = curJson?.optJSONArray("spend_by_category")
        assertNotNull("spend_by_category array must exist", spendArr)
        assertTrue("Must have spend categories", spendArr!!.length() > 0)
    }

    @Test
    fun testLiveTelemetryOverview() {
        val req = Request.Builder()
            .url("${baseUrl}/api/telemetry/overview")
            .addHeader("Authorization", "Bearer $authToken")
            .get()
            .build()

        val resp = httpClient.newCall(req).execute()
        assertTrue("GET /api/telemetry/overview should return HTTP 200", resp.isSuccessful)
        val json = JSONObject(resp.body?.string() ?: "{}")
        assertTrue("JSON must contain active_sessions", json.has("active_sessions"))
        assertTrue("JSON must contain total_tool_calls", json.has("total_tool_calls"))
    }

    @Test
    fun testEndToEndActionApprovalWorkflow() {
        val executor = Executors.newSingleThreadExecutor()

        // 1. Submit approval request on agent side in background
        val futureVerdict: Future<String> = executor.submit<String> {
            val reqBody = JSONObject().apply {
                put("agent_type", "antigravity")
                put("tool_name", "test_e2e_approval_tool")
                put("workspace", "/tmp")
                put("tool_args", JSONObject().put("cmd", "echo Approved!"))
                put("reason", "Automated JUnit End-to-End Verification")
                put("timeout_seconds", 10)
            }.toString()

            val req = Request.Builder()
                .url("${baseUrl}/api/approvals/request")
                .post(reqBody.toRequestBody("application/json".toMediaType()))
                .build()

            val resp = httpClient.newCall(req).execute()
            val respJson = JSONObject(resp.body?.string() ?: "{}")
            respJson.optString("verdict")
        }

        // 2. Allow server to register pending request
        Thread.sleep(100)

        // 3. Query pending approvals as mobile client
        val pendingReq = Request.Builder()
            .url("${baseUrl}/api/approvals/pending")
            .addHeader("Authorization", "Bearer $authToken")
            .get()
            .build()

        val pendingResp = httpClient.newCall(pendingReq).execute()
        assertTrue("GET /api/approvals/pending should succeed", pendingResp.isSuccessful)
        val pendingArr = JSONArray(pendingResp.body?.string() ?: "[]")
        assertTrue("Pending list must contain at least 1 item", pendingArr.length() >= 1)

        var targetApprovalId = ""
        for (i in 0 until pendingArr.length()) {
            val item = pendingArr.getJSONObject(i)
            if (item.optString("tool_name") == "test_e2e_approval_tool") {
                targetApprovalId = item.optString("approval_id")
                break
            }
        }
        assertTrue("Must find target approval ID", targetApprovalId.isNotEmpty())

        // 4. Submit approval decision as mobile client
        val decisionBody = JSONObject().apply {
            put("approval_id", targetApprovalId)
            put("decision", "allow")
        }.toString()

        val decisionReq = Request.Builder()
            .url("${baseUrl}/api/approvals/decision")
            .addHeader("Authorization", "Bearer $authToken")
            .post(decisionBody.toRequestBody("application/json".toMediaType()))
            .build()

        val decisionResp = httpClient.newCall(decisionReq).execute()
        assertTrue("POST /api/approvals/decision should succeed", decisionResp.isSuccessful)
        val decisionJson = JSONObject(decisionResp.body?.string() ?: "{}")
        assertTrue("Decision resolved must be true", decisionJson.optBoolean("resolved"))

        // 5. Verify the waiting agent received APPROVED verdict
        val agentVerdict = futureVerdict.get(5, TimeUnit.SECONDS)
        assertEquals("Agent must receive APPROVED verdict", "APPROVED", agentVerdict)

        executor.shutdown()
    }
}
