package com.rohan.ludordclient

import android.content.Context

object ModConfig {
    var redAlwaysWins: Boolean = false
    var forceDice: Int = 0
    var aiDumbMode: Boolean = false
    var speedMultiplier: Int = 1

    fun saveToPrefs(context: Context) {
        @Suppress("DEPRECATION")
        val prefs = context.getSharedPreferences("ludo_mods", Context.MODE_WORLD_READABLE)
        prefs.edit().apply {
            putBoolean("red_always_wins", redAlwaysWins)
            putInt("force_dice", forceDice)
            putBoolean("ai_dumb", aiDumbMode)
            putInt("speed_multiplier", speedMultiplier)
        }.apply()
    }

    fun loadFromPrefs(context: Context) {
        val prefs = context.getSharedPreferences("ludo_mods", Context.MODE_PRIVATE)
        redAlwaysWins = prefs.getBoolean("red_always_wins", false)
        forceDice = prefs.getInt("force_dice", 0)
        aiDumbMode = prefs.getBoolean("ai_dumb", false)
        speedMultiplier = prefs.getInt("speed_multiplier", 1)
    }
}
