package com.shoubo.tv.config

import org.json.JSONArray
import org.json.JSONObject

/**
 * 一个频道 = 主源 + 同频道备用源 + 内核偏好 + 自定义请求头 + 分组（省份，文档 1.4 / 4.1）。
 * category 用于侧边栏按省份分组；收藏单独存于 Config.favorites。
 */
data class Channel(
    val id: Long,
    val name: String,
    val primaryUrl: String,
    val backupUrl: String,
    val player: String,
    val headers: Map<String, String>,
    val category: String = ""
)

data class Resilience(
    val bufferStallSec: Int,
    val frameStallSec: Int,
    val softRetryMax: Int,
    val hardRetryMax: Int,
    val retryIntervalsSec: List<Int>,
    val probeIntervalSec: Int
)

data class Lock(val channelLock: Boolean)

data class Config(
    val channels: List<Channel>,
    val currentChannelId: Long,
    val resilience: Resilience,
    val lock: Lock,
    val favorites: Set<Long> = emptySet()
) {
    fun currentChannel(): Channel? =
        channels.firstOrNull { it.id == currentChannelId } ?: channels.firstOrNull()
}

object Models {

    fun parse(text: String): Config = parse(JSONObject(text))

    fun parse(json: JSONObject): Config {
        val channels = mutableListOf<Channel>()
        val arr = json.optJSONArray("channels") ?: JSONArray()
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val headers = mutableMapOf<String, String>()
            val h = o.optJSONObject("headers")
            if (h != null) for (k in h.keys()) headers[k] = h.optString(k)
            channels += Channel(
                id = o.optLong("id", (i + 1).toLong()),
                name = o.optString("name", "频道${i + 1}"),
                primaryUrl = o.optString("primary_url").trim(),
                backupUrl = o.optString("backup_url").trim(),
                player = o.optString("player", "auto").trim(),
                headers = headers,
                category = o.optString("category", "").trim()
            )
        }

        val r = json.optJSONObject("resilience") ?: JSONObject()
        val intervals = mutableListOf<Int>()
        val ia = r.optJSONArray("retry_intervals_sec")
        if (ia != null) for (i in 0 until ia.length()) intervals += ia.optInt(i)
        val resilience = Resilience(
            bufferStallSec = r.optInt("buffer_stall_sec", 10),
            frameStallSec = r.optInt("frame_stall_sec", 15),
            softRetryMax = r.optInt("soft_retry_max", 3),
            hardRetryMax = r.optInt("hard_retry_max", 3),
            retryIntervalsSec = if (intervals.isEmpty()) listOf(0, 5, 15, 30) else intervals,
            probeIntervalSec = r.optInt("network_probe_sec", 30)
        )

        val lock = Lock(json.optJSONObject("lock")?.optBoolean("channel_lock") ?: false)

        val favorites = mutableSetOf<Long>()
        val fa = json.optJSONArray("favorites")
        if (fa != null) for (i in 0 until fa.length()) favorites.add(fa.optLong(i))

        return Config(channels, json.optLong("current_channel_id", 1L), resilience, lock, favorites)
    }
}
