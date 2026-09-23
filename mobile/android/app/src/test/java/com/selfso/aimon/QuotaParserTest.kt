/*
 * QuotaParserTest.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon

import com.selfso.aimon.data.*
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class QuotaParserTest {

    @Test
    fun testLiveQuotaJsonParsing() {
        val jsonStr = """
        {
          "antigravity": {
            "error_message": "",
            "is_authenticated": true,
            "last_updated_epoch": 1790178835,
            "models": [
              {
                "model_id": "claude-sonnet-4-6",
                "model_name": "Claude Sonnet 4.6 (Thinking)",
                "remaining_fraction": 1.0,
                "reset_time_iso": "2026-09-23T20:52:00Z"
              },
              {
                "model_id": "gemini-3.7-flash-high",
                "model_name": "Gemini 3.7 Flash (High)",
                "remaining_fraction": 0.8040264,
                "reset_time_iso": "2026-09-23T18:00:13Z"
              }
            ],
            "monthly_flow_credits": 150000,
            "monthly_prompt_credits": 50000,
            "plan_tier": "Google AI Ultra",
            "quota_groups": [
              {
                "buckets": [
                  {
                    "bucket_id": "gemini-weekly",
                    "description": "You have used some of your weekly limit, it will fully refresh in 3 days, 8 hours.",
                    "display_name": "Weekly Limit Remaining",
                    "remaining_fraction": 0.8340755,
                    "reset_time_iso": "2026-09-27T00:18:57Z",
                    "window": "weekly"
                  },
                  {
                    "bucket_id": "gemini-5h",
                    "description": "You have used some of your 5-hour limit, it will fully refresh in 2 hours, 11 minutes.",
                    "display_name": "Five Hour Limit Remaining",
                    "remaining_fraction": 0.8237638,
                    "reset_time_iso": "2026-09-23T18:00:13Z",
                    "window": "5h"
                  }
                ],
                "description": "Models within this group: Gemini Flash, Gemini Pro",
                "display_name": "Gemini Models"
              }
            ]
          },
          "cursor": {
            "cycle_reset_iso": "2026-10-01T00:00:00.000Z",
            "fast_requests_limit": 90000,
            "fast_requests_used": 90015,
            "is_authenticated": true,
            "on_demand_spend": 0.0,
            "plan_tier": "enterprise",
            "spend_by_category": [
              {
                "category": "claude-opus-5-thinking-high",
                "percentage": 24.190199104971516,
                "spend_usd": 217.84
              }
            ],
            "total_spend_usd": 900.53
          }
        }
        """.trimIndent()

        val json = JSONObject(jsonStr)
        assertNotNull(json)

        val agJson = json.optJSONObject("antigravity")
        assertNotNull(agJson)
        assertEquals("Google AI Ultra", agJson?.optString("plan_tier"))
        assertEquals(200000L, agJson?.optLong("monthly_flow_credits", 0)!! + agJson?.optLong("monthly_prompt_credits", 0)!!)

        val models = agJson?.optJSONArray("models")
        assertNotNull(models)
        assertEquals(2, models!!.length())
        assertEquals("Claude Sonnet 4.6 (Thinking)", models.getJSONObject(0).optString("model_name"))

        val curJson = json.optJSONObject("cursor")
        assertNotNull(curJson)
        assertEquals(90015L, curJson?.optLong("fast_requests_used"))
        assertEquals(90000L, curJson?.optLong("fast_requests_limit"))
        assertEquals(900.53, curJson?.optDouble("total_spend_usd") ?: 0.0, 0.001)

        val spendList = curJson?.optJSONArray("spend_by_category")
        assertNotNull(spendList)
        assertEquals(1, spendList!!.length())
        assertEquals("claude-opus-5-thinking-high", spendList.getJSONObject(0).optString("category"))
    }

    @Test
    fun testProgressIndicatorClamping() {
        val overflowPercentage = 100.016666
        val clampedProgress = (overflowPercentage / 100.0).toFloat().coerceIn(0f, 1f)
        assertEquals(1.0f, clampedProgress, 0.0001f)

        val negativePercentage = -5.0
        val clampedNegative = (negativePercentage / 100.0).toFloat().coerceIn(0f, 1f)
        assertEquals(0.0f, clampedNegative, 0.0001f)

        val normalPercentage = 83.4
        val clampedNormal = (normalPercentage / 100.0).toFloat().coerceIn(0f, 1f)
        assertEquals(0.834f, clampedNormal, 0.0001f)
    }
}
