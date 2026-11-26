// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.content.res.Resources
import it.cipi1965.compass.R

/** What the UI calls the AI assistant, by provider (the desktop's AssistantName() and AskTitle()). */
object AiNames {
    /** "Claude", "GPT", "Gemini", or "" for an OpenAI-compatible server, which has no name. */
    @JvmStatic
    fun assistantName(provider: String): String =
        when (provider) {
            "openai" -> "GPT"
            "gemini" -> "Gemini"
            "compatible" -> ""
            else -> "Claude"
        }

    /**
     * "Ask Claude", "Ask GPT", "Ask Gemini", "Ask AI". AI has a string of its own: languages put an
     * article before it ("Chiedi all’AI") but not before a name ("Chiedi a Claude").
     */
    @JvmStatic
    fun askTitle(res: Resources, provider: String): String {
        val name = assistantName(provider)
        return if (name.isEmpty()) res.getString(R.string.compass_ask_ai)
        else res.getString(R.string.compass_ask_named, name)
    }
}
