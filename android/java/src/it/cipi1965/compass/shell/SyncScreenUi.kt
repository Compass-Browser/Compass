// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.text.format.DateFormat
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
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.input.clearText
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedSecureTextField
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.rememberTextFieldState
import java.util.Date

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

        /** Removes another device of the chain (docs/design/sync.md, "Removing a device"). */
        fun onRemoveDevice(guid: String)

        fun onRestart()

        fun onSetDataType(type: Int, enabled: Boolean)

        /** Asks for the screen lock, then fills SyncScreenState.recoveryWords. */
        fun onShowRecoveryCode()

        fun onHideRecoveryCode()

        /** "I saved it" under a new chain's words. */
        fun onNewWordsSaved()

        /** Company sign-in: signs the chain in, or opens the running sign-in's page again. */
        fun onStartLogin()

        fun onCancelLogin()

        // A free server on Cloudflare (docs/design/sync.md, "A free server on Cloudflare").

        /** Sets up a server with a pasted API token; [pushRelay] is the Phone notifications switch. */
        fun onStartOnCloudflare(token: String, pushRelay: Boolean)

        /** "Sign in with Cloudflare": the sign-in opens in a tab, then the setup goes on here. */
        fun onStartOnCloudflareSignIn(pushRelay: Boolean)

        /** "I have an invite" (docs/design/sync.md, "Inviting friends"). */
        fun onJoinWithInvite(link: String)

        // The owner's "People on your server".

        fun onInviteFriend(name: String)

        /** Removes a friend (their chain ID) or revokes an open invite (its code). */
        fun onRemoveFromServer(target: String)

        fun onSetServerPushRelay(on: Boolean)

        /** Copies or shares the invite link made last. */
        fun onCopyInviteLink(link: String)

        fun onShareInviteLink(link: String)

        /** Updates the server: with an API token, or (empty) after signing in with Cloudflare. */
        fun onUpdateServer(token: String)

        /** Opens a help page (Cloudflare's API Tokens) in a new tab. */
        fun onOpenPage(url: String)

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
            Login(state, listener)
            when (state.state) {
                "unavailable" -> Text("Sync isn't available in this profile.")
                "needs_restart" -> {
                    // The service's message says why ("Restart Compass to start syncing.").
                    if (state.message.isEmpty()) Text("Restart Compass to start syncing.")
                    Button(onClick = { listener.onRestart() }) { Text("Restart") }
                }
                "on", "starting", "error", "login_required" ->
                    if (state.removed) {
                        // The message says so; leaving (then joining again) is the way on.
                        OutlinedButton(onClick = { listener.onLeave() }) { Text("Leave the chain") }
                    } else {
                        Chain(state, listener)
                    }
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

// Company sign-in (docs/design/sync.md, "Company sign-in (OIDC)"): the server asks the chain to
// sign in; while a sign-in runs, the code its page shows.
@Composable
private fun Login(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    val provider = state.loginProvider
    if (state.loginInProgress) {
        Text(
            if (provider.isEmpty()) "Sign in" else "Sign in with $provider",
            style = MaterialTheme.typography.titleMedium,
        )
        if (state.loginCode.isNotEmpty()) {
            Text(
                state.loginCode,
                style = MaterialTheme.typography.displaySmall,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.SemiBold,
            )
        }
        Text("Sign in in the tab that opened. Allow only if the page shows this code.")
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedButton(
                onClick = { listener.onStartLogin() },
                enabled = state.loginCode.isNotEmpty(),
            ) {
                Text("Open the page")
            }
            OutlinedButton(onClick = { listener.onCancelLogin() }) { Text("Cancel") }
        }
        HorizontalDivider()
    } else if (state.state == "login_required") {
        Text(
            "This server only syncs for people of its organization: sign in with your work " +
                "account. Your data stays encrypted on your devices; signing in only lets this " +
                "chain use the server."
        )
        Button(onClick = { listener.onStartLogin() }) {
            Text(if (provider.isEmpty()) "Sign in" else "Sign in to $provider")
        }
        HorizontalDivider()
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
    HorizontalDivider()
    JoinWithInvite(state, listener)
    HorizontalDivider()
    Cloudflare(state, listener)
}

// docs/design/sync.md, "Inviting friends": a friend's server, a chain of one's own on it.
@Composable
private fun JoinWithInvite(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    var shown by remember { mutableStateOf(false) }
    if (!shown) {
        TextButton(onClick = { shown = true }) { Text("I have an invite") }
        return
    }
    val linkField = rememberTextFieldState()
    val link = linkField.text.toString().trim()
    Text("Join with an invite", style = MaterialTheme.typography.titleMedium)
    OutlinedTextField(
        state = linkField,
        label = { Text("Invite link") },
        placeholder = { Text("https://compass-sync.example.workers.dev/invite#…") },
        lineLimits = TextFieldLineLimits.SingleLine,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
        modifier = Modifier.fillMaxWidth(),
    )
    Text(
        "A friend who runs a Compass sync server sent it. You get a sync chain of your own on " +
            "their server; they can't read your data."
    )
    Button(
        onClick = { listener.onJoinWithInvite(link) },
        enabled = link.isNotEmpty() && state.serverSetup.isEmpty(),
    ) {
        Text("Join and start sync")
    }
}

// docs/design/sync.md, "A free server on Cloudflare": Compass sets one up in the user's own
// account, signing in in a Compass tab (or with a pasted API token).
@Composable
private fun Cloudflare(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    var shown by remember { mutableStateOf(false) }
    var showToken by remember { mutableStateOf(false) }
    // docs/design/sync.md, "Push on a Cloudflare server": on by default, as the desktop's.
    var pushRelay by remember { mutableStateOf(true) }
    val settingUp = state.serverSetup.isNotEmpty()
    if (!shown && !settingUp) {
        TextButton(onClick = { shown = true }) { Text("No server? Set up a free one on Cloudflare") }
        return
    }
    val tokenField = rememberTextFieldState()
    val token = tokenField.text.toString().trim()
    Text("A free server on Cloudflare", style = MaterialTheme.typography.titleMedium)
    Text(
        "Compass sets up a sync server in your own Cloudflare account. It's free, and only " +
            "your devices, and friends you invite, can use it."
    )
    SwitchRow(
        title = "Phone notifications",
        details = SETUP_PUSH_RELAY_TEXT,
        checked = pushRelay,
        enabled = !settingUp,
        onCheckedChange = { pushRelay = it },
    )
    Button(onClick = { listener.onStartOnCloudflareSignIn(pushRelay) }, enabled = !settingUp) {
        Text("Sign in with Cloudflare")
    }
    Text(
        "No Cloudflare account? You can make a free one on the sign-in page. Compass uses the " +
            "access only to set up the server, and doesn't keep it."
    )
    if (!showToken) {
        TextButton(onClick = { showToken = true }, enabled = !settingUp) {
            Text("Use an API token instead")
        }
    } else {
        Text("1. Open API Tokens, choose Create Token, and use the template Edit Cloudflare Workers.")
        TextButton(onClick = { listener.onOpenPage(CLOUDFLARE_TOKENS_URL) }) {
            Text("Open API Tokens")
        }
        Text("2. Paste the token here.")
        OutlinedSecureTextField(
            state = tokenField,
            label = { Text("Cloudflare API token") },
            modifier = Modifier.fillMaxWidth(),
        )
        Button(
            onClick = {
                listener.onStartOnCloudflare(token, pushRelay)
                tokenField.clearText()
            },
            enabled = token.isNotEmpty() && !settingUp,
        ) {
            Text("Set up and start sync")
        }
    }
    if (settingUp) Text(state.serverSetup)
}

private const val CLOUDFLARE_TOKENS_URL = "https://dash.cloudflare.com/profile/api-tokens"

private const val SETUP_PUSH_RELAY_TEXT =
    "When a tab is sent to an iPhone or Android phone, Compass's push relay " +
        "(push.compassbrowser.org) tells it at once. It gets each phone's notification token, " +
        "never your tabs or data. Off, sent tabs arrive when the phone next syncs. You can " +
        "change this later."

@Composable
private fun SwitchRow(
    title: String,
    details: String,
    checked: Boolean,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f).padding(end = 12.dp)) {
            Text(title)
            Text(
                details,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Switch(checked = checked, onCheckedChange = onCheckedChange, enabled = enabled)
    }
}

@Composable
private fun Chain(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    val user =
        if (state.loginUserName.isNotEmpty() && state.loginUserEmail.isNotEmpty()) {
            "${state.loginUserName} (${state.loginUserEmail})"
        } else {
            state.loginUserName.ifEmpty { state.loginUserEmail }
        }
    if (user.isNotEmpty()) Text("Signed in as $user.")
    Text("Devices", style = MaterialTheme.typography.titleMedium)
    // The device the Remove dialog asks about.
    var removing by remember { mutableStateOf<SyncScreenState.Device?>(null) }
    for (device in state.devices) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                device.name.ifEmpty { "Unnamed device" } + if (device.self) " (this device)" else "",
                modifier = Modifier.weight(1f),
            )
            if (!device.self) {
                TextButton(onClick = { removing = device }, enabled = !state.removing) {
                    Text("Remove")
                }
            }
        }
    }
    if (state.removeError.isNotEmpty()) {
        Text(state.removeError, color = MaterialTheme.colorScheme.error)
    }
    removing?.let { device ->
        AlertDialog(
            onDismissRequest = { removing = null },
            title = { Text("Remove ${device.name.ifEmpty { "Unnamed device" }}?") },
            text = { Text(REMOVE_DEVICE_TEXT) },
            confirmButton = {
                TextButton(
                    onClick = {
                        removing = null
                        listener.onRemoveDevice(device.guid)
                    }
                ) {
                    Text("Remove", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { removing = null }) { Text("Cancel") } },
        )
    }
    Button(onClick = { listener.onAddDevice() }) { Text("Add a device") }
    if (state.peopleAvailable) {
        HorizontalDivider()
        People(state, listener)
    }
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

// docs/design/sync.md, "Inviting friends": the friends and open invites of the server this chain
// owns (only on a server with an access list), its update and its phone notifications.
@Composable
private fun People(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    val context = LocalContext.current
    val dateFormat = remember { DateFormat.getMediumDateFormat(context) }
    Text("People on your server", style = MaterialTheme.typography.titleMedium)
    if (state.updateAvailable) ServerUpdate(state, listener)
    Text(
        "Only your devices and the friends you invite can sync on your server. Their data is " +
            "encrypted on their devices: you can't read it."
    )
    // The friend the Remove dialog asks about.
    var removing by remember { mutableStateOf<SyncScreenState.Member?>(null) }
    for (member in state.members) {
        PersonRow(
            title = member.name.ifEmpty { "Friend" },
            details = SyncScreenState.memberDetails(dateFormat.format(Date(member.addedMs)), member.linked),
            action = "Remove",
            onClick = { removing = member },
        )
    }
    for (invite in state.invites) {
        PersonRow(
            title = if (invite.name.isEmpty()) "Invite" else "Invite for ${invite.name}",
            details = "Not used yet · expires ${dateFormat.format(Date(invite.expiresMs))}",
            action = "Revoke",
            onClick = { listener.onRemoveFromServer(invite.code) },
        )
    }
    if (state.members.isEmpty() && state.invites.isEmpty()) Text("No one else yet.")
    removing?.let { member ->
        val title = member.name.ifEmpty { "Friend" }
        AlertDialog(
            onDismissRequest = { removing = null },
            title = { Text("Remove $title?") },
            text = { Text(REMOVE_FRIEND_TEXT) },
            confirmButton = {
                TextButton(
                    onClick = {
                        removing = null
                        listener.onRemoveFromServer(member.chain)
                    }
                ) {
                    Text("Remove", color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { removing = null }) { Text("Cancel") } },
        )
    }
    // docs/design/sync.md, "Push on a Cloudflare server": a server from before version 3 has no
    // such setting, and one with push off for good can't turn it on.
    if (state.pushRelayAvailable) {
        SwitchRow(
            title = "Phone notifications",
            details = SyncScreenState.pushRelayDetails(state.pushRelayHost),
            checked = state.pushRelay,
            enabled = !state.peopleBusy,
            onCheckedChange = { listener.onSetServerPushRelay(it) },
        )
    }
    val nameField = rememberTextFieldState()
    OutlinedTextField(
        state = nameField,
        label = { Text("Invite a friend") },
        placeholder = { Text("Their name (only you see it)") },
        lineLimits = TextFieldLineLimits.SingleLine,
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedButton(
        onClick = {
            listener.onInviteFriend(nameField.text.toString().trim())
            nameField.clearText()
        }
    ) {
        Text("Make an invite link")
    }
    if (state.inviteLink.isNotEmpty()) {
        Text(
            "Send this link to your friend. It works once, within 7 days. In Compass, they " +
                "choose Sync, then I have an invite."
        )
        SelectionContainer { Text(state.inviteLink, fontFamily = FontFamily.Monospace) }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(onClick = { listener.onShareInviteLink(state.inviteLink) }) { Text("Share") }
            OutlinedButton(onClick = { listener.onCopyInviteLink(state.inviteLink) }) {
                Text("Copy link")
            }
        }
    }
    if (state.peopleError.isNotEmpty()) Text(state.peopleError, color = MaterialTheme.colorScheme.error)
}

@Composable
private fun PersonRow(title: String, details: String, action: String, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title)
            Text(
                details,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        TextButton(onClick = onClick) { Text(action) }
    }
}

// docs/design/sync.md, "Updating a server": a server Compass set up on Cloudflare, older than the
// one this Compass ships.
@Composable
private fun ServerUpdate(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    var showToken by remember { mutableStateOf(false) }
    val tokenField = rememberTextFieldState()
    val token = tokenField.text.toString().trim()
    val updating = state.serverSetup.isNotEmpty()
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Text("A newer version of your server is ready.", fontWeight = FontWeight.SemiBold)
            Text(SyncScreenState.updateNote(state.updateCurrent, state.updateLatest))
            Button(onClick = { listener.onUpdateServer("") }, enabled = !updating) {
                Text("Update with Cloudflare")
            }
            if (!showToken) {
                TextButton(onClick = { showToken = true }, enabled = !updating) {
                    Text("Use an API token instead")
                }
            } else {
                OutlinedSecureTextField(
                    state = tokenField,
                    label = { Text("Cloudflare API token") },
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedButton(
                    onClick = {
                        listener.onUpdateServer(token)
                        tokenField.clearText()
                    },
                    enabled = token.isNotEmpty() && !updating,
                ) {
                    Text("Update")
                }
            }
            if (updating) Text(state.serverSetup)
            if (state.updateError.isNotEmpty()) {
                Text(state.updateError, color = MaterialTheme.colorScheme.error)
            }
        }
    }
}

private const val REMOVE_FRIEND_TEXT =
    "They stop syncing on your server within a minute. Their data stays on their devices. " +
        "You can invite them again."

private const val REMOVE_DEVICE_TEXT =
    "It stops syncing and disappears from your devices. What's on it stays there, " +
        "and it can join again with a pairing code. Someone who has your recovery code " +
        "can still join: to lock everyone out, use Reset sync on a computer."

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
