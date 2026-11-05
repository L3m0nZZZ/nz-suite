package com.shoubo.tv.util

import android.content.Context
import android.os.Environment
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 本地日志（文档 4.6-3 / 附录B）：
 * 每条记录 = 时间 | 事件类型 | 详情，按天分文件，只保留最近7天。
 * 优先写 /sdcard/守播/logs/，无权限时退到应用专属目录。
 */
object Logs {

    private const val RETENTION_DAYS = 7L
    private const val MAX_LOG_BYTES = 2L * 1024 * 1024

    private var dir: File? = null
    private val ts = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.CHINA)
    private val day = SimpleDateFormat("yyyy-MM-dd", Locale.CHINA)

    fun init(context: Context) {
        val primary = File(Environment.getExternalStorageDirectory(), "守播/logs")
        val fallback = File(context.getExternalFilesDir(null) ?: context.filesDir, "logs")
        dir = if (primary.isDirectory || primary.mkdirs()) primary else fallback
        cleanOld()
        event("运行", "日志初始化完成，目录：${dir?.absolutePath}")
    }

    @Synchronized
    fun event(type: String, detail: String) {
        val d = dir ?: return
        try {
            val f = File(d, day.format(Date()) + ".log")
            if (f.length() > MAX_LOG_BYTES) {
                val old = File(d, "previous.log")
                old.delete()
                f.renameTo(old)
            }
            f.appendText("${ts.format(Date())} | $type | $detail\n")
        } catch (_: Exception) {
        }
    }

    private fun cleanOld() {
        val d = dir ?: return
        val expire = System.currentTimeMillis() - RETENTION_DAYS * 24 * 3600 * 1000
        d.listFiles { file -> file.isFile && file.name.endsWith(".log") }?.forEach {
            if (it.lastModified() < expire) it.delete()
        }
    }
}
