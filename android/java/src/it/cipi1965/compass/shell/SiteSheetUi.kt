// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R

/**
 * The page's site, Compass's: in place of Chrome's page info (docs/design/android.md, "The Site
 * sheet"). Its connection, the permissions it has been given or denied, its own settings (as
 * iOS's Site Settings: desktop site, sound, pop-ups, JavaScript), and Chrome's page for the rest.
 */
object SiteSheetUi {
    interface Listener {
        fun onToggle(key: Int, on: Boolean)

        fun onResetPermissions()

        fun onAllSettings()

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: SiteSheetState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                CompassSheet(state.open, onDismiss = { listener.onDismiss() }) { SiteSheet(state, listener) }
            }
        }
        return view
    }
}

@Composable
private fun SiteSheet(state: SiteSheetState, listener: SiteSheetUi.Listener) {
    Column(
        Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(start = 16.dp, end = 16.dp, bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            SiteTile(state.url)
            Text(
                state.host.removePrefix("www."),
                style = MaterialTheme.typography.titleLarge,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
        }
        Card { ConnectionRow(state.connection) }
        if (state.permissions.isNotEmpty()) {
            CardSection(stringResource(R.string.compass_site_permissions)) {
                for (setting in state.permissions) SettingRow(setting, listener)
                CardRow(stringResource(R.string.compass_site_reset_permissions), onClick = { listener.onResetPermissions() }) {
                    MenuIcon(R.drawable.compass_sym_restart_alt)
                }
            }
        }
        CardSection(stringResource(R.string.compass_site_this_site)) { for (setting in state.options) SettingRow(setting, listener) }
        Card {
            CardRow(
                stringResource(R.string.compass_site_all_settings),
                subtitle = stringResource(R.string.compass_site_all_settings_subtitle),
                trailing = R.drawable.compass_sym_chevron_right,
                onClick = { listener.onAllSettings() },
            ) {
                MenuIcon(R.drawable.compass_sym_tune)
            }
        }
    }
}

@Composable
private fun ConnectionRow(connection: SiteSheetState.Connection) {
    val (title, subtitle, icon) =
        when (connection) {
            SiteSheetState.Connection.SECURE ->
                Triple(
                    stringResource(R.string.compass_site_secure),
                    stringResource(R.string.compass_site_secure_subtitle),
                    R.drawable.compass_sym_lock,
                )
            SiteSheetState.Connection.DANGEROUS ->
                Triple(
                    stringResource(R.string.compass_site_dangerous),
                    stringResource(R.string.compass_site_no_passwords),
                    R.drawable.compass_sym_warning,
                )
            SiteSheetState.Connection.NOT_SECURE ->
                Triple(
                    stringResource(R.string.compass_site_not_secure),
                    stringResource(R.string.compass_site_no_passwords),
                    R.drawable.compass_sym_no_encryption,
                )
            SiteSheetState.Connection.NONE -> Triple(stringResource(R.string.compass_site_compass_page), "", R.drawable.compass_sym_info)
        }
    val alert = connection == SiteSheetState.Connection.DANGEROUS
    Row(
        Modifier.fillMaxWidth().heightIn(min = 72.dp).padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        MenuIcon(icon)
        Column(Modifier.weight(1f)) {
            Text(
                title,
                style = MaterialTheme.typography.bodyLarge,
                color = if (alert) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface,
            )
            if (subtitle.isNotEmpty()) {
                Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}

@Composable
private fun SettingRow(setting: SiteSheetState.Setting, listener: SiteSheetUi.Listener) {
    Row(
        Modifier.fillMaxWidth()
            .heightIn(min = 56.dp)
            .then(
                if (setting.enabled) Modifier.clickable { listener.onToggle(setting.key, !setting.on) }
                else Modifier.alpha(0.38f)
            )
            .padding(horizontal = 16.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        MenuIcon(setting.icon)
        Column(Modifier.weight(1f)) {
            Text(setting.title, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurface)
            Text(
                if (setting.on) setting.onText else setting.offText,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Switch(
            checked = setting.on,
            onCheckedChange = { listener.onToggle(setting.key, it) },
            enabled = setting.enabled,
        )
    }
}

