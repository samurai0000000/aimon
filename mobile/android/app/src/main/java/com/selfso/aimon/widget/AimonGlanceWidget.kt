/*
 * AimonGlanceWidget.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.widget

import android.content.Context
import androidx.compose.runtime.Composable
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.glance.GlanceId
import androidx.glance.GlanceModifier
import androidx.glance.GlanceTheme
import androidx.glance.appwidget.GlanceAppWidget
import androidx.glance.appwidget.GlanceAppWidgetReceiver
import androidx.glance.appwidget.cornerRadius
import androidx.glance.appwidget.provideContent
import androidx.glance.background
import androidx.glance.layout.*
import androidx.glance.text.FontWeight
import androidx.glance.text.Text
import androidx.glance.text.TextStyle
import com.selfso.aimon.ui.theme.CyanAccent
import com.selfso.aimon.ui.theme.DarkBackground
import com.selfso.aimon.ui.theme.DarkSurface
import com.selfso.aimon.ui.theme.TextPrimary
import com.selfso.aimon.ui.theme.TextSecondary

class AimonGlanceWidget : GlanceAppWidget() {

    override suspend fun provideGlance(context: Context, id: GlanceId) {
        provideContent {
            GlanceTheme {
                WidgetContent()
            }
        }
    }

    @Composable
    private fun WidgetContent() {
        Column(
            modifier = GlanceModifier
                .fillMaxSize()
                .background(DarkBackground)
                .cornerRadius(16.dp)
                .padding(12.dp)
        ) {
            Row(
                modifier = GlanceModifier.fillMaxWidth(),
                horizontalAlignment = Alignment.Horizontal.Start,
                verticalAlignment = Alignment.Vertical.CenterVertically
            ) {
                Text(
                    text = "aimon",
                    style = TextStyle(
                        color = androidx.glance.unit.ColorProvider(CyanAccent),
                        fontSize = 14.sp,
                        fontWeight = FontWeight.Bold
                    )
                )
            }

            Spacer(modifier = GlanceModifier.height(8.dp))

            Column(
                modifier = GlanceModifier
                    .fillMaxWidth()
                    .background(DarkSurface)
                    .cornerRadius(8.dp)
                    .padding(8.dp)
            ) {
                Text(
                    text = "Google AI Ultra Credits",
                    style = TextStyle(
                        color = androidx.glance.unit.ColorProvider(TextSecondary),
                        fontSize = 11.sp
                    )
                )
                Text(
                    text = "3,024 Available",
                    style = TextStyle(
                        color = androidx.glance.unit.ColorProvider(TextPrimary),
                        fontSize = 13.sp,
                        fontWeight = FontWeight.Bold
                    )
                )
            }
        }
    }
}

class AimonGlanceWidgetReceiver : GlanceAppWidgetReceiver() {
    override val glanceAppWidget: GlanceAppWidget = AimonGlanceWidget()
}
