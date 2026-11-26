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
import androidx.compose.ui.res.stringResource
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
import it.cipi1965.compass.R
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
        R.string.compass_sync_type_passwords,
        R.string.compass_sync_type_history,
        R.string.compass_sync_type_bookmarks,
        R.string.compass_sync_type_autofill,
        R.string.compass_sync_type_extensions,
        R.string.compass_sync_type_settings,
        R.string.compass_sync_type_reading_list,
        R.string.compass_sync_type_open_tabs,
        R.string.compass_sync_type_spaces,
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
                    stringResource(R.string.compass_sync_title),
                    style = MaterialTheme.typography.headlineMedium,
                    modifier = Modifier.weight(1f),
                )
                TextButton(onClick = { listener.onDismiss() }) { Text(stringResource(R.string.compass_done)) }
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
                "unavailable" -> Text(stringResource(R.string.compass_sync_unavailable))
                "needs_restart" -> {
                    // The service's message says why ("Restart Compass to start syncing.").
                    if (state.message.isEmpty()) Text(stringResource(R.string.compass_sync_restart_to_start))
                    Button(onClick = { listener.onRestart() }) { Text(stringResource(R.string.compass_sync_restart)) }
                }
                "on", "starting", "error", "login_required" ->
                    if (state.removed) {
                        // The message says so; leaving (then joining again) is the way on.
                        OutlinedButton(onClick = { listener.onLeave() }) { Text(stringResource(R.string.compass_sync_leave_chain)) }
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
        "working" -> Text(stringResource(R.string.compass_sync_pairing))
        "waiting" -> {
            Text(stringResource(R.string.compass_sync_approve_on_other))
            OutlinedButton(onClick = { listener.onCancelPairing() }) { Text(stringResource(R.string.compass_cancel)) }
        }
        "showing_code" -> {
            Text(stringResource(R.string.compass_sync_enter_code_on_other))
            Text(
                state.pairingCode,
                style = MaterialTheme.typography.displaySmall,
                fontFamily = FontFamily.Monospace,
                fontWeight = FontWeight.SemiBold,
            )
            OutlinedButton(onClick = { listener.onCancelPairing() }) { Text(stringResource(R.string.compass_cancel)) }
        }
        "confirm" -> {
            Text(stringResource(R.string.compass_sync_let_join, state.pairingDevice))
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = { listener.onAnswerPairing(true) }) { Text(stringResource(R.string.compass_sync_allow)) }
                OutlinedButton(onClick = { listener.onAnswerPairing(false) }) { Text(stringResource(R.string.compass_sync_deny)) }
            }
        }
        "failed" ->
            Text(
                state.pairingMessage.ifEmpty { stringResource(R.string.compass_sync_pairing_failed) },
                color = MaterialTheme.colorScheme.error,
            )
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
            if (provider.isEmpty()) {
                stringResource(R.string.compass_sync_sign_in)
            } else {
                stringResource(R.string.compass_sync_sign_in_with, provider)
            },
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
        Text(stringResource(R.string.compass_sync_sign_in_in_tab))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedButton(
                onClick = { listener.onStartLogin() },
                enabled = state.loginCode.isNotEmpty(),
            ) {
                Text(stringResource(R.string.compass_sync_open_page))
            }
            OutlinedButton(onClick = { listener.onCancelLogin() }) { Text(stringResource(R.string.compass_cancel)) }
        }
        HorizontalDivider()
    } else if (state.state == "login_required") {
        Text(stringResource(R.string.compass_sync_login_required))
        Button(onClick = { listener.onStartLogin() }) {
            Text(
                if (provider.isEmpty()) {
                    stringResource(R.string.compass_sync_sign_in)
                } else {
                    stringResource(R.string.compass_sync_sign_in_to, provider)
                }
            )
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
    Text(stringResource(R.string.compass_sync_intro))
    OutlinedTextField(
        state = serverField,
        label = { Text(stringResource(R.string.compass_sync_server)) },
        placeholder = { Text("https://…") },
        lineLimits = TextFieldLineLimits.SingleLine,
        modifier = Modifier.fillMaxWidth(),
    )
    HorizontalDivider()
    Text(stringResource(R.string.compass_sync_pair_with_device), style = MaterialTheme.typography.titleMedium)
    Text(stringResource(R.string.compass_sync_pair_instructions))
    OutlinedTextField(
        state = codeField,
        label = { Text(stringResource(R.string.compass_sync_pairing_code)) },
        lineLimits = TextFieldLineLimits.SingleLine,
        modifier = Modifier.fillMaxWidth(),
    )
    Button(
        onClick = { listener.onJoinWithPairingCode(server, code.trim()) },
        enabled = server.isNotEmpty() && code.isNotBlank(),
    ) {
        Text(stringResource(R.string.compass_sync_join))
    }
    HorizontalDivider()
    Text(stringResource(R.string.compass_sync_or_recovery_code), style = MaterialTheme.typography.titleMedium)
    OutlinedTextField(
        state = wordsField,
        label = { Text(stringResource(R.string.compass_sync_recovery_code)) },
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedButton(
        onClick = { listener.onJoinWithRecoveryCode(server, words.trim()) },
        enabled = server.isNotEmpty() && words.isNotBlank(),
    ) {
        Text(stringResource(R.string.compass_sync_join_with_recovery_code))
    }
    HorizontalDivider()
    TextButton(onClick = { listener.onStartNewChain(server) }, enabled = server.isNotEmpty()) {
        Text(stringResource(R.string.compass_sync_start_new_chain))
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
        TextButton(onClick = { shown = true }) { Text(stringResource(R.string.compass_sync_have_invite)) }
        return
    }
    val linkField = rememberTextFieldState()
    val link = linkField.text.toString().trim()
    Text(stringResource(R.string.compass_sync_join_with_invite), style = MaterialTheme.typography.titleMedium)
    OutlinedTextField(
        state = linkField,
        label = { Text(stringResource(R.string.compass_sync_invite_link)) },
        placeholder = { Text("https://compass-sync.example.workers.dev/invite#…") },
        lineLimits = TextFieldLineLimits.SingleLine,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
        modifier = Modifier.fillMaxWidth(),
    )
    Text(stringResource(R.string.compass_sync_invite_explained))
    Button(
        onClick = { listener.onJoinWithInvite(link) },
        enabled = link.isNotEmpty() && state.serverSetup.isEmpty(),
    ) {
        Text(stringResource(R.string.compass_sync_join_and_start))
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
        TextButton(onClick = { shown = true }) { Text(stringResource(R.string.compass_sync_no_server)) }
        return
    }
    val tokenField = rememberTextFieldState()
    val token = tokenField.text.toString().trim()
    Text(stringResource(R.string.compass_sync_cloudflare_title), style = MaterialTheme.typography.titleMedium)
    Text(stringResource(R.string.compass_sync_cloudflare_intro))
    SwitchRow(
        title = stringResource(R.string.compass_sync_phone_notifications),
        details = stringResource(R.string.compass_sync_setup_push_relay),
        checked = pushRelay,
        enabled = !settingUp,
        onCheckedChange = { pushRelay = it },
    )
    Button(onClick = { listener.onStartOnCloudflareSignIn(pushRelay) }, enabled = !settingUp) {
        Text(stringResource(R.string.compass_sync_sign_in_with_cloudflare))
    }
    Text(stringResource(R.string.compass_sync_no_cloudflare_account))
    if (!showToken) {
        TextButton(onClick = { showToken = true }, enabled = !settingUp) {
            Text(stringResource(R.string.compass_sync_use_api_token))
        }
    } else {
        Text(stringResource(R.string.compass_sync_token_step_1))
        TextButton(onClick = { listener.onOpenPage(CLOUDFLARE_TOKENS_URL) }) {
            Text(stringResource(R.string.compass_sync_open_api_tokens))
        }
        Text(stringResource(R.string.compass_sync_token_step_2))
        OutlinedSecureTextField(
            state = tokenField,
            label = { Text(stringResource(R.string.compass_sync_cloudflare_api_token)) },
            modifier = Modifier.fillMaxWidth(),
        )
        Button(
            onClick = {
                listener.onStartOnCloudflare(token, pushRelay)
                tokenField.clearText()
            },
            enabled = token.isNotEmpty() && !settingUp,
        ) {
            Text(stringResource(R.string.compass_sync_set_up_and_start))
        }
    }
    if (settingUp) Text(state.serverSetup)
}

private const val CLOUDFLARE_TOKENS_URL = "https://dash.cloudflare.com/profile/api-tokens"

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
    if (user.isNotEmpty()) Text(stringResource(R.string.compass_sync_signed_in_as, user))
    Text(stringResource(R.string.compass_sync_devices), style = MaterialTheme.typography.titleMedium)
    // The device the Remove dialog asks about.
    var removing by remember { mutableStateOf<SyncScreenState.Device?>(null) }
    for (device in state.devices) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            val name = device.name.ifEmpty { stringResource(R.string.compass_sync_unnamed_device) }
            Text(
                if (device.self) stringResource(R.string.compass_sync_this_device, name) else name,
                modifier = Modifier.weight(1f),
            )
            if (!device.self) {
                TextButton(onClick = { removing = device }, enabled = !state.removing) {
                    Text(stringResource(R.string.compass_remove))
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
            title = {
                Text(
                    stringResource(
                        R.string.compass_sync_remove_named,
                        device.name.ifEmpty { stringResource(R.string.compass_sync_unnamed_device) },
                    )
                )
            },
            text = { Text(stringResource(R.string.compass_sync_remove_device_text)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        removing = null
                        listener.onRemoveDevice(device.guid)
                    }
                ) {
                    Text(stringResource(R.string.compass_remove), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { removing = null }) { Text(stringResource(R.string.compass_cancel)) } },
        )
    }
    Button(onClick = { listener.onAddDevice() }) { Text(stringResource(R.string.compass_sync_add_device)) }
    if (state.peopleAvailable) {
        HorizontalDivider()
        People(state, listener)
    }
    HorizontalDivider()
    Text(stringResource(R.string.compass_sync_what_syncs), style = MaterialTheme.typography.titleMedium)
    for (type in state.types) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                TYPE_NAMES.getOrNull(type.type)?.let { stringResource(it) } ?: "?",
                modifier = Modifier.weight(1f),
            )
            Switch(checked = type.enabled, onCheckedChange = { listener.onSetDataType(type.type, it) })
        }
    }
    HorizontalDivider()
    RecoveryCode(state, listener)
    HorizontalDivider()
    OutlinedButton(onClick = { listener.onLeave() }) { Text(stringResource(R.string.compass_sync_leave_chain)) }
}

// docs/design/sync.md, "Inviting friends": the friends and open invites of the server this chain
// owns (only on a server with an access list), its update and its phone notifications.
@Composable
private fun People(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    val context = LocalContext.current
    val dateFormat = remember { DateFormat.getMediumDateFormat(context) }
    Text(stringResource(R.string.compass_sync_people_on_server), style = MaterialTheme.typography.titleMedium)
    if (state.updateAvailable) ServerUpdate(state, listener)
    Text(stringResource(R.string.compass_sync_people_intro))
    // The friend the Remove dialog asks about.
    var removing by remember { mutableStateOf<SyncScreenState.Member?>(null) }
    for (member in state.members) {
        PersonRow(
            title = member.name.ifEmpty { stringResource(R.string.compass_sync_friend) },
            details =
                SyncScreenState.memberDetails(
                    context.resources,
                    dateFormat.format(Date(member.addedMs)),
                    member.linked,
                ),
            action = stringResource(R.string.compass_remove),
            onClick = { removing = member },
        )
    }
    for (invite in state.invites) {
        PersonRow(
            title =
                if (invite.name.isEmpty()) {
                    stringResource(R.string.compass_sync_invite)
                } else {
                    stringResource(R.string.compass_sync_invite_for, invite.name)
                },
            details =
                stringResource(
                    R.string.compass_sync_invite_unused,
                    dateFormat.format(Date(invite.expiresMs)),
                ),
            action = stringResource(R.string.compass_sync_revoke),
            onClick = { listener.onRemoveFromServer(invite.code) },
        )
    }
    if (state.members.isEmpty() && state.invites.isEmpty()) Text(stringResource(R.string.compass_sync_no_one_else))
    removing?.let { member ->
        val title = member.name.ifEmpty { stringResource(R.string.compass_sync_friend) }
        AlertDialog(
            onDismissRequest = { removing = null },
            title = { Text(stringResource(R.string.compass_sync_remove_named, title)) },
            text = { Text(stringResource(R.string.compass_sync_remove_friend_text)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        removing = null
                        listener.onRemoveFromServer(member.chain)
                    }
                ) {
                    Text(stringResource(R.string.compass_remove), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = { TextButton(onClick = { removing = null }) { Text(stringResource(R.string.compass_cancel)) } },
        )
    }
    // docs/design/sync.md, "Push on a Cloudflare server": a server from before version 3 has no
    // such setting, and one with push off for good can't turn it on.
    if (state.pushRelayAvailable) {
        SwitchRow(
            title = stringResource(R.string.compass_sync_phone_notifications),
            details = SyncScreenState.pushRelayDetails(context.resources, state.pushRelayHost),
            checked = state.pushRelay,
            enabled = !state.peopleBusy,
            onCheckedChange = { listener.onSetServerPushRelay(it) },
        )
    }
    val nameField = rememberTextFieldState()
    OutlinedTextField(
        state = nameField,
        label = { Text(stringResource(R.string.compass_sync_invite_friend)) },
        placeholder = { Text(stringResource(R.string.compass_sync_invite_name_hint)) },
        lineLimits = TextFieldLineLimits.SingleLine,
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedButton(
        onClick = {
            listener.onInviteFriend(nameField.text.toString().trim())
            nameField.clearText()
        }
    ) {
        Text(stringResource(R.string.compass_sync_make_invite_link))
    }
    if (state.inviteLink.isNotEmpty()) {
        Text(stringResource(R.string.compass_sync_send_invite_link))
        SelectionContainer { Text(state.inviteLink, fontFamily = FontFamily.Monospace) }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(onClick = { listener.onShareInviteLink(state.inviteLink) }) { Text(stringResource(R.string.compass_share)) }
            OutlinedButton(onClick = { listener.onCopyInviteLink(state.inviteLink) }) {
                Text(stringResource(R.string.compass_copy_link))
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
            Text(stringResource(R.string.compass_sync_server_update_ready), fontWeight = FontWeight.SemiBold)
            val resources = LocalContext.current.resources
            Text(SyncScreenState.updateNote(resources, state.updateCurrent, state.updateLatest))
            Button(onClick = { listener.onUpdateServer("") }, enabled = !updating) {
                Text(stringResource(R.string.compass_sync_update_with_cloudflare))
            }
            if (!showToken) {
                TextButton(onClick = { showToken = true }, enabled = !updating) {
                    Text(stringResource(R.string.compass_sync_use_api_token))
                }
            } else {
                OutlinedSecureTextField(
                    state = tokenField,
                    label = { Text(stringResource(R.string.compass_sync_cloudflare_api_token)) },
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedButton(
                    onClick = {
                        listener.onUpdateServer(token)
                        tokenField.clearText()
                    },
                    enabled = token.isNotEmpty() && !updating,
                ) {
                    Text(stringResource(R.string.compass_sync_update))
                }
            }
            if (updating) Text(state.serverSetup)
            if (state.updateError.isNotEmpty()) {
                Text(state.updateError, color = MaterialTheme.colorScheme.error)
            }
        }
    }
}

@Composable
private fun WordList(words: List<String>) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        words.forEachIndexed { i, word -> Text("${i + 1}. $word", fontFamily = FontFamily.Monospace) }
    }
}

/** A new chain's words, shown once (no screen lock, as the desktop's "Start sync"). */
@Composable
private fun NewWords(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    Text(stringResource(R.string.compass_sync_your_recovery_code), style = MaterialTheme.typography.titleMedium)
    Text(stringResource(R.string.compass_sync_save_words_text))
    WordList(state.newWords)
    Button(onClick = { listener.onNewWordsSaved() }) { Text(stringResource(R.string.compass_sync_i_saved_it)) }
}

/** As the desktop's Recovery code section: behind the screen lock, the window secure meanwhile. */
@Composable
private fun RecoveryCode(state: SyncScreenState, listener: SyncScreenUi.Listener) {
    Text(stringResource(R.string.compass_sync_recovery_code), style = MaterialTheme.typography.titleMedium)
    if (state.recoveryWords.isEmpty()) {
        Text(stringResource(R.string.compass_sync_recovery_code_intro))
        if (state.recoveryError.isNotEmpty()) {
            Text(state.recoveryError, color = MaterialTheme.colorScheme.error)
        }
        OutlinedButton(onClick = { listener.onShowRecoveryCode() }) { Text(stringResource(R.string.compass_sync_show_recovery_code)) }
        return
    }
    Text(stringResource(R.string.compass_sync_save_words_text))
    if (state.recoveryUnprotected) {
        Text(
            stringResource(R.string.compass_sync_no_screen_lock),
            color = MaterialTheme.colorScheme.error,
        )
    }
    WordList(state.recoveryWords)
    Button(onClick = { listener.onHideRecoveryCode() }) { Text(stringResource(R.string.compass_sync_hide)) }
}
