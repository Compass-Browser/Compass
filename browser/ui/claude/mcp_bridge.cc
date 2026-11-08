// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/mcp_bridge.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/path_service.h"
#include "base/memory/ref_counted.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/common/chrome_paths.h"
#include "compass/browser/ui/claude/mcp_listener.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/constrained_window/constrained_window_views.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "ui/base/base_window.h"
#include "ui/base/interaction/element_identifier.h"
#include "ui/base/models/dialog_model.h"

namespace compass::claude {

namespace {

McpBridge* g_bridge = nullptr;

DEFINE_LOCAL_ELEMENT_IDENTIFIER_VALUE(kAlwaysAllowCheckbox);

BrowserWindowInterface* LastActiveWindow(Profile* profile) {
  BrowserWindowInterface* found = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (browser->GetProfile() != profile ||
            browser->GetType() != BrowserWindowInterface::TYPE_NORMAL) {
          return true;
        }
        WindowFeatures* features = WindowFeatures::From(browser);
        if (features &&
            (features->is_parking_window() || features->swapped_away())) {
          return true;
        }
        found = browser;
        return false;
      });
  return found;
}

}  // namespace

// static
McpBridge* McpBridge::Get() {
  if (!g_bridge) {
    g_bridge = new McpBridge();
  }
  return g_bridge;
}

// static
McpBridge* McpBridge::GetIfExists() {
  return g_bridge;
}

McpBridge::McpBridge() = default;
McpBridge::~McpBridge() = default;

McpBridge::Chat::Chat() = default;
McpBridge::Chat::Chat(Chat&&) = default;
McpBridge::Chat& McpBridge::Chat::operator=(Chat&&) = default;
McpBridge::Chat::~Chat() = default;

McpBridge::Connection::Connection() = default;
McpBridge::Connection::Connection(Connection&&) = default;
McpBridge::Connection& McpBridge::Connection::operator=(Connection&&) =
    default;
McpBridge::Connection::~Connection() = default;

base::FilePath McpBridge::socket_path() const {
  if (!socket_path_for_testing_.empty()) {
    return socket_path_for_testing_;
  }
  base::FilePath user_data;
  base::PathService::Get(chrome::DIR_USER_DATA, &user_data);
  return user_data.AppendASCII(kSocketName);
}

void McpBridge::SetSocketPathForTesting(base::FilePath path) {
  socket_path_for_testing_ = std::move(path);
  if (!listener_.is_null()) {
    listener_.Reset();
    listening_ = false;
    UpdateListener();
  }
}

void McpBridge::SetAskForTesting(McpSession::AskCallback ask) {
  ask_for_testing_ = std::move(ask);
}

void McpBridge::SetProfileEnabled(Profile* profile, bool enabled) {
  if (enabled) {
    profiles_.insert(profile);
  } else {
    profiles_.erase(profile);
    // Its connections end: the user turned it off, or the profile goes.
    // A chat's connections are the chat's to end.
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if (it->second.session && it->second.chat.empty() &&
          it->second.session->profile() == profile) {
        listener_.AsyncCall(&McpListener::Close).WithArgs(it->first);
        it = sessions_.erase(it);
      } else {
        ++it;
      }
    }
  }
  UpdateListener();
}

std::string McpBridge::RegisterChat(Profile* profile,
                                    McpSession::ChatToolRunner runner) {
  std::string token = base::HexEncodeLower(base::RandBytesAsVector(16));
  Chat chat;
  chat.profile = profile;
  chat.runner = std::move(runner);
  chats_.emplace(token, std::move(chat));
  UpdateListener();
  return token;
}

void McpBridge::UnregisterChat(const std::string& token) {
  chats_.erase(token);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->second.chat == token) {
      listener_.AsyncCall(&McpListener::Close).WithArgs(it->first);
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
  UpdateListener();
}

void McpBridge::UpdateListener() {
  if (profiles_.empty() && chats_.empty()) {
    listener_.Reset();
    listening_ = false;
    // Connections that hadn't said whose they are close with it.
    sessions_.clear();
    return;
  }
  if (!listener_.is_null()) {
    return;
  }
  scoped_refptr<base::SequencedTaskRunner> ui =
      base::SequencedTaskRunner::GetCurrentDefault();
  base::WeakPtr<McpBridge> self = weak_factory_.GetWeakPtr();
  McpListener::Callbacks callbacks;
  callbacks.connected = base::BindPostTask(
      ui, base::BindRepeating(&McpBridge::OnConnected, self));
  callbacks.line =
      base::BindPostTask(ui, base::BindRepeating(&McpBridge::OnLine, self));
  callbacks.closed =
      base::BindPostTask(ui, base::BindRepeating(&McpBridge::OnClosed, self));
  callbacks.listening =
      base::BindPostTask(ui, base::BindOnce(&McpBridge::OnListening, self));
  // net's sockets need the IO thread's message pump.
  listener_ = base::SequenceBound<McpListener>(
      content::GetIOThreadTaskRunner({}), socket_path(), std::move(callbacks));
}

void McpBridge::OnListening(bool listening) {
  listening_ = listening;
}

void McpBridge::OnConnected(int id) {
  // Whose it is comes with its first line.
  sessions_.emplace(id, Connection());
}

void McpBridge::OnLine(int id, std::string line) {
  auto it = sessions_.find(id);
  if (it == sessions_.end()) {
    return;
  }
  if (!it->second.session) {
    StartSession(id, line);
    return;
  }
  it->second.session->OnMessage(line);
}

void McpBridge::StartSession(int id, const std::string& first_line) {
  Connection& connection = sessions_[id];
  std::optional<base::DictValue> hello =
      base::JSONReader::ReadDict(first_line, base::JSON_PARSE_RFC);
  if (const std::string* token =
          hello ? hello->FindString("compass_chat") : nullptr) {
    auto chat = chats_.find(*token);
    if (chat == chats_.end()) {
      // A chat that ended, or a guess.
      listener_.AsyncCall(&McpListener::Close).WithArgs(id);
      sessions_.erase(id);
      return;
    }
    connection.chat = *token;
    connection.session = std::make_unique<McpSession>(
        chat->second.profile,
        base::BindRepeating(&McpBridge::Send, weak_factory_.GetWeakPtr(), id),
        chat->second.runner);
    return;
  }
  Profile* profile = PickProfile();
  if (!profile) {
    // No window of a profile that allows MCP: nothing to serve. The error
    // goes out before the close (the listener runs them in order).
    Send(id,
         R"({"jsonrpc":"2.0","id":null,"error":{"code":-32000,"message":)"
         R"("Open a Compass window of a profile that allows MCP clients."}})");
    listener_.AsyncCall(&McpListener::Close).WithArgs(id);
    sessions_.erase(id);
    return;
  }
  connection.session = std::make_unique<McpSession>(
      profile,
      base::BindRepeating(&McpBridge::Send, weak_factory_.GetWeakPtr(), id),
      base::BindRepeating(&McpBridge::Ask, weak_factory_.GetWeakPtr()));
  connection.session->OnMessage(first_line);
}

void McpBridge::OnClosed(int id) {
  sessions_.erase(id);
}

void McpBridge::Send(int id, std::string line) {
  listener_.AsyncCall(&McpListener::Send).WithArgs(id, std::move(line));
}

Profile* McpBridge::PickProfile() {
  Profile* found = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* browser) {
        if (profiles_.contains(browser->GetProfile())) {
          found = browser->GetProfile();
          return false;
        }
        return true;
      });
  return found;
}

void McpBridge::Ask(Profile* profile,
                    const std::string& client,
                    base::OnceCallback<void(McpApproval)> done) {
  if (ask_for_testing_) {
    ask_for_testing_.Run(profile, client, std::move(done));
    return;
  }
  BrowserWindowInterface* window = LastActiveWindow(profile);
  if (!window) {
    std::move(done).Run(McpApproval::kDeny);
    return;
  }
  // Whichever button, or closing the dialog, answers once. The model is
  // the dialog's, alive while its callbacks can run.
  struct Pending : base::RefCounted<Pending> {
    base::OnceCallback<void(McpApproval)> done;
    raw_ptr<ui::DialogModel> model = nullptr;

    void Answer(McpApproval approval) {
      if (done) {
        std::move(done).Run(approval);
      }
    }
    void Allow() {
      const bool always =
          model && model->GetCheckboxByUniqueId(kAlwaysAllowCheckbox)
                       ->is_checked();
      Answer(always ? McpApproval::kAlwaysAllow : McpApproval::kAllowOnce);
    }

   private:
    friend class base::RefCounted<Pending>;
    ~Pending() = default;
  };
  auto pending = base::MakeRefCounted<Pending>();
  pending->done = std::move(done);
  const std::u16string name = base::UTF8ToUTF16(client);
  std::unique_ptr<ui::DialogModel> model =
      ui::DialogModel::Builder()
          .SetTitle(name + u" wants to control Compass")
          .AddParagraph(ui::DialogModelLabel(
              name +
              u" is an app on this Mac that uses Claude's tools in Compass. "
              u"Allowed, it can read your tabs, take screenshots, read their "
              u"consoles, run JavaScript, and click and type in them, signed "
              u"in as you. Allow only apps you trust."))
          .AddCheckbox(kAlwaysAllowCheckbox,
                       ui::DialogModelLabel(u"Always allow " + name))
          .AddOkButton(base::BindOnce(&Pending::Allow, pending),
                       ui::DialogModel::Button::Params().SetLabel(u"Allow"))
          .AddCancelButton(
              base::BindOnce(&Pending::Answer, pending, McpApproval::kDeny),
              ui::DialogModel::Button::Params().SetLabel(u"Deny"))
          .SetCloseActionCallback(
              base::BindOnce(&Pending::Answer, pending, McpApproval::kDeny))
          .Build();
  pending->model = model.get();
  constrained_window::ShowBrowserModal(
      std::move(model), window->GetWindow()->GetNativeWindow());
  // The user is working in the client: show them the question.
  window->GetWindow()->Activate();
}

}  // namespace compass::claude
