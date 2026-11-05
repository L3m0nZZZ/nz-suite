package com.shoubo.tv.player

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.datasource.DefaultHttpDataSource
import androidx.media3.exoplayer.DefaultLoadControl
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory
import androidx.media3.ui.PlayerView
import com.shoubo.tv.config.Channel
import com.shoubo.tv.config.ConfigRepository
import com.shoubo.tv.net.NetworkMonitor
import com.shoubo.tv.util.Logs
import com.shoubo.tv.util.Stats
import java.util.Locale

/**
 * 播放内核 + 自愈状态机（文档 4.1 / 4.3 / 4.4）。
 *
 * 重连节奏（指数退避 + 30秒封顶，永不放弃）：
 *   第1次失败立即重试，第2次等5秒、第3次等15秒，之后固定30秒无限循环。
 * 手段分级：软重连（stop→prepare，保留Surface，画面闪断最短）最多 soft_retry_max 次，
 *   然后硬重连（释放并重建播放器）最多 hard_retry_max 次，
 *   仍失败进入休眠等待：每 network_probe_sec 秒探测一轮网络，恢复后立即重连。
 *
 * 铁律：重连时只重新加载当前频道当前源，代码里不存在任何换台/换源逻辑（文档 4.2）。
 */
class PlayerManager(
    context: Context,
    private val repo: ConfigRepository,
    private val network: NetworkMonitor
) {

    enum class Mode { PLAYING, RETRYING, WAITING_NETWORK }

    interface Ui {
        fun onModeChanged(mode: Mode)
        fun onTick()
    }

    var ui: Ui? = null

    var mode = Mode.PLAYING
        private set

    private val appContext = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private var player: ExoPlayer? = null
    private var playerView: PlayerView? = null
    private var channel: Channel? = null

    private var started = false
    private var failures = 0
    private var softCount = 0
    private var lastFailAt = 0L
    private var bufferingSince = 0L
    private var lastPosMs = 0L
    private var posStallTicks = 0
    private var nextRetryAt = 0L

    private var retryPending: Runnable? = null
    private var waitingPending: Runnable? = null
    private var watchdog: Runnable? = null

    init {
        INSTANCE = this
    }

    fun channelName(): String = channel?.name ?: "未配置频道"

    fun currentChannelId(): Long = channel?.id ?: -1L

    fun failures(): Int = failures

    /**
     * 手动切台（侧边栏点击/频道键）。铁律不受影响：自动逻辑仍然只重连当前频道当前源，
     * 切台只有用户主动这一个入口（文档 FR-05：列表供手动切换）。
     */
    fun switchTo(id: Long) {
        val ch = repo.config.channels.firstOrNull { it.id == id } ?: return
        if (channel?.id == id) return
        Logs.event("切台", "手动切换：${ch.name}")
        channel = ch
        repo.saveCurrentChannel(id)
        hardRebuild("手动切台：${ch.name}")
    }

    fun nextRetryInSec(): Long {
        val v = (nextRetryAt - SystemClock.elapsedRealtime()) / 1000
        return if (v > 0) v else 0
    }

    fun attach(view: PlayerView) {
        playerView = view
        view.player = player
    }

    fun start() {
        if (started) return
        started = true
        val ch = repo.config.currentChannel()
        if (ch == null) {
            Logs.event("异常", "配置中没有任何频道，无法播放")
            return
        }
        channel = ch
        if (ch.player == "ijk") {
            Logs.event("提示", "ijkplayer 备用内核为二期计划，当前使用 ExoPlayer")
        }
        network.onRestored = { onNetworkRestored() }
        startWatchdog()
        buildPlayer()
    }

    // ---------- 对外触发入口（每日自愈 / 内存超限 / 配置热加载 / 崩溃自启后） ----------

    fun hardRebuild(reason: String) {
        Logs.event("重启", "播放器重建：$reason")
        failures = 0
        softCount = 0
        lastFailAt = 0
        nextRetryAt = 0
        posStallTicks = 0
        cancelPending()
        cancelWaiting()
        mode = Mode.PLAYING
        buildPlayer()
        ui?.onModeChanged(mode)
    }

    fun onConfigChanged() {
        val ch = repo.config.currentChannel()
        if (ch == null) {
            Logs.event("异常", "新配置中没有可用频道，保持现状")
            return
        }
        val changed = ch.id != channel?.id || ch.primaryUrl != channel?.primaryUrl
        channel = ch
        hardRebuild(if (changed) "配置变更（频道或地址变化）" else "配置变更")
    }

    fun releaseAll() {
        cancelPending()
        cancelWaiting()
        watchdog?.let { main.removeCallbacks(it) }
        watchdog = null
        releasePlayer()
        if (INSTANCE === this) INSTANCE = null
    }

    // ---------- 播放器事件 ----------

    private val playerListener = object : Player.Listener {
        override fun onPlaybackStateChanged(playbackState: Int) {
            when (playbackState) {
                Player.STATE_READY -> bufferingSince = 0
                // 直播流不应走到 ENDED，走到即说明推流侧断了
                Player.STATE_ENDED -> fail("直播流结束")
            }
        }

        override fun onIsPlayingChanged(isPlaying: Boolean) {
            if (isPlaying) onSuccess()
        }

        override fun onPlayerError(error: PlaybackException) {
            fail("播放错误：" + error.errorCodeName)
        }
    }

    private fun onSuccess() {
        if (mode == Mode.PLAYING) return
        val cost = if (lastFailAt > 0) (SystemClock.elapsedRealtime() - lastFailAt) / 1000.0 else 0.0
        Logs.event("恢复", String.format(Locale.CHINA, "恢复正常，耗时 %.1f 秒，共尝试 %d 次", cost, failures))
        failures = 0
        softCount = 0
        lastFailAt = 0
        nextRetryAt = 0
        posStallTicks = 0
        cancelPending()
        cancelWaiting()
        mode = Mode.PLAYING
        ui?.onModeChanged(mode)
    }

    private fun fail(reason: String, stall: Boolean = false) {
        if (mode == Mode.WAITING_NETWORK) return
        if (mode == Mode.PLAYING) {
            lastFailAt = SystemClock.elapsedRealtime()
            Stats.incDisconnect()
            if (stall) Stats.incStall()
            Logs.event("断线", reason)
            mode = Mode.RETRYING
            ui?.onModeChanged(mode)
        }
        failures++
        cancelPending()
        val delaySec = retryInterval(failures)
        nextRetryAt = SystemClock.elapsedRealtime() + delaySec * 1000L
        Logs.event("重连", "第 $failures 次失败，${if (delaySec == 0) "立即" else "${delaySec}秒后"}重试")
        val r = Runnable { attemptRetry() }
        retryPending = r
        main.postDelayed(r, delaySec * 1000L)
        ui?.onTick()
    }

    private fun retryInterval(failureNo: Int): Int {
        val intervals = repo.config.resilience.retryIntervalsSec
        return intervals[(failureNo - 1).coerceIn(0, intervals.size - 1)]
    }

    private fun attemptRetry() {
        retryPending = null
        if (!network.online) {
            enterWaiting()
            return
        }
        val r = repo.config.resilience
        if (softCount < r.softRetryMax) {
            softCount++
            softReconnect()
        } else if (failures <= r.softRetryMax + r.hardRetryMax) {
            hardReconnect()
        } else {
            enterWaiting()
        }
    }

    /** 软重连：重置播放器重新 prepare，保留 Surface，画面闪断最短 */
    private fun softReconnect() {
        Logs.event("重连", "软重连（保留画面）")
        val p = player
        if (p == null) {
            hardReconnect()
            return
        }
        try {
            p.stop()
            p.seekToDefaultPosition()
            p.prepare()
            p.playWhenReady = true
        } catch (e: Exception) {
            Logs.event("重连", "软重连异常，转硬重连：" + e.message)
            hardReconnect()
        }
    }

    /** 硬重连：完全释放并重建播放器实例，恢复能力最强（会黑屏一下） */
    private fun hardReconnect() {
        Logs.event("重连", "硬重连（重建播放器实例）")
        buildPlayer()
    }

    private fun enterWaiting() {
        if (mode == Mode.WAITING_NETWORK) return
        mode = Mode.WAITING_NETWORK
        Logs.event("等待", "连续重试失败，进入休眠等待（每 ${repo.config.resilience.probeIntervalSec} 秒探测网络）")
        ui?.onModeChanged(mode)
        scheduleProbe()
    }

    /** 休眠等待模式：30秒周期探测网络；恢复后立即重连，不等满整轮 */
    private fun scheduleProbe() {
        val r = Runnable {
            network.probe { ok ->
                if (mode != Mode.WAITING_NETWORK) return@probe
                if (ok) {
                    Logs.event("恢复", "网络探测成功，立即重连原频道原源")
                    failures = 0
                    softCount = 0
                    mode = Mode.RETRYING
                    ui?.onModeChanged(mode)
                    buildPlayer()
                } else {
                    scheduleProbe()
                }
            }
        }
        waitingPending = r
        main.postDelayed(r, repo.config.resilience.probeIntervalSec * 1000L)
    }

    private fun onNetworkRestored() {
        when (mode) {
            Mode.WAITING_NETWORK -> {
                cancelWaiting()
                Logs.event("恢复", "收到网络恢复回调，立即重连")
                failures = 0
                softCount = 0
                mode = Mode.RETRYING
                ui?.onModeChanged(mode)
                buildPlayer()
            }
            Mode.RETRYING -> {
                // 正在退避等待下一次重试，网络恢复后提前触发（恢复即连）
                cancelPending()
                attemptRetry()
            }
            Mode.PLAYING -> Unit
        }
    }

    // ---------- 看门狗：传输层 + 渲染层检测（文档 4.3） ----------

    private fun startWatchdog() {
        watchdog = object : Runnable {
            override fun run() {
                try {
                    check()
                } catch (_: Exception) {
                }
                main.postDelayed(this, WATCHDOG_PERIOD_MS)
            }
        }
        main.postDelayed(watchdog!!, WATCHDOG_PERIOD_MS)
    }

    private fun check() {
        val p = player ?: return
        if (mode != Mode.PLAYING) return
        when (p.playbackState) {
            Player.STATE_BUFFERING -> {
                // 传输层：持续缓冲超过阈值且无进展
                if (bufferingSince == 0L) {
                    bufferingSince = SystemClock.elapsedRealtime()
                } else if (SystemClock.elapsedRealtime() - bufferingSince >=
                    repo.config.resilience.bufferStallSec * 1000L
                ) {
                    bufferingSince = 0
                    fail("缓冲停滞超过 ${repo.config.resilience.bufferStallSec} 秒")
                }
            }
            Player.STATE_READY -> {
                bufferingSince = 0
                // 渲染层：状态是"播放中"但播放位置不再前进。
                // ExoPlayer 的播放位置由视频渲染时钟驱动，解码 hang 住时位置冻结，
                // 这正是"画面卡死但软件以为在播"的场景，直接判卡死。
                val pos = p.currentPosition
                if (p.isPlaying && p.videoFormat != null) {
                    if (pos != lastPosMs) {
                        lastPosMs = pos
                        posStallTicks = 0
                    } else {
                        posStallTicks++
                        if (posStallTicks * WATCHDOG_PERIOD_MS >= repo.config.resilience.frameStallSec * 1000L) {
                            posStallTicks = 0
                            fail("画面停滞 ${repo.config.resilience.frameStallSec} 秒（判定卡死）", stall = true)
                        }
                    }
                } else {
                    posStallTicks = 0
                    lastPosMs = pos
                }
            }
        }
    }

    // ---------- 播放器生命周期 ----------

    private fun buildPlayer() {
        val ch = channel ?: return
        releasePlayer()
        bufferingSince = 0
        lastPosMs = 0
        posStallTicks = 0
        val httpFactory = DefaultHttpDataSource.Factory()
            .setUserAgent(ch.headers["User-Agent"] ?: DEFAULT_UA)
            .setConnectTimeoutMs(10_000)
            .setReadTimeoutMs(10_000)
            .setAllowCrossProtocolRedirects(true)
            .setDefaultRequestProperties(ch.headers.filterKeys { !it.equals("User-Agent", ignoreCase = true) })
        val p = ExoPlayer.Builder(appContext)
            .setMediaSourceFactory(DefaultMediaSourceFactory(httpFactory))
            .setLoadControl(
                DefaultLoadControl.Builder()
                    // 直播加大缓冲：无人值守延迟不敏感，用少量延迟换抗抖动（文档 4.1）
                    .setBufferDurationsMs(30_000, 60_000, 2_500, 5_000)
                    .build()
            )
            .build()
        p.setMediaItem(MediaItem.fromUri(ch.primaryUrl))
        p.addListener(playerListener)
        p.playWhenReady = true
        p.prepare()
        player = p
        playerView?.player = p
    }

    private fun releasePlayer() {
        playerView?.player = null
        player?.let {
            try {
                it.removeListener(playerListener)
            } catch (_: Exception) {
            }
            try {
                it.release()
            } catch (_: Exception) {
            }
        }
        player = null
    }

    private fun cancelPending() {
        retryPending?.let { main.removeCallbacks(it) }
        retryPending = null
    }

    private fun cancelWaiting() {
        waitingPending?.let { main.removeCallbacks(it) }
        waitingPending = null
    }

    companion object {
        private const val WATCHDOG_PERIOD_MS = 5000L
        private const val DEFAULT_UA =
            "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36 ShouBo/0.1"

        @Volatile
        private var INSTANCE: PlayerManager? = null

        /** 供前台服务等无 Activity 上下文的组件调用 */
        fun get(): PlayerManager? = INSTANCE
    }
}
