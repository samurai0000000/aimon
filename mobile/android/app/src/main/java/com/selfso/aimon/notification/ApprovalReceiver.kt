/*
 * ApprovalReceiver.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.notification

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.selfso.aimon.AimonApplication
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class ApprovalReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val approvalId = intent.getStringExtra(EXTRA_APPROVAL_ID) ?: return
        val action = intent.action ?: return

        val repository = AimonApplication.instance.repository
        val notificationManager = AimonNotificationManager(context)

        notificationManager.dismissApprovalNotification(approvalId)

        val decision = when (action) {
            ACTION_APPROVE -> "allow"
            ACTION_DENY -> "deny"
            else -> return
        }

        CoroutineScope(Dispatchers.IO).launch {
            repository.submitDecision(approvalId, decision)
        }
    }

    companion object {
        const val ACTION_APPROVE = "com.selfso.aimon.ACTION_APPROVE"
        const val ACTION_DENY = "com.selfso.aimon.ACTION_DENY"
        const val EXTRA_APPROVAL_ID = "EXTRA_APPROVAL_ID"
    }
}
