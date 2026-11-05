package com.shoubo.tv.keepalive

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.shoubo.tv.player.PlayerManager
import com.shoubo.tv.util.Logs

/** 每日自愈重启的闹钟接收器：重建播放器，释放累积的内存碎片与句柄 */
class DailyRestartReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        Logs.event("重启", "每日自愈重启：重建播放器，恢复频道")
        PlayerManager.get()?.hardRebuild("每日 04:00 自愈重启")
    }
}
