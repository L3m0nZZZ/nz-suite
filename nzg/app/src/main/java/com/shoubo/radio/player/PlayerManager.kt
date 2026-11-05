package com.shoubo.radio.player

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
import com.shoubo.radio.config.Channel
import com.shoubo.radio.config.ConfigRepository
import com.shoubo.radio.audio.AudioBooster
import com.shoubo.radio.net.NetworkMonitor
import com.shoubo.radio.util.Logs
import com.shoubo.radio.util.Stats
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

    /** 音量放大：系统音量顶满 + 软件数字增益，两层都在播放器重建时重套 */
    val audio = AudioBooster(context)
    // 当前 ExoPlayer 实例创建时使用的请求头（Referer/UA 等）。
    // 注意：头部写在 DataSource.Factory 上，换 MediaItem 不会替换头部，
    // 所以切台时若头部不同必须重建实例，否则新频道会沿用上一个频道的头部导致 403。
    private var activeHeaders: Map<String, String> = emptyMap()

    private var started = false
    private var failures = 0
    private var softCount = 0
    private var lastFailAt = 0L
    private var bufferingSince = 0L
    private var lastPosMs = 0L
    private var posStallTicks = 0
    private var nextRetryAt = 0L

    /** 本次播放会话的建立时刻（elapsedRealtime），动态签名源换票计时用 */
    private var sessionBuiltAt = 0L

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
        audio.apply(repo.config.audio)
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
        audio.release()
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
        // 重连期间用户很可能就是在按遥控器音量键，恢复播放时再顶一次
        audio.raiseSystemVolume("恢复正常")
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

    /**
     * 当前台是否为「动态签名源」——litenews / 河南大象 hntv / 聚云 juyun。
     * 这类源的地址是带时效的票，必须周期性重新解析，不能指望断了再救。
     */
    private fun isDynamicSource(): Boolean {
        val u = channel?.primaryUrl ?: return false
        return u.startsWith(LITE_SCHEME) || u.startsWith(HNTV_SCHEME) || u.startsWith(JUYUN_SCHEME)
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

    @androidx.annotation.OptIn(androidx.media3.common.util.UnstableApi::class)
    private fun check() {
        val p = player ?: return
        if (mode != Mode.PLAYING) return

        /* ---- 动态签名源：到期前主动换票 ----
         *
         * litenews 解析出来的是 100ycdn 的会话票（wsSession/wsIPSercert/wsBindIP），
         * hntv 是 wsSecret+wsTime，juyun 是 AES 密文地址 —— 都有时效。
         * 票过期后 CDN 往往不是干脆 403，而是把 keep-alive 连接挂住不发数据：
         * ExoPlayer 一直停在 STATE_READY、isPlaying=true、位置不再前进，
         * 表现就是「播了半天后一直不动」。断之前主动重建会话，
         * 重新解析换新票，比等它断（必带十几秒静音）稳得多。 */
        if (isDynamicSource() && sessionBuiltAt > 0 &&
            SystemClock.elapsedRealtime() - sessionBuiltAt > DYNAMIC_REFRESH_SEC * 1000L
        ) {
            Logs.event("换票", "动态签名源已用 ${(SystemClock.elapsedRealtime() - sessionBuiltAt) / 60000} 分钟，主动重新解析")
            hardReconnect()
            return
        }

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
                /* 渲染层：状态是"播放中"但播放位置不再前进。
                 *
                 * ⚠️ 2026-10-23 修：原判断带 `p.videoFormat != null`，
                 * 纯音频台（videoFormat 恒为 null）**根本不进这段** ——
                 * 音频流被 CDN 挂死时 ExoPlayer 仍报 STATE_READY + isPlaying，
                 * 看门狗全程装聋，这就是「广播台播半天后一直不动」的另一半根因。
                 * 广播场景绝大多数台都是纯音频，必须去掉这个条件。
                 *
                 * 注意只在 isPlaying 为真时判：暂停/切台瞬间位置本就不动。 */
                val pos = p.currentPosition
                if (p.isPlaying) {
                    if (pos != lastPosMs) {
                        lastPosMs = pos
                        posStallTicks = 0
                    } else {
                        posStallTicks++
                        if (posStallTicks * WATCHDOG_PERIOD_MS >= repo.config.resilience.frameStallSec * 1000L) {
                            posStallTicks = 0
                            fail("播放位置停滞 ${repo.config.resilience.frameStallSec} 秒（判定卡死）", stall = true)
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
        val url = ch.primaryUrl
        if (url.startsWith(LITE_SCHEME)) {
            // litenews 动态源：播放前实时拉取最新签名 URL（签名约几分钟失效）
            resolveLiteAsync(ch) { realUrl ->
                if (channel?.id == ch.id) buildPlayerWith(realUrl, ch)
            }
            return
        }
        if (url.startsWith(PAGE_SCHEME)) {
            // 页面动态源：primaryUrl = page://https://站点/电视页.html
            // 播放前抓页面，取其中第一个 http(s).m3u8 作为真实流（页面每次刷新换签名的台用）
            resolvePageAsync(ch) { realUrl ->
                if (channel?.id == ch.id && realUrl.startsWith("http")) {
                    buildPlayerWith(realUrl, ch)
                } else {
                    fail("页面源解析失败")
                }
            }
            return
        }
        if (url.startsWith(JUYUN_SCHEME)) {
            // juyun 动态源：primaryUrl = juyun://<channelId>
            // 播放前调 api.juyun.tv is_pay_tv 接口拿 AES 密文地址并解密（密钥为 juyun 平台通用常量）
            resolveJuyunAsync(ch) { realUrl ->
                if (channel?.id == ch.id && realUrl.startsWith("http")) {
                    buildPlayerWith(realUrl, ch)
                } else {
                    fail("juyun 源解析失败")
                }
            }
            return
        }
        if (url.startsWith(HNTV_SCHEME)) {
            // 河南大象系动态源：primaryUrl = hntv://<县名或clientId>[?cls=1|2]
            // 播放前调 xy-gw 网关（sha256 签名 + clientId）实时取带 wsSecret 的签名流地址
            resolveHntvAsync(ch) { realUrl ->
                if (channel?.id == ch.id && realUrl.startsWith("http")) {
                    buildPlayerWith(realUrl, ch)
                } else {
                    fail("河南源解析失败")
                }
            }
            return
        }
        buildPlayerWith(url, ch)
    }

    /** 播放前实时解析河南大象系(xy-gw)源：接口需 timestamp + sign(sha256) + clientId 三个头 */
    private fun resolveHntvAsync(ch: Channel, done: (String) -> Unit) {
        Thread {
            var target: String = ch.primaryUrl
            try {
                val u = android.net.Uri.parse(ch.primaryUrl) ?: return@Thread
                val key = (u.host ?: "").trim()
                val clientId = HNTV_CLIENTS[key] ?: key
                val cls = (u.getQueryParameter("cls") ?: "1").trim()
                val ts = (System.currentTimeMillis() / 1000L).toString()
                val sign = sha256Hex(HNTV_SALT + ts)
                val api = "https://xy-gw.hntv.tv/program/getAuth/live/class/$cls"
                val conn = (java.net.URL(api).openConnection() as java.net.HttpURLConnection).apply {
                    requestMethod = "GET"
                    connectTimeout = 8000
                    readTimeout = 8000
                    setRequestProperty("User-Agent", "okhttp/4.9.0")
                    setRequestProperty("timestamp", ts)
                    setRequestProperty("sign", sign)
                    setRequestProperty("clientId", clientId)
                }
                if (conn.responseCode == 200) {
                    val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
                    val m = Regex("\"streams\":\\[\"(https?://[^\"]+)\"\\]").find(body)
                    if (m != null) target = m.groupValues[1].replace("\\/", "/")
                }
                conn.disconnect()
            } catch (_: Exception) {
            }
            val real = target
            main.post { done(real) }
        }.start()
    }

    /** sHA-256 十六进制（与服务端 sign = sha256(salt + timestamp) 一致） */
    private fun sha256Hex(s: String): String = try {
        val md = java.security.MessageDigest.getInstance("SHA-256")
        md.digest(s.toByteArray(Charsets.UTF_8)).joinToString("") { "%02x".format(it) }
    } catch (_: Exception) {
        ""
    }

    /** 播放前实时解析 juyun(居云) 源：接口返回 AES-ECB(PKCS7) 加密地址，密钥见 JUYUN_KEY */
    private fun resolveJuyunAsync(ch: Channel, done: (String) -> Unit) {
        Thread {
            var target: String = ch.primaryUrl
            try {
                val id = ch.primaryUrl.removePrefix(JUYUN_SCHEME).trim()
                val api = "https://api.juyun.tv/juxian/api/is_pay_tv.jsp?id=$id&callback=cb"
                val conn = (java.net.URL(api).openConnection() as java.net.HttpURLConnection).apply {
                    requestMethod = "GET"
                    connectTimeout = 8000
                    readTimeout = 8000
                    setRequestProperty("User-Agent", DEFAULT_UA)
                    setRequestProperty("Referer", "https://kanxianyang.com/")
                }
                if (conn.responseCode == 200) {
                    val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
                    val m = Regex("\"address\":\"([^\"]+)\"").find(body)
                    if (m != null) {
                        val real = decryptJuyun(m.groupValues[1])
                        if (real != null) target = if (real.startsWith("//")) "https:$real" else real
                    }
                }
                conn.disconnect()
            } catch (_: Exception) {
            }
            val real = target
            main.post { done(real) }
        }.start()
    }

    /** juyun AES-128-ECB 解密（与平台网页端一致） */
    private fun decryptJuyun(enc: String): String? = try {
        val key = android.util.Base64.decode(JUYUN_KEY, android.util.Base64.DEFAULT)
        val cipher = javax.crypto.Cipher.getInstance("AES/ECB/PKCS5Padding")
        cipher.init(javax.crypto.Cipher.DECRYPT_MODE, javax.crypto.spec.SecretKeySpec(key, "AES"))
        String(cipher.doFinal(android.util.Base64.decode(enc, android.util.Base64.DEFAULT)), Charsets.UTF_8)
    } catch (_: Exception) {
        null
    }

    /** 播放前实时解析页面源：抓 HTML 取第一个 m3u8 直链 */
    private fun resolvePageAsync(ch: Channel, done: (String) -> Unit) {
        Thread {
            var target: String = ch.primaryUrl
            try {
                val page = ch.primaryUrl.removePrefix(PAGE_SCHEME)
                val conn = (java.net.URL(page).openConnection() as java.net.HttpURLConnection).apply {
                    requestMethod = "GET"
                    connectTimeout = 8000
                    readTimeout = 8000
                    setRequestProperty("User-Agent", DEFAULT_UA)
                }
                val code = conn.responseCode
                if (code == 200) {
                    val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
                    val m = Regex("https?://[^\"'\\s<>]+\\.m3u8[^\"'\\s<>]*").find(body)
                    if (m != null) target = m.value
                }
                conn.disconnect()
            } catch (_: Exception) {
            }
            val real = target
            main.post { done(real) }
        }.start()
    }

    /**
     * 播放前实时解析 litenews 源：primaryUrl = litenews://orgid?name=频道名[&kind=radio|tv]
     *
     * NZ广播(nzg) 关键差异：litenews 把「广播」和「电视」放在两个端点
     *   - 广播 GET /v1/app/play/radio/live?orgid=<n>
     *   - 电视 GET /v1/app/play/tv/live?orgid=<n>
     * 绝大多数县台账号的广播只出现在 radio 端点（nzp 只查 tv 端点，所以取不到）。
     * 这里默认「先 radio 后 tv」，兼顾两类账号；要强制单端点可写 &kind=tv / &kind=radio。
     */
    private fun resolveLiteAsync(ch: Channel, done: (String) -> Unit) {
        Thread {
            var target: String = ch.primaryUrl
            try {
                val u = android.net.Uri.parse(ch.primaryUrl) ?: return@Thread
                val orgid = u.host ?: return@Thread
                val want = (u.getQueryParameter("name") ?: "").trim()
                val kind = (u.getQueryParameter("kind") ?: "").trim().lowercase()
                val endpoints = when (kind) {
                    "radio" -> listOf("radio")
                    "tv" -> listOf("tv")
                    else -> listOf("radio", "tv")
                }
                for (ep in endpoints) {
                    val hit = fetchLiteStream(orgid, ep, want)
                    if (hit != null) {
                        target = hit
                        break
                    }
                }
            } catch (_: Exception) {
            }
            val real = target
            main.post { done(real) }
        }.start()
    }

    /** 拉 litenews 指定端点(radio/tv) 并按频道名子串匹配，返回可用 stream；未命中返回 null */
    private fun fetchLiteStream(orgid: String, kind: String, want: String): String? {
        var found: String? = null
        try {
            val api = "https://app.litenews.cn/v1/app/play/$kind/live?orgid=$orgid"
            val conn = (java.net.URL(api).openConnection() as java.net.HttpURLConnection).apply {
                requestMethod = "GET"
                connectTimeout = 8000
                readTimeout = 8000
                setRequestProperty("User-Agent",
                    "Mozilla/5.0 (Linux; Android 9; TV) AppleWebKit/537.36")
            }
            if (conn.responseCode == 200) {
                val body = conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
                val obj = org.json.JSONObject(body)
                if (obj.optInt("code") == 1) {
                    val data = obj.optJSONArray("data")
                    if (data != null) {
                        for (i in 0 until data.length()) {
                            val c = data.optJSONObject(i) ?: continue
                            val nm = c.optString("name", "")
                            val st = c.optString("stream", "")
                            if (st.startsWith("http") && (want.isEmpty() || nm.contains(want))) {
                                found = st
                                break
                            }
                        }
                    }
                }
            }
            conn.disconnect()
        } catch (_: Exception) {
        }
        return found
    }

    private fun buildPlayerWith(url: String, ch: Channel) {
        // 换票计时起点：每次真正拿到底层地址（含动态源解析结果）都重新计时
        sessionBuiltAt = SystemClock.elapsedRealtime()
        val p = player
        // 头部不同必须重建：DataSource.Factory 的头部在实例创建时固定，
        // 直接 setMediaItem 会沿用旧头部（铜川等需要 Referer 的台会 403）。
        if (p != null && activeHeaders == ch.headers) {
            // 已有实例且头部一致：直接替换数据源，避免重建（保留画面）
            try {
                p.setMediaItem(buildMediaItem(url, ch))
                p.prepare()
                p.playWhenReady = true
                playerView?.player = p
                return
            } catch (_: Exception) {
            }
        }
        buildPlayerWithNew(url, ch)
    }

    /**
     * 构造 MediaItem。
     * 注意：本项目 media3 版本的 MediaItem.Builder 没有 setRequestHeaders，
     * 头信息统一由 buildPlayerWithNew 里的 DataSource.Factory 承载（对 m3u8 主列表、
     * 二级列表、ts 分片所有请求均生效），并由 buildPlayerWith 在头部变化时强制重建实例。
     */
    private fun buildMediaItem(url: String, ch: Channel): MediaItem = MediaItem.fromUri(url)

    /**
     * 重建播放器实例。
     *
     * `@OptIn(UnstableApi::class)`：media3 把 `DefaultHttpDataSource.Factory` 的链式配置、
     * `DefaultLoadControl.Builder`、`ExoPlayer.audioSessionId` 都标成了 UnstableApi。
     * 本工程用的是 1.4.1 这一固定小版本、且这些 API 在 1.x 内签名稳定，所以在此处显式接受；
     * 作用域限定在本函数内，不会掩盖其它位置的同类问题。
     */
    @androidx.annotation.OptIn(androidx.media3.common.util.UnstableApi::class)
    private fun buildPlayerWithNew(url: String, ch: Channel) {
        if (channel == null) return
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
        p.setMediaItem(buildMediaItem(url, ch))
        p.addListener(playerListener)
        p.playWhenReady = true
        p.prepare()
        player = p
        playerView?.player = p
        activeHeaders = ch.headers

        // 音量：每次重建都要重套。系统音量可能被外力调低，
        // 软件增益绑的是 audioSessionId —— 新实例等于新会话，必须重挂。
        audio.raiseSystemVolume("播放器重建")
        audio.setPlayer(p.audioSessionId)
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
        activeHeaders = emptyMap()
        // 旧会话已销毁，效果器必须跟着放掉，否则原生效果器泄漏
        audio.setPlayer(0)
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
        const val LITE_SCHEME = "litenews://"
        const val PAGE_SCHEME = "page://"
        const val JUYUN_SCHEME = "juyun://"
        const val HNTV_SCHEME = "hntv://"
        private const val JUYUN_KEY = "ji0e3G8RR/JrBULhyaJUdg=="

        /** 河南大象系 xy-gw 网关：sign = sha256(salt + timestamp) */
        private const val HNTV_SALT = "6ca114a836ac7d73"

        /**
         * 动态签名源主动换票周期（秒）。
         *
         * litenews 的 100ycdn 会话票、hntv 的 wsSecret+wsTime、juyun 的 AES 密文地址
         * 都是有时效的；票过期后 CDN 常见做法是挂住连接不发数据，客户端表现为
         * 「播了半天后一直不动」。
         *
         * 2026-10-23 六小时长测定案：单县 155 / 巨野 139 用**同一个签名 URL** 连拉
         * 1428 次（各 714 次、间隔 30s），全部 200、fails 恒 0、playlist 一路推进
         * → 有效期实测 ≥ 6 小时。取 3 小时 = 2 倍余量；
         * 别再调回 20 分钟 —— 换票实测掉 6 秒音频（win 版实测；安卓重建更快些但也是秒级），
         * 20 分钟一次等于每天 72 次断音。
         * 万一某平台票更短，失效后走「判死 → 重建」兜底，不会卡死。
         * 现场要调就改这里（配置里没有这个键，避免 Gson 缺字段被读成 0）。
         */
        private const val DYNAMIC_REFRESH_SEC = 10800L

        /** 河南各县 App 专属 clientId（键为拼音，避免中文 host 解析问题；新增县只需在此登记） */
        private val HNTV_CLIENTS = mapOf(
            "shenqiu" to "09a9ab11660d4aa5a4a0fb6a7809f1e6",   // 沈丘
            "taikang" to "71a0b3560f4e422ab7e5260ea14f1a8f",   // 太康
            "xihua" to "069b05fa076f4a9b9a97fea2eac9c97d",     // 西华
            "luyi" to "ecddfc8273dd42508b2d5d427437d067",      // 鹿邑
            "fugou" to "558feebc64524f988fa12d9ecaddf81e",     // 扶沟
        )

        @Volatile
        private var INSTANCE: PlayerManager? = null

        /** 供前台服务等无 Activity 上下文的组件调用 */
        fun get(): PlayerManager? = INSTANCE
    }
}
