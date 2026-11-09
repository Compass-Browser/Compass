// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.Context
import android.view.View
import androidx.activity.OnBackPressedDispatcherOwner
import androidx.activity.compose.BackHandler
import androidx.activity.setViewTreeOnBackPressedDispatcherOwner
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.platform.ComposeView
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.setViewTreeLifecycleOwner
import androidx.savedstate.SavedStateRegistryOwner
import androidx.savedstate.setViewTreeSavedStateRegistryOwner
import it.cipi1965.compass.R

/**
 * Find on page, as iOS's find bar over the keyboard: the text, "2 of 7", the previous and next
 * match, and Done, in a floating capsule (FindBar.java). Chrome's own find bar is part of the
 * toolbar Compass replaces.
 */
object FindBarUi {
    interface Listener {
        fun onTextChanged(text: String)

        fun onNext(forward: Boolean)

        fun onDone()
    }

    class State {
        var open by mutableStateOf(false)
        var text by mutableStateOf("")
        var active by mutableIntStateOf(0)
        var total by mutableIntStateOf(0)
        /** The page answered for the text (else nothing shows, rather than "0 of 0"). */
        var answered by mutableStateOf(false)
    }

    @JvmStatic
    fun createView(
        context: Context,
        lifecycleOwner: LifecycleOwner,
        savedStateOwner: SavedStateRegistryOwner,
        backOwner: OnBackPressedDispatcherOwner,
        state: State,
        listener: Listener,
    ): View {
        val view = ComposeView(context)
        view.setViewTreeLifecycleOwner(lifecycleOwner)
        view.setViewTreeSavedStateRegistryOwner(savedStateOwner)
        view.setViewTreeOnBackPressedDispatcherOwner(backOwner)
        view.setContent {
            CompassTheme(dark = isSystemInDarkTheme()) {
                if (state.open) FindBar(state, listener)
            }
        }
        return view
    }
}

// BasicTextField's String overload, as the command bar's field.
@Suppress("DEPRECATION")
@Composable
private fun FindBar(state: FindBarUi.State, listener: FindBarUi.Listener) {
    BackHandler { listener.onDone() }
    val focus = remember { FocusRequester() }
    val keyboard = LocalSoftwareKeyboardController.current
    LaunchedEffect(Unit) {
        focus.requestFocus()
        keyboard?.show()
    }
    // On a strip of its own: it covers the bar while it's up.
    Box(Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surface)) {
    Row(
        Modifier.fillMaxWidth()
            .padding(horizontal = 12.dp, vertical = 6.dp)
            .height(54.dp)
            .floatingGlass(CircleShape)
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Sym(R.drawable.compass_sym_search, 16f, MaterialTheme.colorScheme.onSurfaceVariant)
        Box(Modifier.weight(1f)) {
            if (state.text.isEmpty()) {
                Text("Find on page", fontSize = 17.sp, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 1)
            }
            BasicTextField(
                value = state.text,
                onValueChange = { listener.onTextChanged(it) },
                singleLine = true,
                textStyle = MaterialTheme.typography.bodyLarge.copy(color = MaterialTheme.colorScheme.onSurface),
                cursorBrush = SolidColor(MaterialTheme.colorScheme.primary),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = { listener.onNext(true) }),
                modifier = Modifier.fillMaxWidth().focusRequester(focus),
            )
        }
        if (state.text.isNotEmpty() && state.answered) {
            Text(
                if (state.total == 0) "No matches" else "${state.active} of ${state.total}",
                fontSize = 13.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
            )
        }
        val enabled = state.total > 0
        Box(
            Modifier.size(36.dp).clip(CircleShape).clickable(enabled = enabled) { listener.onNext(false) },
            contentAlignment = Alignment.Center,
        ) {
            Sym(R.drawable.compass_sym_chevron_right, 18f, modifier = Modifier.rotate(-90f).alpha(if (enabled) 1f else 0.3f))
        }
        Box(
            Modifier.size(36.dp).clip(CircleShape).clickable(enabled = enabled) { listener.onNext(true) },
            contentAlignment = Alignment.Center,
        ) {
            Sym(R.drawable.compass_sym_chevron_right, 18f, modifier = Modifier.rotate(90f).alpha(if (enabled) 1f else 0.3f))
        }
        androidx.compose.material3.TextButton(onClick = { listener.onDone() }) { Text("Done") }
    }
}
}
