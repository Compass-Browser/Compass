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
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.clearText
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.lifecycle.LifecycleOwner
import it.cipi1965.compass.R
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

/** One of three strings by the kind ("space", "easel" or a folder): the words agree with it. */
private fun byKind(kind: String, space: Int, folder: Int, easel: Int) =
    when (kind) {
        "space" -> space
        "easel" -> easel
        else -> folder
    }

@Composable
private fun roleName(role: String) =
    when (role) {
        "owner" -> stringResource(R.string.compass_share_role_owner)
        "editor" -> stringResource(R.string.compass_share_role_editor)
        else -> stringResource(R.string.compass_share_role_viewer)
    }

/** The roles an invite or a member can have. */
@Composable
private fun roleChoices() =
    listOf(
        "editor" to stringResource(R.string.compass_share_role_editor),
        "viewer" to stringResource(R.string.compass_share_role_viewer),
    )

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
                TextButton(onClick = { listener.onDismiss() }) { Text(stringResource(R.string.compass_done)) }
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
            stringResource(
                R.string.compass_share_title,
                state.name.ifEmpty { stringResource(R.string.compass_untitled) },
            ),
            style = MaterialTheme.typography.titleLarge,
        )
        if (state.error.isNotEmpty()) {
            Text(state.error, color = MaterialTheme.colorScheme.error)
        }
        if (state.roomId.isEmpty()) {
            Text(
                stringResource(
                    byKind(
                        state.kind,
                        R.string.compass_share_intro_space,
                        R.string.compass_share_intro_folder,
                        R.string.compass_share_intro_easel,
                    )
                ),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Button(onClick = { listener.onShare() }, enabled = state.canShare && !state.busy) {
                Text(stringResource(R.string.compass_share_start))
            }
            if (!state.canShare) {
                Text(stringResource(R.string.compass_share_needs_sync))
            }
            return@Page
        }
        val owner = state.role == "owner"
        Text(
            if (state.role == "viewer") {
                stringResource(R.string.compass_share_shared_on_viewer, state.server)
            } else {
                stringResource(R.string.compass_share_shared_on, state.server)
            },
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (owner) {
            HorizontalDivider()
            Text(stringResource(R.string.compass_share_invite_with_link), fontWeight = FontWeight.SemiBold)
            var role by remember { mutableStateOf("editor") }
            var expires by remember { mutableStateOf("7") }
            var uses by remember { mutableStateOf("0") }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Choice(stringResource(R.string.compass_share_role), roleChoices(), role) { role = it }
                Choice(
                    stringResource(R.string.compass_share_expires),
                    listOf(
                        "7" to stringResource(R.string.compass_share_7_days),
                        "1" to stringResource(R.string.compass_share_1_day),
                        "0" to stringResource(R.string.compass_never),
                    ),
                    expires,
                ) { expires = it }
            }
            Choice(
                stringResource(R.string.compass_share_uses),
                listOf(
                    "0" to stringResource(R.string.compass_share_any_number),
                    "1" to stringResource(R.string.compass_share_one_person),
                    "5" to stringResource(R.string.compass_share_up_to_5),
                    "25" to stringResource(R.string.compass_share_up_to_25),
                ),
                uses,
            ) { uses = it }
            Button(
                onClick = { listener.onCopyLink(role, expires.toInt(), uses.toInt()) },
                enabled = !state.busy,
            ) {
                Text(
                    stringResource(
                        if (state.link.isEmpty()) {
                            R.string.compass_share_copy_invite_link
                        } else {
                            R.string.compass_share_copy_new_link
                        }
                    )
                )
            }
            if (state.link.isNotEmpty()) {
                Text(stringResource(R.string.compass_share_link_copied))
            }
            if (state.oidc) {
                HorizontalDivider()
                Text(stringResource(R.string.compass_share_invite_by_email), fontWeight = FontWeight.SemiBold)
                val emailField = rememberTextFieldState()
                val email = emailField.text.toString().trim()
                var emailRole by remember { mutableStateOf("editor") }
                OutlinedTextField(
                    state = emailField,
                    label = { Text(stringResource(R.string.compass_share_email_hint)) },
                    lineLimits = TextFieldLineLimits.SingleLine,
                    modifier = Modifier.fillMaxWidth(),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Choice(stringResource(R.string.compass_share_role), roleChoices(), emailRole) {
                        emailRole = it
                    }
                    Button(onClick = {
                        listener.onInviteByEmail(email, emailRole)
                        emailField.clearText()
                    }, enabled = email.contains('@') && !state.busy) { Text(stringResource(R.string.compass_share_invite)) }
                }
            }
        }
        HorizontalDivider()
        Text(stringResource(R.string.compass_share_people, state.members.size), fontWeight = FontWeight.SemiBold)
        for (member in state.members) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    val name =
                        member.name.ifEmpty {
                            member.email.ifEmpty { stringResource(R.string.compass_share_someone) }
                        }
                    val named =
                        if (member.me) stringResource(R.string.compass_share_member_you, name) else name
                    Text(
                        if (member.pending) {
                            stringResource(R.string.compass_share_member_invited, named)
                        } else {
                            named
                        }
                    )
                    if (member.name.isNotEmpty() && member.email.isNotEmpty()) {
                        Text(member.email, style = MaterialTheme.typography.bodySmall)
                    }
                }
                if (owner && member.role != "owner") {
                    Choice(
                        stringResource(R.string.compass_share_role),
                        roleChoices(),
                        member.role,
                    ) { listener.onChangeRole(member.clientId, it) }
                    TextButton(onClick = { removing = member }) {
                        Text(stringResource(R.string.compass_remove), color = MaterialTheme.colorScheme.error)
                    }
                } else {
                    Text(roleName(member.role), style = MaterialTheme.typography.bodySmall)
                }
            }
        }
        if (owner && state.invites.isNotEmpty()) {
            HorizontalDivider()
            Text(stringResource(R.string.compass_share_invite_links), fontWeight = FontWeight.SemiBold)
            for (invite in state.invites) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    val role = roleName(invite.role)
                    val expiry =
                        if (invite.expires == 0L) {
                            stringResource(R.string.compass_share_invite_never_expires, role)
                        } else {
                            stringResource(R.string.compass_share_invite_expires, role)
                        }
                    Text(
                        if (invite.usesLeft < 0) {
                            expiry
                        } else {
                            pluralStringResource(
                                R.plurals.compass_share_invite_uses_left,
                                invite.usesLeft,
                                expiry,
                                invite.usesLeft,
                            )
                        },
                        Modifier.weight(1f),
                    )
                    TextButton(onClick = { listener.onRevokeInvite(invite.id) }) {
                        Text(stringResource(R.string.compass_share_revoke))
                    }
                }
            }
        }
        HorizontalDivider()
        Text(stringResource(R.string.compass_share_your_name), fontWeight = FontWeight.SemiBold)
        val nameField = rememberTextFieldState(state.displayName)
        OutlinedTextField(
            state = nameField,
            lineLimits = TextFieldLineLimits.SingleLine,
            modifier = Modifier.fillMaxWidth(),
        )
        TextButton(onClick = { listener.onSetDisplayName(nameField.text.toString()) }) { Text(stringResource(R.string.compass_share_save_name)) }
        Text(
            stringResource(R.string.compass_share_your_name_details),
            style = MaterialTheme.typography.bodySmall,
        )
        HorizontalDivider()
        TextButton(onClick = { confirmLeave = true }) {
            Text(
                stringResource(
                    if (owner) R.string.compass_share_delete_for_everyone else R.string.compass_share_leave
                ),
                color = MaterialTheme.colorScheme.error,
            )
        }
    }
    if (confirmLeave) {
        val owner = state.role == "owner"
        AlertDialog(
            onDismissRequest = { confirmLeave = false },
            title = {
                Text(
                    stringResource(
                        if (owner) {
                            R.string.compass_share_delete_for_everyone_question
                        } else {
                            R.string.compass_share_leave_question
                        }
                    )
                )
            },
            text = {
                Text(
                    stringResource(
                        if (owner) {
                            byKind(
                                state.kind,
                                R.string.compass_share_stop_sharing_space,
                                R.string.compass_share_stop_sharing_folder,
                                R.string.compass_share_stop_sharing_easel,
                            )
                        } else {
                            byKind(
                                state.kind,
                                R.string.compass_share_leave_space,
                                R.string.compass_share_leave_folder,
                                R.string.compass_share_leave_easel,
                            )
                        }
                    )
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmLeave = false
                    listener.onLeaveOrDelete()
                }) {
                    Text(
                        stringResource(
                            if (owner) R.string.compass_delete else R.string.compass_share_leave
                        )
                    )
                }
            },
            dismissButton = {
                TextButton(onClick = { confirmLeave = false }) {
                    Text(stringResource(R.string.compass_cancel))
                }
            },
        )
    }
    removing?.let { member ->
        AlertDialog(
            onDismissRequest = { removing = null },
            title = {
                Text(
                    if (member.name.isEmpty()) {
                        stringResource(R.string.compass_share_remove_them)
                    } else {
                        stringResource(R.string.compass_share_remove_named, member.name)
                    }
                )
            },
            text = {
                Text(stringResource(R.string.compass_share_remove_member_text))
            },
            confirmButton = {
                TextButton(onClick = {
                    removing = null
                    listener.onRemoveMember(member.clientId)
                }) { Text(stringResource(R.string.compass_remove)) }
            },
            dismissButton = {
                TextButton(onClick = { removing = null }) {
                    Text(stringResource(R.string.compass_cancel))
                }
            },
        )
    }
}

@Composable
private fun JoinScreen(state: ShareSheetState, listener: ShareSheetUi.Listener) {
    Page(listener) {
        if (!state.joinValid) {
            Text(stringResource(R.string.compass_share_invite_invalid), style = MaterialTheme.typography.titleLarge)
            Text(stringResource(R.string.compass_share_invite_invalid_details))
            return@Page
        }
        if (state.joined) {
            Text(stringResource(R.string.compass_share_joined), style = MaterialTheme.typography.titleLarge)
            Text(stringResource(R.string.compass_share_in_sidebar, state.name))
            return@Page
        }
        Text(
            stringResource(
                R.string.compass_share_join_title,
                state.name.ifEmpty { stringResource(R.string.compass_untitled) },
            ),
            style = MaterialTheme.typography.titleLarge,
        )
        Text(
            stringResource(
                byKind(
                    state.joinKind,
                    R.string.compass_share_invited_space,
                    R.string.compass_share_invited_folder,
                    R.string.compass_share_invited_easel,
                ),
                state.joinServer,
            ),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (state.alreadyJoined) {
            Text(stringResource(R.string.compass_share_already_joined))
            return@Page
        }
        if (state.joinKind != "space" && state.spaces.isNotEmpty()) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.compass_share_put_it_in))
                Choice(stringResource(R.string.compass_share_space), state.spaces.map { it.id to it.name }, state.joinSpace) { state.joinSpace = it }
            }
        }
        if (state.error.isNotEmpty()) {
            Text(state.error, color = MaterialTheme.colorScheme.error)
        }
        Button(onClick = { listener.onJoin() }, enabled = !state.busy) {
            Text(
                stringResource(
                    if (state.busy) R.string.compass_share_joining else R.string.compass_share_join
                )
            )
        }
    }
}
