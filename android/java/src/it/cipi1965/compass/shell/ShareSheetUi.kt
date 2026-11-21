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
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.clearText
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner

object ShareSheetUi {
    interface Listener {
        fun onShare()

        /** |expiresDays| 0: never; |uses| 0: any number. */
        fun onCopyLink(role: String, expiresDays: Int, uses: Int)

        fun onInviteByEmail(email: String, role: String)

        fun onChangeRole(clientId: String, role: String)

        fun onRemoveMember(clientId: String)

        fun onRevokeInvite(inviteId: String)

        fun onLeaveOrDelete()

        fun onSetDisplayName(name: String)

        fun onJoin()

        fun onDismiss()
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: ShareSheetState,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                when (state.mode) {
                    "sheet" -> ShareSheet(state, listener)
                    "join" -> JoinScreen(state, listener)
                }
            }
        }
        return view
    }
}

private fun kindName(kind: String) =
    when (kind) {
        "space" -> "space"
        "easel" -> "easel"
        else -> "folder"
    }

private fun roleName(role: String) =
    when (role) {
        "owner" -> "Owner"
        "editor" -> "Can edit"
        else -> "Can view"
    }

@Composable
private fun Page(listener: ShareSheetUi.Listener, content: @Composable () -> Unit) {
    BackHandler { listener.onDismiss() }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier.fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {}
                TextButton(onClick = { listener.onDismiss() }) { Text("Done") }
            }
            content()
        }
    }
}

@Composable
private fun Choice(
    label: String,
    options: List<Pair<String, String>>,
    selected: String,
    onSelect: (String) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    OutlinedButton(onClick = { open = true }) {
        Text(options.firstOrNull { it.first == selected }?.second ?: label)
    }
    DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
        for ((value, text) in options) {
            DropdownMenuItem(text = { Text(text) }, onClick = {
                open = false
                onSelect(value)
            })
        }
    }
}

@Composable
private fun ShareSheet(state: ShareSheetState, listener: ShareSheetUi.Listener) {
    var confirmLeave by remember { mutableStateOf(false) }
    var removing by remember { mutableStateOf<ShareSheetState.Member?>(null) }
    Page(listener) {
        Text(
            "Share “${state.name.ifEmpty { "Untitled" }}”",
            style = MaterialTheme.typography.titleLarge,
        )
        if (state.error.isNotEmpty()) {
            Text(state.error, color = MaterialTheme.colorScheme.error)
        }
        if (state.roomId.isEmpty()) {
            Text(
                "Invite people to this ${kindName(state.kind)}. Everyone sees changes as they " +
                    "happen. It stays end-to-end encrypted: your sync server can’t read it.",
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(onClick = { listener.onShare() }, enabled = state.canShare && !state.busy) {
                Text("Start sharing")
            }
            if (!state.canShare) {
                Text("Turn on sync to share. Sharing goes through your sync server.")
            }
            return@Page
        }
        val owner = state.role == "owner"
        Text(
            "Shared on ${state.server}. " +
                if (state.role == "viewer") "You can view it." else "End-to-end encrypted.",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (owner) {
            HorizontalDivider()
            Text("Invite with a link", fontWeight = FontWeight.SemiBold)
            var role by remember { mutableStateOf("editor") }
            var expires by remember { mutableStateOf("7") }
            var uses by remember { mutableStateOf("0") }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Choice("Role", listOf("editor" to "Can edit", "viewer" to "Can view"), role) { role = it }
                Choice(
                    "Expires",
                    listOf("7" to "7 days", "1" to "1 day", "0" to "Never"),
                    expires,
                ) { expires = it }
            }
            Choice(
                "Uses",
                listOf("0" to "Any number of people", "1" to "One person", "5" to "Up to 5", "25" to "Up to 25"),
                uses,
            ) { uses = it }
            Button(
                onClick = { listener.onCopyLink(role, expires.toInt(), uses.toInt()) },
                enabled = !state.busy,
            ) {
                Text(if (state.link.isEmpty()) "Copy invite link" else "Copy a new link")
            }
            if (state.link.isNotEmpty()) {
                Text("Copied. Anyone with the link can join until it expires or you revoke it.")
            }
            if (state.oidc) {
                HorizontalDivider()
                Text("Invite by email", fontWeight = FontWeight.SemiBold)
                val emailField = rememberTextFieldState()
                val email = emailField.text.toString().trim()
                var emailRole by remember { mutableStateOf("editor") }
                OutlinedTextField(
                    state = emailField,
                    label = { Text("name@company.com") },
                    lineLimits = TextFieldLineLimits.SingleLine,
                    modifier = Modifier.fillMaxWidth(),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Choice("Role", listOf("editor" to "Can edit", "viewer" to "Can view"), emailRole) {
                        emailRole = it
                    }
                    Button(onClick = {
                        listener.onInviteByEmail(email, emailRole)
                        emailField.clearText()
                    }, enabled = email.contains('@') && !state.busy) { Text("Invite") }
                }
            }
        }
        HorizontalDivider()
        Text("People (${state.members.size})", fontWeight = FontWeight.SemiBold)
        for (member in state.members) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    val name = member.name.ifEmpty { member.email.ifEmpty { "Someone" } }
                    Text(
                        (if (member.me) "$name (you)" else name) +
                            if (member.pending) " · invited" else ""
                    )
                    if (member.name.isNotEmpty() && member.email.isNotEmpty()) {
                        Text(member.email, style = MaterialTheme.typography.bodySmall)
                    }
                }
                if (owner && member.role != "owner") {
                    Choice(
                        "Role",
                        listOf("editor" to "Can edit", "viewer" to "Can view"),
                        member.role,
                    ) { listener.onChangeRole(member.clientId, it) }
                    TextButton(onClick = { removing = member }) {
                        Text("Remove", color = MaterialTheme.colorScheme.error)
                    }
                } else {
                    Text(roleName(member.role), style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        if (owner && state.invites.isNotEmpty()) {
            HorizontalDivider()
            Text("Invite links", fontWeight = FontWeight.SemiBold)
            for (invite in state.invites) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        roleName(invite.role) +
                            (if (invite.expires == 0L) " · never expires" else " · expires") +
                            (if (invite.usesLeft < 0) "" else ", ${invite.usesLeft} uses left"),
                        Modifier.weight(1f),
                    )
                    TextButton(onClick = { listener.onRevokeInvite(invite.id) }) { Text("Revoke") }
                }
            }
        }
        HorizontalDivider()
        Text("Your name for sharing", fontWeight = FontWeight.SemiBold)
        val nameField = rememberTextFieldState(state.displayName)
        OutlinedTextField(
            state = nameField,
            lineLimits = TextFieldLineLimits.SingleLine,
            modifier = Modifier.fillMaxWidth(),
        )
        TextButton(onClick = { listener.onSetDisplayName(nameField.text.toString()) }) { Text("Save name") }
        Text(
            "What the people you share with see, unless your company’s sign-in gives your name.",
            style = MaterialTheme.typography.bodySmall,
        )
        HorizontalDivider()
        TextButton(onClick = { confirmLeave = true }) {
            Text(
                if (owner) "Delete for everyone" else "Leave",
                color = MaterialTheme.colorScheme.error,
            )
        }
    }
    if (confirmLeave) {
        val owner = state.role == "owner"
        AlertDialog(
            onDismissRequest = { confirmLeave = false },
            title = { Text(if (owner) "Delete for everyone?" else "Leave?") },
            text = {
                Text(
                    if (owner) {
                        "Stop sharing this ${kindName(state.kind)} and remove it from everyone " +
                            "else? You keep it."
                    } else {
                        "Leave this ${kindName(state.kind)}? You keep a copy, but won’t see " +
                            "new changes."
                    }
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmLeave = false
                    listener.onLeaveOrDelete()
                }) { Text(if (owner) "Delete" else "Leave") }
            },
            dismissButton = { TextButton(onClick = { confirmLeave = false }) { Text("Cancel") } },
        )
    }
    removing?.let { member ->
        AlertDialog(
            onDismissRequest = { removing = null },
            title = { Text("Remove ${member.name.ifEmpty { "them" }}?") },
            text = {
                Text("They keep a copy of what was shared, but won’t see new changes.")
            },
            confirmButton = {
                TextButton(onClick = {
                    removing = null
                    listener.onRemoveMember(member.clientId)
                }) { Text("Remove") }
            },
            dismissButton = { TextButton(onClick = { removing = null }) { Text("Cancel") } },
        )
    }
}

@Composable
private fun JoinScreen(state: ShareSheetState, listener: ShareSheetUi.Listener) {
    Page(listener) {
        if (!state.joinValid) {
            Text("This invite link doesn’t work", style = MaterialTheme.typography.titleLarge)
            Text("It may have been copied only in part. Ask for a new one.")
            return@Page
        }
        if (state.joined) {
            Text("Joined", style = MaterialTheme.typography.titleLarge)
            Text("“${state.name}” is in your sidebar.")
            return@Page
        }
        Text(
            "Join “${state.name.ifEmpty { "Untitled" }}”",
            style = MaterialTheme.typography.titleLarge,
        )
        Text(
            "You’ve been invited to a shared ${kindName(state.joinKind)} on " +
                "${state.joinServer}. The person who shared it decides whether you can edit it " +
                "or only view it.",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (state.alreadyJoined) {
            Text("You’ve already joined it.")
            return@Page
        }
        if (state.joinKind != "space" && state.spaces.isNotEmpty()) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Put it in")
                Choice("Space", state.spaces.map { it.id to it.name }, state.joinSpace) { state.joinSpace = it }
            }
        }
        if (state.error.isNotEmpty()) {
            Text(state.error, color = MaterialTheme.colorScheme.error)
        }
        Button(onClick = { listener.onJoin() }, enabled = !state.busy) {
            Text(if (state.busy) "Joining…" else "Join")
        }
    }
}
