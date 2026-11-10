// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.annotation.DrawableRes
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R

/**
 * Live folders' sheet (docs/design/live-folders.md, "UI"), a Material bottom sheet: New Live Folder
 * (the plugins, then the chosen one's form), Edit Source, and installing a plugin from its
 * manifest's address after a review of what it reaches and asks for. LiveFolders.java runs it.
 */
object LiveFoldersUi {
    interface Listener {
        /** Creates the folder, or saves the edited one's source. */
        fun onSave()

        /** Fetches the plugin at |url| for review. */
        fun onReview(url: String)

        /** Installs the reviewed plugin. */
        fun onInstall()

        fun onRemovePlugin(pluginId: String)

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: LiveFoldersState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                CompassSheet(state.open, onDismiss = { listener.onDismiss() }) { LiveFolderSheet(state, listener) }
            }
        }
        return view
    }

    /** A plugin's icon: a Material Symbol for the built-ins, a bolt for the others. */
    @JvmStatic
    @DrawableRes
    fun pluginIcon(pluginId: String): Int =
        when (pluginId) {
            "compass.rss" -> R.drawable.compass_sym_rss_feed
            "compass.github" -> R.drawable.compass_sym_code
            "compass.calendar" -> R.drawable.compass_sym_event
            "compass.web-list" -> R.drawable.compass_sym_format_list_bulleted
            else -> R.drawable.compass_sym_bolt
        }
}

@Composable
private fun LiveFolderSheet(state: LiveFoldersState, listener: LiveFoldersUi.Listener) {
    Column(
        Modifier.fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .imePadding()
            .padding(start = 16.dp, end = 16.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Header(state)
        when {
            state.installing -> InstallPage(state, listener)
            state.plugin != null -> FormPage(state, state.plugin!!, listener)
            state.editing -> MissingPluginPage(state)
            else -> PickerPage(state, listener)
        }
    }
}

/** The title, with a back arrow from a plugin's form or the install page to the plugins. */
@Composable
private fun Header(state: LiveFoldersState) {
    val back = state.installing || (!state.editing && state.plugin != null)
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        if (back) {
            IconButton(onClick = {
                if (state.installing) {
                    state.installing = false
                    state.preview = null
                    state.error = ""
                } else {
                    state.pick(null)
                }
            }) {
                Sym(R.drawable.compass_sym_arrow_back, 18f)
            }
        }
        Text(state.heading(), style = MaterialTheme.typography.titleLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun ErrorText(error: String) {
    if (error.isNotEmpty()) {
        Text(error, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error)
    }
}

/** The plugins, and installing one. */
@Composable
private fun PickerPage(state: LiveFoldersState, listener: LiveFoldersUi.Listener) {
    Text(
        "A live folder fills itself from a source and keeps up with it. Pick where its entries come from.",
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    if (state.loading) {
        Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) {
            CircularProgressIndicator(Modifier.size(28.dp), strokeWidth = 3.dp)
        }
        return
    }
    Card {
        for (plugin in state.plugins) key(plugin.id) {
            var menu by remember { mutableStateOf(false) }
            Box {
                CardRow(
                    title = plugin.name,
                    subtitle = plugin.description.ifEmpty { if (plugin.builtin) "Built in" else "Installed" },
                    onClick = { state.pick(plugin) },
                ) { PluginTile(plugin.id) }
                // An installed plugin can go (a local one goes with its folder).
                if (!plugin.builtin && !plugin.local) {
                    Box(Modifier.align(Alignment.CenterEnd).padding(end = 4.dp)) {
                        IconButton(onClick = { menu = true }) {
                            Sym(R.drawable.compass_sym_more_horiz, 16f, MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                            MenuItem("Remove Plugin", R.drawable.compass_sym_delete, destructive = true) {
                                menu = false
                                listener.onRemovePlugin(plugin.id)
                            }
                        }
                    }
                }
            }
        }
        CardRow(
            title = "Install a Plugin…",
            subtitle = "From the address of its manifest",
            onClick = {
                state.installing = true
                state.installUrl = ""
                state.preview = null
                state.error = ""
            },
        ) { MenuIcon(R.drawable.compass_sym_download) }
    }
    ErrorText(state.error)
}

@Composable
private fun PluginTile(pluginId: String) {
    Box(
        Modifier.size(40.dp).clip(CircleShape).background(MaterialTheme.colorScheme.secondaryContainer),
        contentAlignment = Alignment.Center,
    ) {
        Sym(LiveFoldersUi.pluginIcon(pluginId), 18f, MaterialTheme.colorScheme.onSecondaryContainer)
    }
}

/** Edit Source on a folder whose plugin this device doesn't have: install it from where it came. */
@Composable
private fun MissingPluginPage(state: LiveFoldersState) {
    if (state.loading) {
        Box(Modifier.fillMaxWidth().padding(24.dp), contentAlignment = Alignment.Center) {
            CircularProgressIndicator(Modifier.size(28.dp), strokeWidth = 3.dp)
        }
        return
    }
    ErrorText(state.error)
    if (state.missingPluginUrl.isNotEmpty()) {
        Button(onClick = {
            state.installing = true
            state.installUrl = state.missingPluginUrl
            state.preview = null
            state.error = ""
        }) { Text("Install Plugin…") }
    }
}

/** A plugin's address, then what it reaches and asks for, before it's installed. */
@Composable
private fun InstallPage(state: LiveFoldersState, listener: LiveFoldersUi.Listener) {
    OutlinedTextField(
        value = state.installUrl,
        onValueChange = {
            state.installUrl = it
            state.preview = null
        },
        label = { Text("Manifest address") },
        placeholder = { Text("https://example.com/plugin/manifest.json") },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
        modifier = Modifier.fillMaxWidth(),
    )
    val preview = state.preview
    if (preview == null) {
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Spacer(Modifier.weight(1f))
            Button(onClick = { listener.onReview(state.installUrl.trim()) }, enabled = state.installUrl.isNotBlank() && !state.saving) {
                Text("Review")
            }
        }
        ErrorText(state.error)
        return
    }
    Card {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                PluginTile(preview.id)
                Column {
                    Text(preview.name, style = MaterialTheme.typography.titleMedium)
                    Text(
                        "Version ${preview.version}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            if (preview.description.isNotEmpty()) Text(preview.description, style = MaterialTheme.typography.bodyMedium)
            ReviewLine("Reaches", LiveFoldersState.describeHosts(preview.hosts))
            if (preview.cookieHosts.isNotEmpty()) {
                ReviewLine("Can ask for your cookies on", LiveFoldersState.describeHosts(preview.cookieHosts))
            }
            if (preview.secrets.isNotEmpty()) ReviewLine("Asks for", preview.secrets.joinToString(", "))
        }
    }
    ErrorText(state.error)
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        Spacer(Modifier.weight(1f))
        TextButton(onClick = {
            state.installing = false
            state.preview = null
            state.error = ""
        }) { Text("Cancel") }
        Button(onClick = { listener.onInstall() }, enabled = !state.saving) { Text("Install") }
    }
}

@Composable
private fun ReviewLine(label: String, value: String) {
    Column {
        Text(label, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, style = MaterialTheme.typography.bodyMedium)
    }
}

/** The folder's name, the plugin's config fields and secrets, and the cookie grant. */
@Composable
private fun FormPage(state: LiveFoldersState, plugin: LiveFoldersState.Plugin, listener: LiveFoldersUi.Listener) {
    if (!state.editing && plugin.description.isNotEmpty()) {
        Text(plugin.description, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    OutlinedTextField(
        value = state.title,
        onValueChange = { state.title = it },
        label = { Text("Name (optional)") },
        placeholder = { Text(plugin.name) },
        singleLine = true,
        modifier = Modifier.fillMaxWidth(),
    )
    for (field in plugin.config) ConfigField(state, field)
    for (secret in plugin.secrets) {
        OutlinedTextField(
            value = state.secrets[secret.key] ?: "",
            onValueChange = { state.secrets[secret.key] = it },
            label = { Text(secret.label + if (secret.optional) " (optional)" else "") },
            placeholder = { if (secret.set) Text("Saved on this device") },
            supportingText = {
                Text(
                    listOf(secret.help, "Kept on this device only, never synced.")
                        .filter { it.isNotEmpty() }
                        .joinToString(" ")
                )
            },
            singleLine = true,
            visualTransformation = PasswordVisualTransformation(),
            // A password keyboard: no suggestions, no learning.
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password),
            modifier = Modifier.fillMaxWidth(),
        )
    }
    if (plugin.cookieHosts.isNotEmpty()) {
        SwitchRow(
            "Use my cookies for ${LiveFoldersState.describeHosts(plugin.cookieHosts)}",
            "Lets the plugin see what you see there when signed in. Only for this folder, on this device.",
            state.grantCookies,
        ) { state.grantCookies = it }
    }
    ErrorText(state.error)
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        if (state.saving) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
        Spacer(Modifier.weight(1f))
        TextButton(onClick = { listener.onDismiss() }) { Text("Cancel") }
        Button(onClick = { listener.onSave() }, enabled = !state.saving) {
            Text(if (state.editing) "Save" else "Create")
        }
    }
}

@Composable
private fun ConfigField(state: LiveFoldersState, field: LiveFoldersState.Field) {
    val label = field.label + if (field.optional) " (optional)" else ""
    when (field.type) {
        "bool" -> SwitchRow(label, "", state.value(field) == "true") { state.values[field.key] = if (it) "true" else "false" }
        "select" -> SelectField(state, field, label)
        else ->
            OutlinedTextField(
                value = state.value(field),
                onValueChange = { state.values[field.key] = it },
                label = { Text(label) },
                placeholder = { if (field.placeholder.isNotEmpty()) Text(field.placeholder) },
                singleLine = true,
                keyboardOptions =
                    KeyboardOptions(
                        keyboardType =
                            when (field.type) {
                                "url" -> KeyboardType.Uri
                                "number" -> KeyboardType.Decimal
                                else -> KeyboardType.Text
                            }
                    ),
                modifier = Modifier.fillMaxWidth(),
            )
    }
}

/** A select field: a read-only field that opens a menu of its options. */
@Composable
private fun SelectField(state: LiveFoldersState, field: LiveFoldersState.Field, label: String) {
    var expanded by remember { mutableStateOf(false) }
    val value = state.value(field)
    val shown = field.options.firstOrNull { it.value == value }?.label ?: value
    Box(Modifier.fillMaxWidth()) {
        OutlinedTextField(
            value = shown,
            onValueChange = {},
            readOnly = true,
            label = { Text(label) },
            trailingIcon = {
                Sym(
                    R.drawable.compass_sym_chevron_right,
                    14f,
                    MaterialTheme.colorScheme.onSurfaceVariant,
                    Modifier.rotate(if (expanded) 270f else 90f),
                )
            },
            singleLine = true,
            modifier = Modifier.fillMaxWidth(),
        )
        // Over the field: a tap opens the menu (a read-only field still takes focus).
        Box(Modifier.matchParentSize().clickable { expanded = true })
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            for (option in field.options) {
                DropdownMenuItem(
                    text = { Text(option.label.ifEmpty { option.value }) },
                    onClick = {
                        state.values[field.key] = option.value
                        expanded = false
                    },
                )
            }
        }
    }
}

@Composable
private fun SwitchRow(title: String, subtitle: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = 56.dp).clickable { onChange(!checked) },
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            if (subtitle.isNotEmpty()) {
                Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

/** A live folder's state in its sidebar row: refreshing, failed, or new entries while collapsed. */
@Composable
internal fun LiveFolderBadge(node: SidebarState.Node) {
    when {
        node.refreshing -> CircularProgressIndicator(Modifier.size(14.dp), strokeWidth = 2.dp)
        node.error.isNotEmpty() -> Sym(R.drawable.compass_sym_error, 13f, MaterialTheme.colorScheme.error)
        node.collapsed && node.unseenCount > 0 ->
            Box(
                Modifier.heightIn(min = 18.dp)
                    .clip(CircleShape)
                    .background(MaterialTheme.colorScheme.primary)
                    .padding(horizontal = 6.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    node.unseenCount.toString(),
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onPrimary,
                )
            }
        else -> Spacer(Modifier.width(0.dp))
    }
}
