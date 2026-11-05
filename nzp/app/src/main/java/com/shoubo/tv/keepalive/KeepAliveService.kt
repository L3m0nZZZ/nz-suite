package com.shoubo.tv.keepalive

import android.app.ActivityManager
import android.app.AlarmManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import com.shoubo.tv.MainActivity
import com.shoubo.tv.R
import com.shoubo.tv.player.PlayerManager
import com.shoubo.tv.util.Logs
import java.util.Calendar

/**
 * 保活与运维层（文档 4.5 / 4.6）：
 * - 前台服务 + 常驻通知：主保活手段；
 * - WakeLock + WifiLock：防 CPU/WiFi 休眠断流（老盒子常见坑）；
 * - 每日 04:00 定时自愈重建播放器；
 * - 每5分钟内存水位监控，超阈值重建播放器释放资源。
 */
class KeepAliveService : Service() {

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private val handler = Handler(Looper.getMainLooper())

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startForegroundWithNotification()
        acquireLocks()
        scheduleDailyRestart(this)
        handler.postDelayed(memoryCheck, MEMORY_CHECK_INTERVAL_MS)
        Logs.event("运行", "前台保活服务已启动")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int = START_STICKY

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        try {
            wakeLock?.release()
        } catch (_: Exception) {
        }
        try {
            wifiLock?.release()
        } catch (_: Exception) {
        }
        Logs.event("运行", "前台保活服务停止")
        super.onDestroy()
    }

    private fun startForegroundWithNotification() {
        val pi = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        val builder: Notification.Builder = if (Build.VERSION.SDK_INT >= 26) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "播放保活", NotificationManager.IMPORTANCE_LOW)
            )
            Notification.Builder(this, CHANNEL_ID)
        } else {
            Notification.Builder(this)
        }
        val notif = builder
            .setContentTitle("NZ播放器运行中")
            .setContentText("频道：" + (PlayerManager.get()?.channelName() ?: "待启动"))
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(
                NOTIFICATION_ID, notif,
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            )
        } else {
            startForeground(NOTIFICATION_ID, notif)
        }
    }

    private fun acquireLocks() {
        try {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "shoubo:cpu").apply {
                setReferenceCounted(false)
                acquire()
            }
            val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "shoubo:wifi").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (e: Exception) {
            Logs.event("异常", "WakeLock 获取失败：" + e.message)
        }
    }

    private val memoryCheck = object : Runnable {
        override fun run() {
            try {
                val am = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
                val info = am.getProcessMemoryInfo(intArrayOf(android.os.Process.myPid())).firstOrNull()
                val mb = (info?.totalPss ?: 0) / 1024
                if (mb > MEMORY_LIMIT_MB) {
                    Logs.event("内存", "进程内存 ${mb}MB 超过阈值 $MEMORY_LIMIT_MB MB，重建播放器释放资源")
                    PlayerManager.get()?.hardRebuild("内存水位超限")
                }
            } catch (_: Exception) {
            }
            handler.postDelayed(this, MEMORY_CHECK_INTERVAL_MS)
        }
    }

    companion object {
        private const val CHANNEL_ID = "shoubo_keepalive"
        private const val NOTIFICATION_ID = 1
        private const val MEMORY_CHECK_INTERVAL_MS = 5 * 60 * 1000L
        private const val MEMORY_LIMIT_MB = 250
        /** 每日自愈重启时间（文档 4.6-1）：凌晨 4:00 重建播放器 */
        private const val DAILY_RESTART_HOUR = 4
        private const val DAILY_RESTART_MINUTE = 0

        /** 每日定时自愈重启（文档 4.6-1）：凌晨4点重建播放器，清掉累积状态换全天稳定 */
        fun scheduleDailyRestart(context: Context) {
            val am = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
            val pi = PendingIntent.getBroadcast(
                context, 2001, Intent(context, DailyRestartReceiver::class.java),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            val cal = Calendar.getInstance().apply {
                set(Calendar.HOUR_OF_DAY, DAILY_RESTART_HOUR)
                set(Calendar.MINUTE, DAILY_RESTART_MINUTE)
                set(Calendar.SECOND, 0)
                set(Calendar.MILLISECOND, 0)
                if (timeInMillis <= System.currentTimeMillis()) add(Calendar.DAY_OF_YEAR, 1)
            }
            am.setRepeating(AlarmManager.RTC_WAKEUP, cal.timeInMillis, 24 * 3600 * 1000L, pi)
        }
    }
}
