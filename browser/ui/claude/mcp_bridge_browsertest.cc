// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/claude/mcp_bridge.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_prefs.h"
#include "compass/browser/window_sync/space_directory.h"
#include "components/prefs/pref_service.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

// The first text block of a tools/call reply.
std::string TextOf(const std::optional<base::DictValue>& reply) {
  if (!reply) {
    return std::string();
  }
  const base::ListValue* content =
      reply->FindListByDottedPath("result.content");
  if (!content || content->empty()) {
    return std::string();
  }
  const std::string* text = (*content)[0].GetDict().FindString("text");
  return text ? *text : std::string();
}

// The id list_tabs gives the entry whose URL contains |url_part|: the line
// reads "- <id>: <title> — <url>".
std::string IdIn(const std::string& list, const std::string& url_part) {
  const size_t at = list.find(url_part);
  if (at == std::string::npos) {
    return std::string();
  }
  const size_t line_start = list.rfind('\n', at);
  const size_t dash =
      list.find("- ", line_start == std::string::npos ? 0 : line_start);
  const size_t colon = list.find(':', dash);
  if (dash == std::string::npos || colon == std::string::npos || colon > at) {
    return std::string();
  }
  return list.substr(dash + 2, colon - dash - 2);
}

// A JSON-RPC client on a pair of file descriptors (a socket, or a
// process's pipes). Reads never block: they wait with RunUntil, so the
// browser keeps answering.
class Client {
 public:
  Client(base::ScopedFD read_fd, base::ScopedFD write_fd)
      : read_fd_(std::move(read_fd)), write_fd_(std::move(write_fd)) {
    fcntl(read_fd_.get(), F_SETFL, O_NONBLOCK);
  }

  // A client of the socket at |path|; invalid if it can't connect.
  static std::unique_ptr<Client> Connect(const base::FilePath& path) {
    base::ScopedFD fd(socket(AF_UNIX, SOCK_STREAM, 0));
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    // A path too long for a socket address links to a short one, as
    // compass-mcp knows.
    std::string target = path.value();
    if (target.size() >= sizeof(address.sun_path)) {
      base::FilePath link;
      if (!base::ReadSymbolicLink(path, &link)) {
        return nullptr;
      }
      target = link.value();
    }
    target.copy(address.sun_path, sizeof(address.sun_path) - 1);
    if (connect(fd.get(), reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
      return nullptr;
    }
    base::ScopedFD write_fd(dup(fd.get()));
    return std::make_unique<Client>(std::move(fd), std::move(write_fd));
  }

  void Send(base::DictValue message) {
    message.Set("jsonrpc", "2.0");
    std::string line;
    base::JSONWriter::Write(message, &line);
    line += '\n';
    ASSERT_TRUE(base::WriteFileDescriptor(write_fd_.get(), line));
  }

  void Request(int id,
               const std::string& method,
               base::DictValue params = base::DictValue()) {
    Send(base::DictValue()
             .Set("id", id)
             .Set("method", method)
             .Set("params", std::move(params)));
  }

  // The next message, or nullopt once the other side closed.
  std::optional<base::DictValue> Read() {
    std::optional<base::DictValue> message;
    EXPECT_TRUE(base::test::RunUntil([&] {
      Pump();
      const size_t end = pending_.find('\n');
      if (end != std::string::npos) {
        message = base::JSONReader::ReadDict(pending_.substr(0, end),
                                             base::JSON_PARSE_RFC);
        pending_.erase(0, end + 1);
        return true;
      }
      return closed_;
    }));
    return message;
  }

  bool WaitForClose() {
    return base::test::RunUntil([&] {
      Pump();
      return closed_;
    });
  }

 private:
  void Pump() {
    char buffer[65536];
    for (;;) {
      const ssize_t got = read(read_fd_.get(), buffer, sizeof(buffer));
      if (got > 0) {
        pending_.append(buffer, got);
        continue;
      }
      if (got == 0) {
        closed_ = true;
      }
      return;
    }
  }

  base::ScopedFD read_fd_;
  base::ScopedFD write_fd_;
  std::string pending_;
  bool closed_ = false;
};

class McpBridgeBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(base::BindRepeating(
        [](const net::test_server::HttpRequest& request)
            -> std::unique_ptr<net::test_server::HttpResponse> {
          auto response =
              std::make_unique<net::test_server::BasicHttpResponse>();
          response->set_content_type("text/html");
          response->set_content(
              request.relative_url == "/other.html"
                  ? "<title>Other page</title><p>Hello from the other "
                    "profile."
                  : "<title>MCP page</title><p>Hello from the MCP page.");
          return response;
        }));
    ASSERT_TRUE(embedded_test_server()->Start());
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("/page.html")));

    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      ASSERT_TRUE(dir_.CreateUniqueTempDir());
    }
    socket_path_ = dir_.GetPath().Append(McpBridge::kSocketName);
    // Often too long for a socket address: the bridge links to a short one.
    bridge()->SetSocketPathForTesting(socket_path_);
    bridge()->SetAskForTesting(base::BindRepeating(
        [](McpBridgeBrowserTest* self, Profile* profile,
           const std::string& client,
           base::OnceCallback<void(McpApproval)> done) {
          self->asked_.push_back(client);
          self->asked_profiles_.push_back(profile);
          std::move(done).Run(self->answer_);
        },
        this));
  }

  void TearDownOnMainThread() override {
    asked_profiles_.clear();
    bridge()->SetProfileEnabled(profile(), false);
    bridge()->SetAskForTesting(McpSession::AskCallback());
    bridge()->SetSocketPathForTesting(base::FilePath());
    InProcessBrowserTest::TearDownOnMainThread();
  }

  McpBridge* bridge() { return McpBridge::Get(); }
  Profile* profile() { return browser()->GetProfile(); }

  void Enable() {
    bridge()->SetProfileEnabled(profile(), true);
    ASSERT_TRUE(base::test::RunUntil([&] { return bridge()->listening(); }));
  }

  // initialize, as a client named |name|.
  void Initialize(Client& client, const std::string& name) {
    client.Request(
        1, "initialize",
        base::DictValue()
            .Set("protocolVersion", "2025-06-18")
            .Set("capabilities", base::DictValue())
            .Set("clientInfo",
                 base::DictValue().Set("name", name).Set("version", "1")));
    std::optional<base::DictValue> reply = client.Read();
    ASSERT_TRUE(reply);
    EXPECT_EQ(*reply->FindStringByDottedPath("result.protocolVersion"),
              "2025-06-18");
    EXPECT_EQ(*reply->FindStringByDottedPath("result.serverInfo.name"),
              "compass");
    client.Send(base::DictValue().Set("method", "notifications/initialized"));
  }

  std::optional<base::DictValue> Call(
      Client& client,
      int id,
      const std::string& tool,
      base::DictValue arguments = base::DictValue()) {
    client.Request(id, "tools/call",
                   base::DictValue()
                       .Set("name", tool)
                       .Set("arguments", std::move(arguments)));
    return client.Read();
  }

  base::ScopedTempDir dir_;
  base::FilePath socket_path_;
  std::vector<std::string> asked_;
  std::vector<raw_ptr<Profile>> asked_profiles_;
  McpApproval answer_ = McpApproval::kAllowOnce;
};

IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, ListsAndCallsTools) {
  // Off by default: no socket.
  EXPECT_FALSE(Client::Connect(socket_path_));
  Enable();
  std::unique_ptr<Client> client = Client::Connect(socket_path_);
  ASSERT_TRUE(client);
  Initialize(*client, "test-client");

  client->Request(2, "tools/list");
  std::optional<base::DictValue> listed = client->Read();
  ASSERT_TRUE(listed);
  const base::ListValue& tools = *listed->FindListByDottedPath("result.tools");
  ASSERT_FALSE(tools.empty());
  EXPECT_EQ(*tools[0].GetDict().FindString("name"), "read_page");
  EXPECT_TRUE(tools[0].GetDict().FindDict("inputSchema"));
  EXPECT_FALSE(tools[0].GetDict().contains("input_schema"));
  // Listing asks nothing.
  EXPECT_TRUE(asked_.empty());

  // The developer tools only while they're on, without edit_page_storage,
  // which asks in a chat.
  auto names = [](const base::ListValue& list) {
    std::vector<std::string> names;
    for (const base::Value& tool : list) {
      names.push_back(*tool.GetDict().FindString("name"));
    }
    return names;
  };
  EXPECT_FALSE(std::ranges::contains(names(tools), "network_requests"));
  profile()->GetPrefs()->SetBoolean(prefs::kDevTools, true);
  client->Request(20, "tools/list");
  std::optional<base::DictValue> with_devtools = client->Read();
  ASSERT_TRUE(with_devtools);
  const std::vector<std::string> devtools_names =
      names(*with_devtools->FindListByDottedPath("result.tools"));
  EXPECT_TRUE(std::ranges::contains(devtools_names, "network_requests"));
  EXPECT_TRUE(std::ranges::contains(devtools_names, "emulate"));
  EXPECT_FALSE(std::ranges::contains(devtools_names, "edit_page_storage"));
  profile()->GetPrefs()->SetBoolean(prefs::kDevTools, false);

  // The first call asks; the user's active tab is the target.
  std::optional<base::DictValue> text = Call(*client, 3, "get_page_text");
  ASSERT_TRUE(text);
  EXPECT_EQ(asked_, std::vector<std::string>{"test-client"});
  EXPECT_EQ(text->FindBoolByDottedPath("result.isError"), false);
  const base::ListValue& content =
      *text->FindListByDottedPath("result.content");
  EXPECT_THAT(*content[0].GetDict().FindString("text"),
              HasSubstr("Hello from the MCP page."));

  // Once per connection; images as MCP's.
  std::optional<base::DictValue> shot = Call(*client, 4, "screenshot");
  ASSERT_TRUE(shot);
  EXPECT_EQ(asked_.size(), 1u);
  const base::DictValue& image =
      (*shot->FindListByDottedPath("result.content"))[0].GetDict();
  EXPECT_EQ(*image.FindString("type"), "image");
  EXPECT_EQ(*image.FindString("mimeType"), "image/jpeg");
  EXPECT_FALSE(image.FindString("data")->empty());

  // Browser tools follow "Let Claude use the whole browser".
  std::optional<base::DictValue> tabs = Call(*client, 5, "list_tabs");
  ASSERT_TRUE(tabs);
  EXPECT_EQ(tabs->FindBoolByDottedPath("result.isError"), true);

  client->Request(6, "ping");
  EXPECT_TRUE(client->Read()->FindDict("result"));
  client->Request(7, "resources/list");
  EXPECT_EQ(client->Read()->FindIntByDottedPath("error.code"), -32601);
}

IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, DeniedAndAlwaysAllowed) {
  Enable();
  answer_ = McpApproval::kDeny;
  std::unique_ptr<Client> denied = Client::Connect(socket_path_);
  ASSERT_TRUE(denied);
  Initialize(*denied, "sketchy");
  std::optional<base::DictValue> refused = Call(*denied, 2, "get_page_text");
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->FindBoolByDottedPath("result.isError"), true);
  EXPECT_THAT(*(*refused->FindListByDottedPath("result.content"))[0]
                   .GetDict()
                   .FindString("text"),
              HasSubstr("didn't allow sketchy"));

  answer_ = McpApproval::kAlwaysAllow;
  std::unique_ptr<Client> first = Client::Connect(socket_path_);
  ASSERT_TRUE(first);
  Initialize(*first, "Claude Code");
  ASSERT_TRUE(Call(*first, 2, "get_page_text"));
  EXPECT_EQ(asked_.size(), 2u);
  EXPECT_EQ(profile()->GetPrefs()->GetList(prefs::kMcpAllowedClients),
            base::ListValue().Append("Claude Code"));

  // Remembered: a new connection isn't asked.
  std::unique_ptr<Client> second = Client::Connect(socket_path_);
  ASSERT_TRUE(second);
  Initialize(*second, "Claude Code");
  std::optional<base::DictValue> text = Call(*second, 2, "get_page_text");
  ASSERT_TRUE(text);
  EXPECT_EQ(text->FindBoolByDottedPath("result.isError"), false);
  EXPECT_EQ(asked_.size(), 2u);
}

IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, TurningItOffCloses) {
  Enable();
  std::unique_ptr<Client> client = Client::Connect(socket_path_);
  ASSERT_TRUE(client);
  Initialize(*client, "test-client");
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return bridge()->session_count_for_testing() == 1; }));

  bridge()->SetProfileEnabled(profile(), false);
  EXPECT_FALSE(bridge()->listening());
  EXPECT_TRUE(client->WaitForClose());
  EXPECT_EQ(bridge()->session_count_for_testing(), 0u);
  EXPECT_TRUE(base::test::RunUntil([&] {
    base::ScopedAllowBlockingForTesting allow_blocking;
    return !base::PathExists(socket_path_);
  }));
}

// compass-mcp, the binary MCP clients run, relays to the bridge; without
// it, it answers each request with what to turn on.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, ThroughCompassMcp) {
  base::FilePath exe_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &exe_dir));
  const base::FilePath binary = exe_dir.Append("compass-mcp");

  auto run = [&]() -> std::pair<base::Process, std::unique_ptr<Client>> {
    int to_child[2];
    int from_child[2];
    EXPECT_EQ(pipe(to_child), 0);
    EXPECT_EQ(pipe(from_child), 0);
    base::CommandLine command(binary);
    command.AppendArg("--user-data-dir=" + dir_.GetPath().value());
    command.AppendArg("--no-launch");
    base::LaunchOptions options;
    options.fds_to_remap.emplace_back(to_child[0], STDIN_FILENO);
    options.fds_to_remap.emplace_back(from_child[1], STDOUT_FILENO);
    base::Process process = base::LaunchProcess(command, options);
    close(to_child[0]);
    close(from_child[1]);
    return {std::move(process),
            std::make_unique<Client>(base::ScopedFD(from_child[0]),
                                     base::ScopedFD(to_child[1]))};
  };

  {
    // Not listening yet.
    auto [process, client] = run();
    ASSERT_TRUE(process.IsValid());
    client->Request(1, "initialize");
    std::optional<base::DictValue> reply = client->Read();
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->FindInt("id"), 1);
    EXPECT_THAT(*reply->FindStringByDottedPath("error.message"),
                HasSubstr("Allow MCP clients"));
    client.reset();
    base::ScopedAllowBlockingForTesting allow_blocking;
    process.WaitForExit(nullptr);
  }

  Enable();
  auto [process, client] = run();
  ASSERT_TRUE(process.IsValid());
  Initialize(*client, "test-client");
  std::optional<base::DictValue> text = Call(*client, 2, "get_page_text");
  ASSERT_TRUE(text);
  EXPECT_THAT(*(*text->FindListByDottedPath("result.content"))[0]
                   .GetDict()
                   .FindString("text"),
              HasSubstr("Hello from the MCP page."));
  // The client goes: compass-mcp follows.
  client.reset();
  base::ScopedAllowBlockingForTesting allow_blocking;
  int exit_code = -1;
  EXPECT_TRUE(process.WaitForExitWithTimeout(base::Seconds(10), &exit_code));
  EXPECT_EQ(exit_code, 0);
}

// One connection serves every profile that allows MCP clients: list_tabs
// lists each, ids route a call to their profile, each profile asks for
// itself, nothing acts across profiles, and turning one off leaves the
// connection to the others.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, ServesEveryEnabledProfile) {
  ProfileManager* manager = g_browser_process->profile_manager();
  Profile* other = &profiles::testing::CreateProfileSync(
      manager, manager->GenerateNextProfileDirectoryPath());
  SpaceDirectory::Get()->List(other);
  ProfileAttributesStorage& storage = manager->GetProfileAttributesStorage();
  storage.GetProfileAttributesWithPath(profile()->GetPath())
      ->SetLocalProfileName(u"Home", /*is_default_name=*/false);
  storage.GetProfileAttributesWithPath(other->GetPath())
      ->SetLocalProfileName(u"Work", /*is_default_name=*/false);
  BrowserWindowInterface* other_window = CreateBrowser(other);
  ASSERT_TRUE(content::NavigateToURL(
      other_window->GetActiveTabInterface()->GetContents(),
      embedded_test_server()->GetURL("/other.html")));

  for (Profile* each : {profile(), other}) {
    each->GetPrefs()->SetBoolean(prefs::kBrowserScope, true);
    each->GetPrefs()->SetList(prefs::kMcpAllowedClients,
                              base::ListValue().Append("trusted"));
  }
  Enable();
  bridge()->SetProfileEnabled(other, true);

  // Allowed in both: list_tabs lists both, under their names.
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  trusted->Request(2, "tools/list");
  std::optional<base::DictValue> listed = trusted->Read();
  ASSERT_TRUE(listed);
  bool has_profile_argument = false;
  for (const base::Value& tool :
       *listed->FindListByDottedPath("result.tools")) {
    if (*tool.GetDict().FindString("name") == "list_tabs") {
      has_profile_argument = !!tool.GetDict().FindDictByDottedPath(
          "inputSchema.properties.profile");
    }
  }
  EXPECT_TRUE(has_profile_argument);

  const std::string list = TextOf(Call(*trusted, 3, "list_tabs"));
  EXPECT_THAT(list, HasSubstr("Profile \"Home\""));
  EXPECT_THAT(list, HasSubstr("Profile \"Work\""));
  const std::string home_id = IdIn(list, "/page.html");
  const std::string work_id = IdIn(list, "/other.html");
  ASSERT_FALSE(home_id.empty()) << list;
  ASSERT_FALSE(work_id.empty()) << list;
  EXPECT_TRUE(asked_.empty());

  // An id picks its profile, whichever window is in front.
  ASSERT_EQ(
      Call(*trusted, 4, "select_tab", base::DictValue().Set("tab_id", work_id))
          ->FindBoolByDottedPath("result.isError"),
      false);
  EXPECT_THAT(TextOf(Call(*trusted, 5, "get_page_text")),
              HasSubstr("Hello from the other profile."));
  ASSERT_EQ(
      Call(*trusted, 6, "select_tab", base::DictValue().Set("tab_id", home_id))
          ->FindBoolByDottedPath("result.isError"),
      false);
  EXPECT_THAT(TextOf(Call(*trusted, 7, "get_page_text")),
              HasSubstr("Hello from the MCP page."));

  // Nothing across profiles.
  std::optional<base::DictValue> split = Call(
      *trusted, 8, "split_tabs",
      base::DictValue().Set("tab_id", home_id).Set("with_tab_id", work_id));
  EXPECT_EQ(split->FindBoolByDottedPath("result.isError"), true);
  EXPECT_THAT(TextOf(split), HasSubstr("across profiles"));

  // profile narrows list_tabs to one.
  const std::string work_only = TextOf(
      Call(*trusted, 9, "list_tabs", base::DictValue().Set("profile", "work")));
  EXPECT_THAT(work_only, HasSubstr("/other.html"));
  EXPECT_THAT(work_only, Not(HasSubstr("/page.html")));
  EXPECT_THAT(TextOf(Call(*trusted, 10, "list_tabs",
                          base::DictValue().Set("profile", "Nobody"))),
              HasSubstr("No profile \"Nobody\""));

  // Another client: each profile asks for itself, and a no in one isn't a
  // no in the other.
  std::unique_ptr<Client> asking = Client::Connect(socket_path_);
  ASSERT_TRUE(asking);
  Initialize(*asking, "asking");
  answer_ = McpApproval::kDeny;
  std::optional<base::DictValue> denied =
      Call(*asking, 2, "select_tab", base::DictValue().Set("tab_id", work_id));
  EXPECT_EQ(denied->FindBoolByDottedPath("result.isError"), true);
  EXPECT_THAT(TextOf(denied), HasSubstr("in profile \"Work\""));
  ASSERT_EQ(asked_profiles_.size(), 1u);
  EXPECT_EQ(asked_profiles_[0], other);
  answer_ = McpApproval::kAllowOnce;
  std::optional<base::DictValue> allowed =
      Call(*asking, 3, "select_tab", base::DictValue().Set("tab_id", home_id));
  EXPECT_EQ(allowed->FindBoolByDottedPath("result.isError"), false);
  ASSERT_EQ(asked_profiles_.size(), 2u);
  EXPECT_EQ(asked_profiles_[1], profile());
  // Not asked again in either.
  Call(*asking, 4, "select_tab", base::DictValue().Set("tab_id", work_id));
  EXPECT_EQ(asked_profiles_.size(), 2u);
  const std::string asking_list = TextOf(Call(*asking, 5, "list_tabs"));
  EXPECT_THAT(asking_list, HasSubstr("didn't allow asking"));

  // Turning one profile off keeps the connection, without it.
  bridge()->SetProfileEnabled(other, false);
  EXPECT_EQ(bridge()->session_count_for_testing(), 2u);
  const std::string after = TextOf(Call(*trusted, 11, "list_tabs"));
  EXPECT_THAT(after, HasSubstr("/page.html"));
  EXPECT_THAT(after, Not(HasSubstr("/other.html")));
  EXPECT_THAT(TextOf(Call(*trusted, 12, "select_tab",
                          base::DictValue().Set("tab_id", work_id))),
              HasSubstr("No tab"));
}

}  // namespace
}  // namespace compass::claude
