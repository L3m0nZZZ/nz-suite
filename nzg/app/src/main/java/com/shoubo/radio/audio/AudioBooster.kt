package com.shoubo.radio.audio

import android.content.Context
import android.media.AudioManager
import android.media.audiofx.LoudnessEnhancer
import android.os.Handler
import android.os.Looper
import com.shoubo.radio.config.Audio
import com.shoubo.radio.util.Logs

/**
 * 音量放大：两层叠加。
 *
 * 第一层「系统媒体音量顶满并持续纠偏」—— 很多盒子媒体音量默认只有六成，
 * 而遥控器音量键常被运营商固件吃掉、App 里又够不到。做法：直接
 * `setStreamVolume(STREAM_MUSIC, max, 0)`，再用 5 秒轮询 + 每次播放器重建时重顶，
 * 把被外力调低的音量顶回最大。
 *
 * ⚠️ 为什么用轮询而不是官方回调：已用 `javap` 反编译 android-34 的 android.jar 核实，
 * **`AudioManager` 根本没有 `registerVolumeChangeListener`**（网上常被误传），
 * 音量变化只有 `ContentObserver` 监听 `Settings.System.VOLUME_SETTING` 这一条路，
 * 而该 Settings 键在部分定制 ROM 上对 STREAM_MUSIC 不生效。轮询是这里唯一稳的做法。
 *
 * 第二层「软件数字增益」—— 广播源（尤其县级 FM 转播）录制电平普遍偏低，
 * 系统音量拉满仍显小。用 `LoudnessEnhancer` 挂在播放器的 audio session 上做数字增益。
 *
 * ⚠️ 三个必须处理的现实问题：
 *  - `LoudnessEnhancer(int)` 声明抛 UnsupportedOperationException / RuntimeException，
 *    定制 ROM 与老盒子上效果器可能被裁剪 —— 必须吞掉并降级为"仅顶满系统音量"。
 *  - audioSessionId 每次重建播放器都会变（且起播前可能是 0），
 *    所以增益要在 `setPlayer(...)` 时重挂；旧实例必须先 release，否则泄漏原生效果器。
 *  - `setStreamVolume` 在勿扰模式 / 音频焦点被占时可能抛 SecurityException —— 一律 catch。
 */
class AudioBooster(context: Context) {

    private val appContext = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private val am = appContext.getSystemService(Context.AUDIO_SERVICE) as? AudioManager

    private var cfg = Audio()
    private var enhancer: LoudnessEnhancer? = null
    private var attachedSession = 0

    /** 未支持软件增益时的原因，供诊断面板显示 */
    @Volatile
    var boostUnsupportedReason: String? = null
        private set

    @Volatile
    var lastAppliedBoostMb: Int = -1
        private set

    fun apply(config: Audio) {
        cfg = config
        if (cfg.forceMax) {
            raiseSystemVolume("初始化")
            main.removeCallbacks(volumePoll)
            main.postDelayed(volumePoll, VOLUME_POLL_MS)
        } else {
            main.removeCallbacks(volumePoll)
        }
    }

    // ---------- 第一层：系统媒体音量 ----------

    /** 把媒体音量顶到最大。返回是否真的改动了 */
    fun raiseSystemVolume(reason: String): Boolean {
        val mgr = am ?: return false
        return try {
            val max = mgr.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
            val cur = mgr.getStreamVolume(AudioManager.STREAM_MUSIC)
            if (cur >= max) return false
            // flags = 0：不弹系统音量 UI（盒子无人值守，弹窗会挡住画面）
            mgr.setStreamVolume(AudioManager.STREAM_MUSIC, max, 0)
            Logs.event("音量", "$reason：媒体音量 $cur/$max → $max/$max")
            true
        } catch (t: Throwable) {
            Logs.event("音量", "$reason：设置媒体音量失败（${t.javaClass.simpleName}: ${t.message}）")
            false
        }
    }

    private val volumePoll = object : Runnable {
        override fun run() {
            if (cfg.forceMax) {
                val mgr = am
                if (mgr != null) {
                    try {
                        val max = mgr.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
                        if (mgr.getStreamVolume(AudioManager.STREAM_MUSIC) < max) {
                            raiseSystemVolume("音量被调低（纠偏）")
                        }
                    } catch (_: Throwable) {
                    }
                }
                main.postDelayed(this, VOLUME_POLL_MS)
            }
        }
    }

    // ---------- 第二层：软件数字增益 ----------

    /**
     * 挂到某个播放器的 audio session 上。sessionId 为 0 表示会话尚未建立，
     * 此时记下来，稍后重试（ExoPlayer 起播后才会拿到有效 id）。
     */
    fun setPlayer(audioSessionId: Int) {
        if (audioSessionId != 0 && audioSessionId == attachedSession && enhancer != null) return
        releaseEnhancer()
        attachedSession = audioSessionId

        if (cfg.boostMb <= 0) return
        if (audioSessionId == 0) {
            main.postDelayed(retryAttach, ATTACH_RETRY_MS)
            return
        }
        val mb = cfg.boostMb.coerceIn(0, minOf(cfg.maxBoostMb, MAX_HARD_MB))
        try {
            val e = LoudnessEnhancer(audioSessionId)
            e.setTargetGain(mb)
            e.enabled = true
            enhancer = e
            lastAppliedBoostMb = mb
            boostUnsupportedReason = null
            Logs.event("音量", "软件增益已生效：+${fmtDb(mb)} dB（${mb} mb，会话 $audioSessionId）")
        } catch (t: Throwable) {
            boostUnsupportedReason = "${t.javaClass.simpleName}: ${t.message}"
            Logs.event("音量", "软件增益不可用（$boostUnsupportedReason），降级为仅顶满系统音量")
        }
    }

    private val retryAttach = Runnable {
        if (cfg.boostMb > 0 && enhancer == null) setPlayer(attachedSession)
    }

    private fun releaseEnhancer() {
        main.removeCallbacks(retryAttach)
        try {
            enhancer?.enabled = false
            enhancer?.release()
        } catch (_: Throwable) {
        }
        enhancer = null
        lastAppliedBoostMb = -1
    }

    fun release() {
        main.removeCallbacks(volumePoll)
        releaseEnhancer()
        attachedSession = 0
    }

    /** 诊断面板用的一行摘要 */
    fun summary(): String {
        val mgr = am ?: return "音频服务不可用"
        val sysPart = try {
            val max = mgr.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
            val cur = mgr.getStreamVolume(AudioManager.STREAM_MUSIC)
            "媒体音量 $cur/${max}" + if (cur >= max) "（已顶满）" else "（未顶满）"
        } catch (_: Throwable) {
            "媒体音量 读取失败"
        }
        val boostPart = when {
            cfg.boostMb <= 0 -> "软件增益 关闭"
            lastAppliedBoostMb > 0 -> "软件增益 +${fmtDb(lastAppliedBoostMb)} dB"
            boostUnsupportedReason != null -> "软件增益 不可用（$boostUnsupportedReason）"
            else -> "软件增益 待挂载"
        }
        return "$sysPart · $boostPart"
    }

    companion object {
        private const val VOLUME_POLL_MS = 5_000L
        private const val ATTACH_RETRY_MS = 1_500L

        /** 增益硬上限：超过 +20 dB 几乎必然削波，且多数设备会直接拒绝 */
        const val MAX_HARD_MB = 2000

        private fun fmtDb(mb: Int) = "%.1f".format(mb / 100.0)
    }
}
