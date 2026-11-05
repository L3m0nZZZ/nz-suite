package com.shoubo.tv

import android.app.AlarmManager
import android.app.Application
import android.app.PendingIntent
import android.content.Intent
import android.os.Build
import com.shoubo.tv.util.Logs
import com.shoubo.tv.util.Stats

/**
 * 崩溃自启（文档 4.5）：
 * 全局异常捕获 → 写日志 → 3秒后经 AlarmManager 自动拉起 MainActivity →
 * 新进程启动后按配置恢复崩溃前频道。进程死亡不影响已注册的系统闹钟。
 */
class App : Application() {

    override fun onCreate() {
        super.onCreate()
        Stats.init(this)
        Logs.init(this)
        installCrashHandler()
    }

    private fun installCrashHandler() {
        val defaultHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { t, e ->
            try {
                Logs.event("崩溃", "进程异常退出：" + (e.message ?: e.javaClass.name))
                val intent = Intent(this, MainActivity::class.java)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
                    .putExtra(MainActivity.EXTRA_RESTART_REASON, "crash")
                val pi = PendingIntent.getActivity(
                    this, 1001, intent,
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
                )
                val am = getSystemService(ALARM_SERVICE) as AlarmManager
                val triggerAt = System.currentTimeMillis() + CRASH_RESTART_DELAY_MS
                // setAndAllowWhileIdle 需要 API 23+，老盒子走 setExact
                if (Build.VERSION.SDK_INT >= 23) {
                    am.setAndAllowWhileIdle(AlarmManager.RTC, triggerAt, pi)
                } else {
                    am.setExact(AlarmManager.RTC, triggerAt, pi)
                }
            } catch (_: Exception) {
            }
            defaultHandler?.uncaughtException(t, e)
        }
    }

    companion object {
        private const val CRASH_RESTART_DELAY_MS = 3000L
    }
}
