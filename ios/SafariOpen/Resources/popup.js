// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Safari's toolbar popup: open the page in Compass, or pin it to a space.
// The native handler (SafariWebExtensionHandler.swift) leaves the request
// in Compass's Inbox; the tab then goes to compass://inbox, which brings
// Compass up (Safari asks once).

const native = (message) => browser.runtime.sendNativeMessage("it.cipi1965.compass", message);

async function activeTab() {
  const [tab] = await browser.tabs.query({ active: true, currentWindow: true });
  return tab;
}

function note(text) {
  const el = document.getElementById("note");
  el.textContent = text;
  el.hidden = false;
}

async function send(action, spaceID, spaceName) {
  const tab = await activeTab();
  if (!tab || !/^https?:/.test(tab.url || "")) {
    note("Only web pages can go to Compass.");
    return;
  }
  const reply = await native({ action, url: tab.url, title: tab.title || "", spaceID });
  if (!reply || !reply.ok) {
    note("Compass couldn't take the page.");
    return;
  }
  if (action === "open") {
    await browser.tabs.update(tab.id, { url: "compass://inbox" });
    window.close();
  } else {
    note(`Pinned in ${spaceName}.`);
    setTimeout(() => window.close(), 900);
  }
}

document.getElementById("open").addEventListener("click", () => send("open"));

const reply = await native({ action: "spaces" }).catch(() => null);
const spaces = (reply && reply.spaces) || [];
if (spaces.length) {
  document.getElementById("pin-label").hidden = false;
  const list = document.getElementById("spaces");
  for (const space of spaces) {
    const button = document.createElement("button");
    const dot = document.createElement("span");
    dot.className = "dot";
    dot.style.background = space.accent;
    button.append(dot, document.createTextNode(space.name));
    button.addEventListener("click", () => send("pin", space.id, space.name));
    list.append(button);
  }
}
