/******************************************************************************
 *                                                                            *
 * Copyright (C) 2021 by nekohasekai <contact-sagernet@sekai.icu>             *
 *                                                                            *
 * This program is free software: you can redistribute it and/or modify       *
 * it under the terms of the GNU General Public License as published by       *
 * the Free Software Foundation, either version 3 of the License, or          *
 *  (at your option) any later version.                                       *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.       *
 *                                                                            *
 ******************************************************************************/

package io.nekohasekai.sagernet.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.icu.util.ULocale
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.View
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.app.ActivityCompat
import androidx.core.os.LocaleListCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.preference.EditTextPreference
import androidx.preference.ListPreference
import androidx.preference.Preference
import androidx.preference.PreferenceFragmentCompat
import androidx.preference.SwitchPreference
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import io.nekohasekai.sagernet.*
import io.nekohasekai.sagernet.Key.MODE_VPN
import io.nekohasekai.sagernet.database.DataStore
import io.nekohasekai.sagernet.database.preference.EditTextPreferenceModifiers
import io.nekohasekai.sagernet.ktx.*
import io.nekohasekai.sagernet.ui.profile.ProfileSettingsActivity
import io.nekohasekai.sagernet.utils.Theme
import io.nekohasekai.sagernet.widget.ColorPickerPreference
import io.nekohasekai.sagernet.widget.LinkOrContentPreference
import kotlinx.coroutines.delay
import java.util.Locale

class SettingsPreferenceFragment : PreferenceFragmentCompat() {

    private lateinit var isProxyApps: SwitchPreference

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        listView.layoutManager = FixedLinearLayoutManager(listView)
        listView.setPadding(0,0,0,dp2px(64))
        ViewCompat.setOnApplyWindowInsetsListener(listView) { v, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars()
                        or WindowInsetsCompat.Type.displayCutout()
            )
            v.updatePadding(
                left = bars.left,
                right = bars.right,
                bottom = bars.bottom + dp2px(64),
            )
            insets
        }
    }

    val reloadListener = Preference.OnPreferenceChangeListener { _, _ ->
        needReload()
        true
    }

    override fun onCreatePreferences(savedInstanceState: Bundle?, rootKey: String?) {
        preferenceManager.preferenceDataStore = DataStore.configurationStore
        DataStore.initGlobal()
        addPreferencesFromResource(R.xml.global_preferences)

        // common
        isProxyApps = findPreference(Key.PROXY_APPS)!!
        val bypassLan = findPreference<SwitchPreference>(Key.BYPASS_LAN)!!
        val requireHttp = findPreference<SwitchPreference>(Key.REQUIRE_HTTP)!!
        val appendHttpProxy = findPreference<SwitchPreference>(Key.APPEND_HTTP_PROXY)!!
        val httpProxyException = findPreference<EditTextPreference>(Key.HTTP_PROXY_EXCEPTION)!!
        httpProxyException.setOnBindEditTextListener(EditTextPreferenceModifiers.Multiline)
        // app settings
        findPreference<ColorPickerPreference>(Key.APP_THEME)!!.setOnPreferenceChangeListener { _, newTheme ->
            val theme = Theme.getTheme(newTheme as Int)
            app.setTheme(theme)
            requireActivity().apply {
                ActivityCompat.recreate(this)
            }
            true
        }

        findPreference<ListPreference>(Key.NIGHT_THEME)!!.setOnPreferenceChangeListener { _, newValue ->
            Theme.currentNightMode = (newValue as String).toInt()
            Theme.applyNightTheme()
            requireActivity().apply {
                ActivityCompat.recreate(this)
            }
            true
        }

        fun getLanguageDisplayName(code: String): String = run {
            return when (code) {
                "" -> getString(R.string.language_system_default)
                "ar" -> getString(R.string.language_ar_display_name)
                "en-US" -> getString(R.string.language_en_display_name)
                "es" -> getString(R.string.language_es_display_name)
                "fa" -> getString(R.string.language_fa_display_name)
                "fr" -> getString(R.string.language_fr_display_name)
                "id" -> getString(R.string.language_id_display_name)
                "it" -> getString(R.string.language_it_display_name)
                "ja" -> getString(R.string.language_ja_display_name)
                "ko" -> getString(R.string.language_ko_display_name)
                "nb-NO" -> getString(R.string.language_nb_NO_display_name)
                "ru" -> getString(R.string.language_ru_display_name)
                "ta" -> getString(R.string.language_ta_display_name)
                "tr" -> getString(R.string.language_tr_display_name)
                "zh-Hans-CN" -> getString(R.string.language_zh_Hans_CN_display_name)
                "zh-Hant-TW" -> getString(R.string.language_zh_Hant_TW_display_name)
                else -> if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                    ULocale.forLanguageTag(code).displayName
                } else {
                    Locale.forLanguageTag(code).displayName
                }
            }
        }
        val appLanguage = findPreference<ListPreference>(Key.APP_LANGUAGE)!!
        val locale = when (val value = AppCompatDelegate.getApplicationLocales().toLanguageTags()) {
            // https://stackoverflow.com/questions/13291578/how-to-localize-an-android-app-in-indonesian-language
            // Some old Android versions still return "in".
            "in" -> "id"
            else -> value
        }
        appLanguage.summary = getLanguageDisplayName(locale)
        appLanguage.value = if (locale in resources.getStringArray(R.array.language_value)) locale else ""
        appLanguage.setOnPreferenceChangeListener { _, newValue ->
            newValue as String
            AppCompatDelegate.setApplicationLocales(LocaleListCompat.forLanguageTags(newValue)) // "id" always works
            appLanguage.summary = getLanguageDisplayName(newValue)
            appLanguage.value = newValue
            true
        }

        val serviceMode = findPreference<ListPreference>(Key.SERVICE_MODE)!!
        val mtu = findPreference<EditTextPreference>(Key.MTU)!!
        val allowAppsBypassVpn = findPreference<SwitchPreference>(Key.ALLOW_APPS_BYPASS_VPN)!!
        serviceMode.setOnPreferenceChangeListener { _, newValue ->
            newValue as String
            mtu.isEnabled = newValue == MODE_VPN
            allowAppsBypassVpn.isEnabled = newValue == MODE_VPN
            isProxyApps.isEnabled = newValue == MODE_VPN
            bypassLan.isEnabled = newValue == MODE_VPN
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                appendHttpProxy.isVisible = requireHttp.isChecked && newValue == MODE_VPN
                httpProxyException.isVisible = requireHttp.isChecked && newValue == MODE_VPN
                        && appendHttpProxy.isVisible && appendHttpProxy.isChecked
            }
            if (SagerNet.started) {
                runOnDefaultDispatcher {
                    SagerNet.stopService()
                    delay(300) // FIXME: Why this is needed?
                    SagerNet.startService()
                }
            }
            true
        }
        mtu.isEnabled = serviceMode.value == MODE_VPN
        mtu.setOnBindEditTextListener(EditTextPreferenceModifiers.Number)
        mtu.onPreferenceChangeListener = reloadListener
        allowAppsBypassVpn.isEnabled = serviceMode.value == MODE_VPN
        allowAppsBypassVpn.onPreferenceChangeListener = reloadListener

        findPreference<ListPreference>(Key.LOG_LEVEL)!!.setOnPreferenceChangeListener { _, newValue ->
            if ((newValue as String).toInt() == LogLevel.DEBUG && !DataStore.logLevelDebugWarningDisable) {
                MaterialAlertDialogBuilder(requireContext()).apply {
                    setMessage(R.string.debug_log_sum)
                    setPositiveButton(android.R.string.ok, null)
                    setNeutralButton(R.string.do_not_show_again, { _, _ ->
                        DataStore.logLevelDebugWarningDisable = true
                    })
                }.show()
            }
            needReload()
            true
        }

        findPreference<ListPreference>(Key.PROVIDER_ROOT_CA)!!.setOnPreferenceChangeListener { _, newValue ->
           if ((newValue as String).toInt() == RootCAProvider.CUSTOM) {
                runOnMainDispatcher {
                    val context = requireContext()
                    MaterialAlertDialogBuilder(context)
                        .setMessage(getString(R.string.custom_root_ca_hint, context.packageName))
                        .setPositiveButton(android.R.string.ok, null)
                        .show()
                }
            }
            true
        }

        // route settings
        isProxyApps.isEnabled = serviceMode.value == MODE_VPN
        isProxyApps.setOnPreferenceChangeListener { _, newValue ->
            startActivity(Intent(activity, AppManagerActivity::class.java))
            newValue as Boolean
        }

        bypassLan.isEnabled = serviceMode.value == MODE_VPN
        bypassLan.setOnPreferenceChangeListener { _, _ ->
            needReload()
            true
        }

        val rulesProvider = findPreference<ListPreference>(Key.RULES_PROVIDER)!!
        val rulesGeositeUrl = findPreference<LinkOrContentPreference>(Key.RULES_GEOSITE_URL)!!
        val rulesGeoipUrl = findPreference<LinkOrContentPreference>(Key.RULES_GEOIP_URL)!!
        rulesProvider.setOnPreferenceChangeListener { _, newValue ->
            val provider = (newValue as String).toInt()
            rulesGeositeUrl.isVisible = provider == 3
            rulesGeoipUrl.isVisible = provider == 3
            true
        }
        rulesGeositeUrl.isVisible = DataStore.rulesProvider == 3
        rulesGeoipUrl.isVisible = DataStore.rulesProvider == 3

        // protocol settings

        // DNS settings (openppp2: direct DNS + tunnel DNS, 2 each)
        findPreference<EditTextPreference>(Key.DNS_DIRECT1)!!.onPreferenceChangeListener = reloadListener
        findPreference<EditTextPreference>(Key.DNS_DIRECT2)!!.onPreferenceChangeListener = reloadListener
        findPreference<EditTextPreference>(Key.DNS1)!!.onPreferenceChangeListener = reloadListener
        findPreference<EditTextPreference>(Key.DNS2)!!.onPreferenceChangeListener = reloadListener

        // openppp2 tunnel parameters
        findPreference<ListPreference>(Key.TUN_MUX)!!.onPreferenceChangeListener = reloadListener
        findPreference<EditTextPreference>(Key.TUN_MUX_ACCELERATION)!!.onPreferenceChangeListener = reloadListener
        findPreference<EditTextPreference>(Key.EXTRA_ARGS)!!.apply {
            setOnBindEditTextListener(EditTextPreferenceModifiers.Multiline)
            onPreferenceChangeListener = reloadListener
        }

        // inbound settings
        val requireSocks = findPreference<SwitchPreference>(Key.REQUIRE_SOCKS)!!

        val portSocks5 = findPreference<EditTextPreference>(Key.SOCKS_PORT)!!
        portSocks5.setOnBindEditTextListener(EditTextPreferenceModifiers.Port)
        portSocks5.isVisible = requireSocks.isChecked
        portSocks5.onPreferenceChangeListener = reloadListener
        val socks5UDP = findPreference<SwitchPreference>(Key.SOCKS_UDP)!!
        val socks5Username = findPreference<EditTextPreference>(Key.SOCKS_USERNAME)!!
        socks5Username.isVisible = requireSocks.isChecked
        socks5Username.setOnPreferenceChangeListener { _, newValue ->
            newValue as String
            if (newValue.isNotEmpty() && socks5UDP.isVisible && socks5UDP.isChecked && !DataStore.socksUDPWarningDisable) runOnMainDispatcher {
                MaterialAlertDialogBuilder(requireContext()).apply {
                    setMessage(R.string.socks5_udp_authentication_warning)
                    setPositiveButton(android.R.string.ok, null)
                    setNeutralButton(R.string.do_not_show_again, { _, _ ->
                        DataStore.socksUDPWarningDisable = true
                    })
                }.show()
            }
            needReload()
            true
        }
        val socks5Password = findPreference<EditTextPreference>(Key.SOCKS_PASSWORD)!!
        socks5Password.summaryProvider = ProfileSettingsActivity.PasswordSummaryProvider
        socks5Password.isVisible = requireSocks.isChecked
        socks5Password.setOnPreferenceChangeListener { _, newValue ->
            newValue as String
            if (newValue.isNotEmpty() && socks5UDP.isVisible && socks5UDP.isChecked && !DataStore.socksUDPWarningDisable) runOnMainDispatcher {
                MaterialAlertDialogBuilder(requireContext()).apply {
                    setMessage(R.string.socks5_udp_authentication_warning)
                    setPositiveButton(android.R.string.ok, null)
                    setNeutralButton(R.string.do_not_show_again, { _, _ ->
                        DataStore.socksUDPWarningDisable = true
                    })
                }.show()
            }
            needReload()
            true
        }
        socks5UDP.isVisible = requireSocks.isChecked
        socks5UDP.setOnPreferenceChangeListener { _, newValue ->
            newValue as Boolean
            if (newValue
                && ((socks5Username.isVisible && !socks5Username.text.isNullOrEmpty()) || (socks5Password.isVisible && !socks5Password.text.isNullOrEmpty()))
                && !DataStore.socksUDPWarningDisable) runOnMainDispatcher {
                MaterialAlertDialogBuilder(requireContext()).apply {
                    setMessage(R.string.socks5_udp_authentication_warning)
                    setPositiveButton(android.R.string.ok, null)
                    setNeutralButton(R.string.do_not_show_again, { _, _ ->
                        DataStore.socksUDPWarningDisable = true
                    })
                }.show()
            }
            needReload()
            true
        }
        requireSocks.setOnPreferenceChangeListener { _, newValue ->
            portSocks5.isVisible = newValue as Boolean
            socks5Username.isVisible = newValue
            socks5Password.isVisible = newValue
            socks5UDP.isVisible = newValue
            needReload()
            true
        }

        val portHttp = findPreference<EditTextPreference>(Key.HTTP_PORT)!!
        portHttp.setOnBindEditTextListener(EditTextPreferenceModifiers.Port)
        portHttp.isVisible = requireHttp.isChecked
        portHttp.onPreferenceChangeListener = reloadListener

        // LAN access rebinds the HTTP/SOCKS inbounds from loopback to
        // 0.0.0.0, which only takes effect when the VPN is (re)started.
        findPreference<SwitchPreference>(Key.ALLOW_ACCESS)!!.onPreferenceChangeListener = reloadListener
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            requireHttp.setOnPreferenceChangeListener { _, newValue ->
                portHttp.isVisible = newValue as Boolean
                needReload()
                true
            }
            appendHttpProxy.remove()
            httpProxyException.remove()
        } else {
            requireHttp.setOnPreferenceChangeListener { _, newValue ->
                portHttp.isVisible = newValue as Boolean
                appendHttpProxy.isVisible = newValue && serviceMode.value == MODE_VPN
                httpProxyException.isVisible = newValue && serviceMode.value == MODE_VPN
                        && appendHttpProxy.isVisible && appendHttpProxy.isChecked
                needReload()
                true
            }
            appendHttpProxy.isVisible = requireHttp.isChecked && serviceMode.value == MODE_VPN
            appendHttpProxy.setOnPreferenceChangeListener { _, newValue ->
                httpProxyException.isVisible = newValue as Boolean
                needReload()
                true
            }
            httpProxyException.isVisible = requireHttp.isChecked && serviceMode.value == MODE_VPN
                    && appendHttpProxy.isVisible && appendHttpProxy.isChecked
            httpProxyException.onPreferenceChangeListener = reloadListener
        }

        findPreference<EditTextPreference>(Key.PPROF_SERVER)!!.apply {
            isVisible = DataStore.enableDebug
            onPreferenceChangeListener = reloadListener
        }

        findPreference<EditTextPreference>(Key.EXPERIMENTAL_FLAGS)!!.isVisible = DataStore.enableDebug

        // misc settings
        findPreference<SwitchPreference>(Key.SHOW_GROUP_NAME)!!.onPreferenceChangeListener = reloadListener
        findPreference<SwitchPreference>(Key.ACQUIRE_WAKE_LOCK)!!.onPreferenceChangeListener = reloadListener
        findPreference<EditTextPreference>(Key.STUN_SERVERS)!!.setOnBindEditTextListener(EditTextPreferenceModifiers.Multiline)
        findPreference<ListPreference>(Key.FAB_STYLE)!!.setOnPreferenceChangeListener { _, _ ->
            requireActivity().apply {
                this.finish()
                startActivity(intent)
            }
            true
        }
    }


    override fun onResume() {
        super.onResume()

        if (::isProxyApps.isInitialized) {
            isProxyApps.isChecked = DataStore.proxyApps
        }
    }

}