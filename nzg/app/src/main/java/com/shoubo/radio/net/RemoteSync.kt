package com.shoubo.radio.net

import android.os.Handler
import android.os.Looper
import android.util.Log
import com.shoubo.radio.config.ConfigRepository
import com.shoubo.radio.util.Logs
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.net.HttpURLConnection
import java.net.URL

/**
 * 远程节目单自动同步（方案②，客户端）：
 * 预留的中央频道服务器（局域网/公网均可）下发 channels，App 定期拉取并热加载，
 * 之后换源/加台只需改服务器上的 channels.json，所有盒子自动生效。
 *
 * 服务器 = nzp2/channel-server/channels_server.py（默认端口 8080）
 *   GET /version        轻量版本探测 → {"version", "updated_at", "etag", "count"}
 *   GET /channels.json  完整节目单 → {"version", "updated_at", "channels":[...]}
 *
 * 服务地址在 config.json 顶层 remote_config.server 配置，默认已内置示例 IP；
 * 部署时把 IP 改成频道服务器所在主机的局域网地址即可（App 启动/每次轮询都重读，
 * 所以只改盒子上的 config.json 也会自动切换服务器）。
 *
 * 合并策略：远程 channels 为权威清单，整体替换本地频道（换源以服务器为准）；
 * 收藏只保留远程中仍存在的 id；当前台若被删则回退到 1 号。
 */
class RemoteSync(private val repo: ConfigRepository) {

    interface Listener {
        /** 远程节目单被应用（频道列表已更新并持久化） */
        fun onRemoteChannelsApplied(count: Int)
    }

    private val main = Handler(Looper.getMainLooper())
    private val io = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
        Thread(r, "remote-sync").apply { isDaemon = true }
    }
    private var ticker: Runnable? = null
    private var started = false
    var listener: Listener? = null

    private var lastUpdatedAt = ""

    fun start() {
        if (started) return
        started = true
        lastUpdatedAt = ""
        val tick = object : Runnable {
            override fun run() {
                val intervalMin = repo.remoteIntervalMin().coerceAtLeast(1)
                syncOnce()
                main.postDelayed(this, intervalMin * 60_000L)
            }
        }
        main.post(tick)
    }

    fun stop() {
        started = false
        ticker?.let { main.removeCallbacks(it) }
        ticker = null
    }

    private fun syncOnce() {
        val server = repo.remoteServer()
        if (server.isBlank() || !server.startsWith("http")) return
        io.execute {
            try {
                val base = server.trimEnd('/')
                // 1) 轻量探测版本
                val ver = httpGet("$base/version", 6000)
                var updated = ""
                if (ver != null) {
                    try {
                        updated = JSONObject(ver).optString("updated_at", "")
                    } catch (_: Exception) {
                    }
                }
                if (updated.isNotEmpty() && updated == lastUpdatedAt) return@execute // 未变

                // 2) 拉完整节目单
                val raw = httpGet("$base/channels.json", 8000) ?: return@execute
                val obj = JSONObject(raw)
                val arr = obj.optJSONArray("channels") ?: return@execute
                val nowUpdated = obj.optString("updated_at", updated)
                val newList = ArrayList<com.shoubo.radio.config.Channel>()
                for (i in 0 until arr.length()) {
                    val o = arr.optJSONObject(i) ?: continue
                    val name = o.optString("name", "").trim()
                    val url = o.optString("primary_url", "").trim()
                    if (name.isEmpty() || url.isEmpty()) continue
                    val headers = HashMap<String, String>()
                    val h = o.optJSONObject("headers")
                    if (h != null) for (k in h.keys()) headers[k] = h.optString(k)
                    newList.add(
                        com.shoubo.radio.config.Channel(
                            id = (i + 1).toLong(),
                            name = name,
                            primaryUrl = url,
                            backupUrl = o.optString("backup_url", "").trim(),
                            player = o.optString("player", "auto").trim(),
                            headers = headers,
                            category = o.optString("category", "").trim(),
                            region = o.optString("region", "").trim()
                        )
                    )
                }
                if (newList.isEmpty()) return@execute
                lastUpdatedAt = nowUpdated
                Logs.event("远程配置", "拉取到 ${newList.size} 个频道 (server=$base)")
                main.post { repo.applyRemoteChannels(newList); listener?.onRemoteChannelsApplied(newList.size) }
            } catch (e: Exception) {
                Logs.event("远程配置", "同步失败：" + (e.message ?: "未知"))
            }
        }
    }

    private fun httpGet(url: String, timeoutMs: Int): String? {
        return try {
            val conn = URL(url).openConnection() as HttpURLConnection
            conn.connectTimeout = timeoutMs
            conn.readTimeout = timeoutMs
            conn.setRequestProperty("User-Agent", "ShouBo/0.3 (TV)")
            conn.setRequestProperty("Accept", "application/json")
            val code = conn.responseCode
            val body = if (code == 200) {
                BufferedReader(conn.inputStream.reader(Charsets.UTF_8)).use { it.readText() }
            } else {
                null
            }
            conn.disconnect()
            body
        } catch (e: Exception) {
            null
        }
    }
}
