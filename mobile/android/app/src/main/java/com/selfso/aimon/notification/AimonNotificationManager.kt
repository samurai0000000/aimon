/*
 * AimonNotificationManager.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.notification

import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import androidx.core.app.NotificationCompat
import com.selfso.aimon.AimonApplication
import com.selfso.aimon.MainActivity
import com.selfso.aimon.data.ApprovalRequest

class AimonNotificationManager(private val context: Context) {

    private val notificationManager =
        context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    fun showApprovalNotification(approval: ApprovalRequest) {
        val tapIntent = Intent(context, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_CLEAR_TOP
            putExtra("EXTRA_APPROVAL_ID", approval.approvalId)
        }
        val tapPendingIntent = PendingIntent.getActivity(
            context,
            approval.approvalId.hashCode(),
            tapIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        // Action [Approve] Intent
        val approveIntent = Intent(context, ApprovalReceiver::class.java).apply {
            action = ApprovalReceiver.ACTION_APPROVE
            putExtra(ApprovalReceiver.EXTRA_APPROVAL_ID, approval.approvalId)
        }
        val approvePendingIntent = PendingIntent.getBroadcast(
            context,
            approval.approvalId.hashCode(),
            approveIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        // Action [Deny] Intent
        val denyIntent = Intent(context, ApprovalReceiver::class.java).apply {
            action = ApprovalReceiver.ACTION_DENY
            putExtra(ApprovalReceiver.EXTRA_APPROVAL_ID, approval.approvalId)
        }
        val denyPendingIntent = PendingIntent.getBroadcast(
            context,
            approval.approvalId.hashCode() + 1,
            denyIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val title = "[${approval.agentType.uppercase()}] Action Approval Required"
        val body = buildString {
            append("Tool: ").append(approval.toolName).append("\n")
            if (approval.reason.isNotEmpty()) {
                append("Reason: ").append(approval.reason).append("\n")
            }
            if (approval.workspace.isNotEmpty()) {
                append("Workspace: ").append(approval.workspace).append("\n")
            }
            if (approval.toolArgs.isNotEmpty()) {
                append("Args: ").append(approval.toolArgs.entries.joinToString(", ") { "${it.key}=${it.value}" })
            }
        }

        val notification = NotificationCompat.Builder(context, AimonApplication.CHANNEL_APPROVALS)
            .setSmallIcon(android.R.drawable.ic_dialog_alert)
            .setContentTitle(title)
            .setContentText("${approval.toolName}: ${approval.reason.ifEmpty { "Confirm tool execution" }}")
            .setStyle(NotificationCompat.BigTextStyle().bigText(body))
            .setPriority(NotificationCompat.PRIORITY_MAX)
            .setCategory(NotificationCompat.CATEGORY_ALARM)
            .setContentIntent(tapPendingIntent)
            .setAutoCancel(true)
            .addAction(android.R.drawable.checkbox_on_background, "Approve", approvePendingIntent)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Deny", denyPendingIntent)
            .build()

        notificationManager.notify(approval.approvalId.hashCode(), notification)
    }

    fun dismissApprovalNotification(approvalId: String) {
        notificationManager.cancel(approvalId.hashCode())
    }
}
