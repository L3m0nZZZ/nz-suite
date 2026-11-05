package com.shoubo.tv.keepalive

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.shoubo.tv.MainActivity
import com.shoubo.tv.util.Logs

/** 开机自启（文档 4.5）：盒子断电重启后自动拉起播放器，恢复固定频道 */
class BootReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        if (action == Intent.ACTION_BOOT_COMPLETED || action == "android.intent.action.QUICKBOOT_POWERON") {
            Logs.event("重启", "收到开机广播，自启播放器")
            try {
                context.startActivity(
                    Intent(context, MainActivity::class.java)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            } catch (e: Exception) {
                Logs.event("异常", "开机启动失败：" + e.message)
            }
        }
    }
}
