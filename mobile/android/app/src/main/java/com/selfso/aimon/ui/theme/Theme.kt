/*
 * Theme.kt
 *
 * Copyright (C) 2026, Charles Chiou
 */

package com.selfso.aimon.ui.theme

import androidx.compose.animation.core.*
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

// --- Antimatter Cybernetic Palette ---
val DarkBackground = Color(0xFF060813)
val DarkBackgroundGradientEnd = Color(0xFF0B1124)
val DarkSurface = Color(0xFF0F172A)
val DarkSurfaceElevated = Color(0xFF131D36)
val GlassSurface = Color(0xDD0D1527)
val GlassBorder = Color(0x3338BDF8)
val GlassBorderPurple = Color(0x33A855F7)

// Neon & Quantum Accents
val CyanAccent = Color(0xFF00F2FE)
val CyanAccentMuted = Color(0xFF38BDF8)
val MagentaAccent = Color(0xFFEC4899)
val PurpleAccent = Color(0xFFA855F7)
val IndigoAccent = Color(0xFF6366F1)
val AmberAccent = Color(0xFFF59E0B)
val GreenAccent = Color(0xFF10B981)
val RedAccent = Color(0xFFEF4444)
val TealAccent = Color(0xFF14B8A6)

// Text
val TextPrimary = Color(0xFFF8FAFC)
val TextSecondary = Color(0xFF94A3B8)
val TextMuted = Color(0xFF64748B)

// Gradients
val AntimatterGradient = Brush.linearGradient(
    colors = listOf(CyanAccent, PurpleAccent)
)

val QuantumPinkGradient = Brush.linearGradient(
    colors = listOf(MagentaAccent, PurpleAccent)
)

val EmeraldGradient = Brush.linearGradient(
    colors = listOf(GreenAccent, TealAccent)
)

val AmberGradient = Brush.linearGradient(
    colors = listOf(AmberAccent, RedAccent)
)

val VoidCardBorder = Brush.linearGradient(
    colors = listOf(Color(0x6600F2FE), Color(0x22A855F7), Color(0x116366F1))
)

private val DarkColorScheme = darkColorScheme(
    primary = CyanAccent,
    secondary = MagentaAccent,
    tertiary = PurpleAccent,
    background = DarkBackground,
    surface = DarkSurface,
    surfaceVariant = DarkSurfaceElevated,
    onPrimary = Color.Black,
    onSecondary = Color.Black,
    onBackground = TextPrimary,
    onSurface = TextPrimary
)

@Composable
fun AimonTheme(
    content: @Composable () -> Unit
) {
    MaterialTheme(
        colorScheme = DarkColorScheme,
        content = content
    )
}

/**
 * Cybernetic Glassmorphic Card Container with subtle gradient border & dark translucent background
 */
@Composable
fun AntimatterCard(
    modifier: Modifier = Modifier,
    shape: Shape = RoundedCornerShape(16.dp),
    borderColor: Color = GlassBorder,
    backgroundColor: Color = GlassSurface,
    content: @Composable ColumnScope.() -> Unit
) {
    Card(
        modifier = modifier
            .border(
                BorderStroke(1.dp, Brush.linearGradient(listOf(borderColor, borderColor.copy(alpha = 0.1f)))),
                shape
            ),
        shape = shape,
        colors = CardDefaults.cardColors(containerColor = backgroundColor),
        elevation = CardDefaults.cardElevation(defaultElevation = 0.dp)
    ) {
        Column(
            modifier = Modifier.padding(16.dp),
            content = content
        )
    }
}

/**
 * Animated Pulsing Live Status Dot (e.g. Online, Agent Active)
 */
@Composable
fun PulsingStatusDot(
    color: Color = GreenAccent,
    size: Dp = 8.dp,
    modifier: Modifier = Modifier
) {
    val infiniteTransition = rememberInfiniteTransition(label = "pulse")
    val alpha by infiniteTransition.animateFloat(
        initialValue = 0.3f,
        targetValue = 1.0f,
        animationSpec = infiniteRepeatable(
            animation = tween(1000, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse
        ),
        label = "pulseAlpha"
    )

    Box(
        modifier = modifier.size(size * 2),
        contentAlignment = Alignment.Center
    ) {
        Box(
            modifier = Modifier
                .size(size * 1.8f)
                .clip(CircleShape)
                .background(color.copy(alpha = alpha * 0.4f))
        )
        Box(
            modifier = Modifier
                .size(size)
                .clip(CircleShape)
                .background(color)
        )
    }
}

/**
 * High-tech Neon Gradient Progress Bar
 */
@Composable
fun NeonProgressBar(
    progress: Float,
    modifier: Modifier = Modifier,
    fillBrush: Brush = AntimatterGradient,
    trackColor: Color = Color.White.copy(alpha = 0.08f),
    height: Dp = 8.dp
) {
    val clampedProgress = progress.coerceIn(0f, 1f)
    Box(
        modifier = modifier
            .fillMaxWidth()
            .height(height)
            .clip(RoundedCornerShape(height / 2))
            .background(trackColor)
    ) {
        Box(
            modifier = Modifier
                .fillMaxHeight()
                .fillMaxWidth(clampedProgress)
                .clip(RoundedCornerShape(height / 2))
                .background(fillBrush)
        )
    }
}

/**
 * High-tech Glow Pill Badge
 */
@Composable
fun QuantumChip(
    text: String,
    accentColor: Color = CyanAccent,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(6.dp))
            .background(accentColor.copy(alpha = 0.15f))
            .border(1.dp, accentColor.copy(alpha = 0.4f), RoundedCornerShape(6.dp))
            .padding(horizontal = 8.dp, vertical = 3.dp)
    ) {
        Text(
            text = text,
            color = accentColor,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace
        )
    }
}
