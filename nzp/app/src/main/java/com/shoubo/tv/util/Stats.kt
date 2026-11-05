package com.shoubo.tv.util

import android.content.Context
import android.content.SharedPreferences
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** 运行统计（文档 4.6-4）：今日断线/卡死次数、本次连续运行时长，按天自动归零 */
object Stats {

    private lateinit var prefs: SharedPreferences
    private var cachedDate = ""

    fun init(context: Context) {
        prefs = context.getSharedPreferences("stats", Context.MODE_PRIVATE)
        ensureDate()
    }

    private fun today() = SimpleDateFormat("yyyy-MM-dd", Locale.CHINA).format(Date())

    private fun ensureDate() {
        val d = today()
        if (d == cachedDate) return
        cachedDate = d
        prefs.edit()
            .putString("date", d)
            .putInt("disconnects", 0)
            .putInt("stalls", 0)
            .putLong("run_start", System.currentTimeMillis())
            .apply()
    }

    fun incDisconnect() {
        ensureDate()
        prefs.edit().putInt("disconnects", getDisconnects() + 1).apply()
    }

    fun incStall() {
        ensureDate()
        prefs.edit().putInt("stalls", getStalls() + 1).apply()
    }

    fun getDisconnects(): Int = if (Stats::prefs.isInitialized) prefs.getInt("disconnects", 0) else 0

    fun getStalls(): Int = if (Stats::prefs.isInitialized) prefs.getInt("stalls", 0) else 0

    fun runDurationText(): String {
        ensureDate()
        val sec = (System.currentTimeMillis() - prefs.getLong("run_start", System.currentTimeMillis())) / 1000
        return String.format(Locale.CHINA, "%02d:%02d:%02d", sec / 3600, sec % 3600 / 60, sec % 60)
    }
}
