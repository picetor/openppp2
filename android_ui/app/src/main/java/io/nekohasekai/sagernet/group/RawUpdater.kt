/******************************************************************************
 *                                                                            *
 * openppp2 fork: RawUpdater downloads an openppp2 subscription document     *
 * ({"type":"openppp2-subscription","version":1,"nodes":[...]}) from a        *
 * subscription URL and stores each node as a TYPE_CONFIG ProxyEntity holding *
 * a ConfigBean. Bare AppConfiguration JSON documents (single object or       *
 * array) are also accepted for compatibility.                               *
 *                                                                            *
 ******************************************************************************/

package io.nekohasekai.sagernet.group

import io.nekohasekai.sagernet.R
import io.nekohasekai.sagernet.database.GroupManager
import io.nekohasekai.sagernet.database.ProxyEntity
import io.nekohasekai.sagernet.database.ProxyGroup
import io.nekohasekai.sagernet.database.SagerDatabase
import io.nekohasekai.sagernet.database.SubscriptionBean
import io.nekohasekai.sagernet.fmt.internal.ConfigBean
import io.nekohasekai.sagernet.ktx.app
import io.nekohasekai.sagernet.ktx.readableMessage
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.TimeUnit

object RawUpdater : GroupUpdater() {

    override suspend fun doUpdate(
        proxyGroup: ProxyGroup,
        subscription: SubscriptionBean,
        userInterface: GroupManager.Interface?,
        byUser: Boolean
    ) {
        if (subscription.link.isBlank()) {
            error(app.getString(R.string.subscription_empty))
        }
        val text = fetchRaw(subscription.link)
        val proxies = parseRaw(text)
            ?: error(app.getString(R.string.invalid_openppp2_configuration, subscription.link))
        val progress = Progress(proxies.size)
        GroupUpdater.progress[proxyGroup.id] = progress
        SagerDatabase.proxyDao.deleteByGroup(proxyGroup.id)
        var order = SagerDatabase.proxyDao.nextOrder(proxyGroup.id) ?: 1L
        for (proxy in proxies) {
            proxy.groupId = proxyGroup.id
            proxy.userOrder = order++
            SagerDatabase.proxyDao.create(proxy)
            progress.progress++
        }
    }

    suspend fun fetchRaw(url: String): String = withContext(Dispatchers.IO) {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15000
            readTimeout = 30000
            instanceFollowRedirects = true
            setRequestProperty("User-Agent", "openppp2-android")
            setRequestProperty("Accept", "application/json,text/plain,*/*")
        }
        try {
            val code = connection.responseCode
            if (code !in 200..299) {
                error("HTTP $code")
            }
            connection.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
                .removePrefix("\uFEFF")
        } finally {
            connection.disconnect()
        }
    }

    /**
     * Parse an openppp2 subscription/configuration document.
     *
     * Primary format (what real openppp2 subscription servers return):
     *   {"type":"openppp2-subscription","version":1,"name":"...",
     *    "profilePrefix":"...","nodes":[{"id":"...","name":"...","enabled":true,
     *    "config":{...} | "config":"{...json string}" | (shorthand) "server":"ppp://...",
     *    "key":{...},"client":{...},"websocket":{...}}]}
     *
     * Legacy fallback (kept for compatibility): a bare AppConfiguration JSON
     * object or array of objects.
     */
    fun parseRaw(text: String): List<ProxyEntity>? {
        val trimmed = text.trim().removePrefix("\uFEFF").trim()
        if (trimmed.isEmpty()) return null
        val result = ArrayList<ProxyEntity>()

        val root = try {
            JSONObject(trimmed)
        } catch (_: Throwable) {
            null
        }
        if (root != null && root.optString("type") == "openppp2-subscription") {
            // Real openppp2 subscription document.
            if (root.optInt("version", 0) != 1) return null
            val nodes = root.optJSONArray("nodes") ?: return null
            val prefix = root.optString("profilePrefix").ifBlank { "" }
            for (i in 0 until nodes.length()) {
                val node = nodes.optJSONObject(i) ?: continue
                if (!node.optBoolean("enabled", true)) continue
                entityFromSubscriptionNode(node, prefix)?.let { result.add(it) }
            }
            return result.ifEmpty { null }
        }

        // Legacy fallback: bare JSON array or single object.
        val array = try {
            JSONArray(trimmed)
        } catch (_: Throwable) {
            null
        }
        if (array != null) {
            for (i in 0 until array.length()) {
                val obj = array.optJSONObject(i) ?: continue
                entityFromJson(obj)?.let { result.add(it) }
            }
        } else if (root != null) {
            entityFromJson(root)?.let { result.add(it) }
        } else {
            return null
        }
        return result.ifEmpty { null }
    }

    private fun entityFromSubscriptionNode(node: JSONObject, prefix: String): ProxyEntity? {
        val id = node.optString("id").ifBlank { return null }
        val rawName = node.optString("name").ifBlank { id }
        val name = if (prefix.isNotBlank() && !rawName.startsWith(prefix)) {
            "$prefix $rawName"
        } else {
            rawName
        }

        // A node carries either a full `config` document, or shorthand fields
        // (server + key + optional client/websocket) built on top of defaults.
        val configJson = when (val config = node.opt("config")) {
            is JSONObject -> config.toString()
            is String -> config.trim().takeIf { it.isNotEmpty() }
                ?.let { runCatching { JSONObject(it).toString() }.getOrNull() }
            else -> null
        }
        val json = configJson ?: buildShorthandConfig(node) ?: return null

        val bean = ConfigBean().apply {
            this.name = name
            type = "openppp2"
            content = json
            serverAddresses = extractServer(json)
        }
        return ProxyEntity().apply {
            putBean(bean)
        }
    }

    private fun buildShorthandConfig(node: JSONObject): String? {
        val server = node.optString("server").ifBlank { return null }
        val key = node.optJSONObject("key") ?: return null
        val root = try {
            JSONObject(DEFAULT_CONFIG_JSON)
        } catch (_: Throwable) {
            return null
        }
        root.put("key", deepMerge(root.optJSONObject("key"), key))
        val client = deepMerge(root.optJSONObject("client"), node.optJSONObject("client"))
        client.put("server", server)
        if (node.has("bandwidth")) {
            client.put("bandwidth", node.optInt("bandwidth"))
        }
        client.put("mappings", JSONArray())
        root.put("client", client)
        // CDN acceleration: merge node.websocket (host/sni) into client.websocket,
        // native client uses it for Host/SNI while client.server is the wss:// IP.
        val websocket = node.optJSONObject("websocket")
        if (websocket != null && websocket.length() > 0) {
            val clientWs = client.optJSONObject("websocket") ?: JSONObject()
            client.put("websocket", deepMerge(clientWs, websocket))
            root.put("client", client)
        }
        return root.toString()
    }

    private fun deepMerge(base: JSONObject?, overlay: JSONObject?): JSONObject {
        val result = base?.let { JSONObject(it.toString()) } ?: JSONObject()
        if (overlay == null) return result
        val keys = overlay.keys()
        while (keys.hasNext()) {
            val key = keys.next()
            val value = overlay.opt(key)
            if (value is JSONObject && result.optJSONObject(key) != null) {
                result.put(key, deepMerge(result.optJSONObject(key), value))
            } else {
                result.put(key, value)
            }
        }
        return result
    }

    private fun extractServer(json: String): String {
        return try {
            JSONObject(json).optJSONObject("client")?.optString("server").orEmpty()
        } catch (_: Throwable) {
            ""
        }
    }

    /**
     * Derive a display name from an openppp2 client.server URI, e.g.
     * "ppp://1.2.3.4:20000/" -> "1.2.3.4:20000". Falls back to "openppp2".
     */
    private fun serverHostLabel(server: String): String {
        val uri = server.trim().removePrefix("ppp://").removePrefix("tcp://")
            .removePrefix("ws://").removePrefix("wss://")
            .removePrefix("http://").removePrefix("https://").removePrefix("socks://")
        val body = uri.substringBefore('/')
        return body.ifBlank { "openppp2" }
    }

    private fun entityFromJson(obj: JSONObject): ProxyEntity? {
        // Require a recognizable openppp2 document (has a key section, or a
        // server/ip + port, or protocol). Reject arbitrary JSON.
        val hasKey = obj.has("key") || obj.has("password")
        val hasEndpoint = (obj.has("ip") || obj.has("server")) && obj.optInt("port", 0) > 0
        if (!hasKey && !hasEndpoint) return null

        val fullText = obj.toString()
        val server = extractServer(fullText)
        val bean = ConfigBean().apply {
            name = obj.optString("name").ifBlank {
                serverHostLabel(server)
            }
            type = "openppp2"
            content = fullText
            serverAddresses = server
        }
        return ProxyEntity().apply {
            putBean(bean)
        }
    }

    /**
     * Minimal valid client-side AppConfiguration used as the base when a
     * subscription node only carries shorthand fields (server + key).
     * Mirrors the default used by the official openppp2 Android client.
     */
    private const val DEFAULT_CONFIG_JSON = """{
  "concurrent": 1,
  "cdn": [80, 443],
  "key": {
    "kf": 154543927,
    "kx": 128,
    "kl": 10,
    "kh": 12,
    "protocol": "aes-128-cfb",
    "protocol-key": "N6HMzdUs7IUnYHwq",
    "transport": "aes-256-cfb",
    "transport-key": "HWFweXu2g5RVMEpy",
    "masked": false,
    "plaintext": false,
    "delta-encode": false,
    "shuffle-data": false
  },
  "ip": { "public": "0.0.0.0", "interface": "0.0.0.0" },
  "client": {
    "guid": "{F4569420-4E49-4CBA-9C36-94E722C8E363}",
    "server": "ppp://127.0.0.1:20000/",
    "bandwidth": 0,
    "reconnections": { "timeout": 5 },
    "paper-airplane": { "tcp": true }
  }
}"""
}
