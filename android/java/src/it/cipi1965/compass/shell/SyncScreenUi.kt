// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.rememberTextFieldState

/** The sync screen: join a chain, pair devices, see what syncs (docs/design/sync.md). */
object SyncScreenUi {
    interface Listener {
        fun onJoinWithPairingCode(server: String, code: String)

        fun onJoinWithRecoveryCode(server: String, words: String)

        fun onStartNewChain(server: String)

        fun onAddDevice()

        fun onAnswerPairing(allow: Boolean)

        fun onCancelPairing()

        fun onLeave()

        fun onRestart()

        fun onSetDataType(type: Int, enabled: Boolean)

        /** Asks for the screen lock, then fills SyncScreenState.recoveryWords. */
        fun onShowRecoveryCode()

        fun onHideRecoveryCode()

        /** "I saved it" under a new chain's words. */
        fun onNewWordsSaved()

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: SyncScreenState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) { if (state.open) SyncScreen(state, listener) }
        }
        return view
    }
}

// SyncDataType's order (sync_controller.h).
private val TYPE_NAMES =
    listOf(
        "Passwords",
        "History",
        "Bookmarks",
        "Autofill",
        "Extensions",
        "Settings",
        "Reading list",
        "Open tabs",
        "Spaces",
    )

@Composable
private fun SyncScreen(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    BackHandler { listener.onDismiss() }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "Sync",
                    style = MaterialTheme.typography.headlineMedium,
                    modifier = Modifier.weight(1f),
                )
                TextButton(onClick = { listener.onDismiss() }) { Text("Done") }
            }
            if (state.error.isNotEmpty()) Text(state.error, color = MaterialTheme.colorScheme.error)
            if (state.message.isNotEmpty()) Text(state.message)
            if (state.newWords.isNotEmpty()) {
                // A new chain's words, alone until saved, as the desktop's #wordsPanel: then
                // Restart.
                NewWords(state, listener)
                return@Column
            }
            Pairing(state, listener)
            when (state.state) {
                "unavailable" -> Text("Sync isn't available in this profile.")
                "needs_restart" -> {
                    // The service's message says why ("Restart Compass to start syncing.").
                    if (state.message.isEmpty()) Text("Restart Compass to start syncing.")
                    Button(onClick = { listener.onRestart() }) { Text("Restart") }
                }
                "on", "starting", "error" -> Chain(state, listener)
                else -> Join(state, listener)
            }
        }
    }
}

@Composable
private fun Pairing(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    when (state.pairingStep) {
        "working" -> Text("Pairing…")
        "waiting" -> {
            Text("Approve this device on your other device.")
            OutlinedButton(onClick = { listener.onCancelPairing() }) { Text("Cancel") }
        }
        "showing_code" -> {
            Text("On your other device, choose Join and enter:")
            Text(
                state.pairingCode,
                style = MaterialTheme.typography.displaySmall,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.SemiBold,
            )
            OutlinedButton(onClick = { listener.onCancelPairing() }) { Text("Cancel") }
        }
        "confirm" -> {
            Text("Let “${state.pairingDevice}” join this chain?")
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = { listener.onAnswerPairing(true) }) { Text("Allow") }
                OutlinedButton(onClick = { listener.onAnswerPairing(false) }) { Text("Deny") }
            }
        }
        "failed" -> Text(state.pairingMessage.ifEmpty { "Pairing failed." }, color = MaterialTheme.colorScheme.error)
        else -> {}
    }
}

@Composable
private fun Join(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    val serverField = rememberTextFieldState(state.server)
    val codeField = rememberTextFieldState()
    val wordsField = rememberTextFieldState()
    val server = serverField.text.toString().trim()
    val code = codeField.text.toString()
    val words = wordsField.text.toString()
    Text(
        "Sync joins a chain of your devices: everything is encrypted on them, " +
            "and the server can't read it. There's no account."
    )
    OutlinedTextField(
        state = serverField,
        label = { Text("Sync server") },
        placeholder = { Text("https://…") },
        lineLimits = TextFieldLineLimits.SingleLine,
        modifier = Modifier.fillMaxWidth(),
    )
    HorizontalDivider()
    Text("Pair with a device", style = MaterialTheme.typography.titleMedium)
    Text("On a device that syncs, choose Add a device, then enter its code here.")
    OutlinedTextField(
        state = codeField,
        label = { Text("Pairing code") },
        lineLimits = TextFieldLineLimits.SingleLine,
        modifier = Modifier.fillMaxWidth(),
    )
    Button(
        onClick = { listener.onJoinWithPairingCode(server, code.trim()) },
        enabled = server.isNotEmpty() && code.isNotBlank(),
    ) {
        Text("Join")
    }
    HorizontalDivider()
    Text("Or use your recovery code", style = MaterialTheme.typography.titleMedium)
    OutlinedTextField(
        state = wordsField,
        label = { Text("Recovery code") },
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedButton(
        onClick = { listener.onJoinWithRecoveryCode(server, words.trim()) },
        enabled = server.isNotEmpty() && words.isNotBlank(),
    ) {
        Text("Join with recovery code")
    }
    HorizontalDivider()
    TextButton(onClick = { listener.onStartNewChain(server) }, enabled = server.isNotEmpty()) {
        Text("Start a new chain")
    }
}

@Composable
private fun Chain(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    Text("Devices", style = MaterialTheme.typography.titleMedium)
    for (device in state.devices) {
        Text(device.name + if (device.self) " (this device)" else "")
    }
    Button(onClick = { listener.onAddDevice() }) { Text("Add a device") }
    HorizontalDivider()
    Text("What syncs", style = MaterialTheme.typography.titleMedium)
    for (type in state.types) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(TYPE_NAMES.getOrElse(type.type) { "?" }, modifier = Modifier.weight(1f))
            Switch(checked = type.enabled, onCheckedChange = { listener.onSetDataType(type.type, it) })
        }
    }
    HorizontalDivider()
    RecoveryCode(state, listener)
    HorizontalDivider()
    OutlinedButton(onClick = { listener.onLeave() }) { Text("Leave the chain") }
}

private const val SAVE_WORDS_TEXT =
    "Save these 12 words somewhere safe, such as a password manager. " +
        "You only need them if you lose every device that syncs: without " +
        "them, the synced data is gone."

@Composable
private fun WordList(words: List<String>) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        words.forEachIndexed { i, word -> Text("${i + 1}. $word", fontFamily = FontFamily.Monospace) }
    }
}

/** A new chain's words, shown once (no screen lock, as the desktop's "Start sync"). */
@Composable
private fun NewWords(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    Text("Your recovery code", style = MaterialTheme.typography.titleMedium)
    Text(SAVE_WORDS_TEXT)
    WordList(state.newWords)
    Button(onClick = { listener.onNewWordsSaved() }) { Text("I saved it") }
}

/** As the desktop's Recovery code section: behind the screen lock, the window secure meanwhile. */
@Composable
private fun RecoveryCode(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    Text("Recovery code", style = MaterialTheme.typography.titleMedium)
    if (state.recoveryWords.isEmpty()) {
        Text("With it, any device can join this chain. Compass asks for your screen lock first.")
        if (state.recoveryError.isNotEmpty()) {
            Text(state.recoveryError, color = MaterialTheme.colorScheme.error)
        }
        OutlinedButton(onClick = { listener.onShowRecoveryCode() }) { Text("Show recovery code") }
        return
    }
    Text(SAVE_WORDS_TEXT)
    if (state.recoveryUnprotected) {
        Text(
            "This device has no screen lock, so Compass didn't ask for one. " +
                "Anyone holding it can see these words.",
            color = MaterialTheme.colorScheme.error,
        )
    }
    WordList(state.recoveryWords)
    Button(onClick = { listener.onHideRecoveryCode() }) { Text("Hide") }
}
