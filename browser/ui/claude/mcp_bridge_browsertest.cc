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
          if (self->hold_) {
            // As if the dialog stayed open.
            self->held_ = std::move(done);
            return;
          }
          std::move(done).Run(self->answer_);
        },
        this));
  }

  void TearDownOnMainThread() override {
    asked_profiles_.clear();
    held_.Reset();
    if (work_) {
      bridge()->SetProfileEnabled(work_, false);
      work_ = nullptr;
    }
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

  // A second profile, "Work", with a window on /other.html; this one is
  // "Home". Both allow MCP clients and the whole browser, and always allow
  // the client "trusted".
  void AddWorkProfile() {
    ProfileManager* manager = g_browser_process->profile_manager();
    work_ = &profiles::testing::CreateProfileSync(
        manager, manager->GenerateNextProfileDirectoryPath());
    SpaceDirectory::Get()->List(work_);
    ProfileAttributesStorage& storage = manager->GetProfileAttributesStorage();
    storage.GetProfileAttributesWithPath(profile()->GetPath())
        ->SetLocalProfileName(u"Home", /*is_default_name=*/false);
    storage.GetProfileAttributesWithPath(work_->GetPath())
        ->SetLocalProfileName(u"Work", /*is_default_name=*/false);
    BrowserWindowInterface* work_window = CreateBrowser(work_);
    ASSERT_TRUE(content::NavigateToURL(
        work_window->GetActiveTabInterface()->GetContents(),
        embedded_test_server()->GetURL("/other.html")));
    for (Profile* each : {profile(), work_.get()}) {
      each->GetPrefs()->SetBoolean(prefs::kBrowserScope, true);
      each->GetPrefs()->SetList(prefs::kMcpAllowedClients,
                                base::ListValue().Append("trusted"));
    }
    Enable();
    bridge()->SetProfileEnabled(work_, true);
  }

  // The ids of the Home and Work pages, from a list_tabs of |client|, which
  // must be allowed in both.
  void ReadIds(Client& client, int id) {
    list_ = TextOf(Call(client, id, "list_tabs"));
    home_id_ = IdIn(list_, "/page.html");
    work_id_ = IdIn(list_, "/other.html");
    ASSERT_FALSE(home_id_.empty()) << list_;
    ASSERT_FALSE(work_id_.empty()) << list_;
  }

  // isError of a tools/call reply; fails the test without a reply.
  static std::optional<bool> IsError(
      const std::optional<base::DictValue>& reply) {
    EXPECT_TRUE(reply);
    return reply ? reply->FindBoolByDottedPath("result.isError") : std::nullopt;
  }

  base::ScopedTempDir dir_;
  base::FilePath socket_path_;
  raw_ptr<Profile> work_ = nullptr;
  std::string list_;
  std::string home_id_;
  std::string work_id_;
  bool hold_ = false;
  base::OnceCallback<void(McpApproval)> held_;
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
  ASSERT_NO_FATAL_FAILURE(AddWorkProfile());

  // Allowed in both: list_tabs lists both, under their names.
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  trusted->Request(2, "tools/list");
  std::optional<base::DictValue> listed = trusted->Read();
  ASSERT_TRUE(listed);
  const base::ListValue* tools = listed->FindListByDottedPath("result.tools");
  ASSERT_TRUE(tools);
  bool has_profile_argument = false;
  for (const base::Value& tool : *tools) {
    const std::string* name = tool.GetDict().FindString("name");
    if (name && *name == "list_tabs") {
      has_profile_argument = !!tool.GetDict().FindDictByDottedPath(
          "inputSchema.properties.profile");
    }
  }
  EXPECT_TRUE(has_profile_argument);

  ASSERT_NO_FATAL_FAILURE(ReadIds(*trusted, 3));
  EXPECT_THAT(list_, HasSubstr("Profile \"Home\""));
  EXPECT_THAT(list_, HasSubstr("Profile \"Work\""));
  // Each profile's window shows a space; only the front one is the user's.
  EXPECT_THAT(list_, HasSubstr("[shown in the user's window]"));
  EXPECT_THAT(list_, HasSubstr("[shown in this profile's window]"));
  EXPECT_TRUE(asked_.empty());

  // An id picks its profile, whichever window is in front.
  EXPECT_EQ(IsError(Call(*trusted, 4, "select_tab",
                         base::DictValue().Set("tab_id", work_id_))),
            false);
  EXPECT_THAT(TextOf(Call(*trusted, 5, "get_page_text")),
              HasSubstr("Hello from the other profile."));
  EXPECT_EQ(IsError(Call(*trusted, 6, "select_tab",
                         base::DictValue().Set("tab_id", home_id_))),
            false);
  EXPECT_THAT(TextOf(Call(*trusted, 7, "get_page_text")),
              HasSubstr("Hello from the MCP page."));

  // Nothing across profiles.
  std::optional<base::DictValue> split = Call(
      *trusted, 8, "split_tabs",
      base::DictValue().Set("tab_id", home_id_).Set("with_tab_id", work_id_));
  EXPECT_EQ(IsError(split), true);
  EXPECT_THAT(TextOf(split), HasSubstr("across profiles"));
  EXPECT_EQ(IsError(Call(*trusted, 30, "create_space",
                         base::DictValue()
                             .Set("profile", "Work")
                             .Set("name", "Only at work"))),
            false);
  std::optional<base::DictValue> moved =
      Call(*trusted, 31, "move_tab",
           base::DictValue()
               .Set("tab_id", home_id_)
               .Set("to", "today")
               .Set("space", "Only at work"));
  EXPECT_EQ(IsError(moved), true);
  EXPECT_THAT(TextOf(moved), HasSubstr("which has no space"));

  // profile narrows list_tabs to one.
  const std::string work_only = TextOf(
      Call(*trusted, 9, "list_tabs", base::DictValue().Set("profile", "work")));
  EXPECT_THAT(work_only, HasSubstr("/other.html"));
  EXPECT_THAT(work_only, Not(HasSubstr("/page.html")));
  EXPECT_THAT(TextOf(Call(*trusted, 10, "list_tabs",
                          base::DictValue().Set("profile", "Nobody"))),
              HasSubstr("No profile \"Nobody\""));

  // Another client: each profile asks for itself, and a no in one isn't a
  // no in the other. Allowed nowhere yet, it isn't told profile names.
  std::unique_ptr<Client> asking = Client::Connect(socket_path_);
  ASSERT_TRUE(asking);
  Initialize(*asking, "asking");
  std::optional<base::DictValue> nobody = Call(
      *asking, 20, "list_tabs", base::DictValue().Set("profile", "Nobody"));
  EXPECT_EQ(IsError(nobody), true);
  EXPECT_THAT(TextOf(nobody), Not(HasSubstr("Home")));
  EXPECT_TRUE(asked_profiles_.empty());
  answer_ = McpApproval::kDeny;
  std::optional<base::DictValue> denied =
      Call(*asking, 2, "select_tab", base::DictValue().Set("tab_id", work_id_));
  EXPECT_EQ(IsError(denied), true);
  EXPECT_THAT(TextOf(denied),
              HasSubstr("didn't allow asking to control Compass."));
  EXPECT_THAT(TextOf(denied), Not(HasSubstr("Work")));
  ASSERT_EQ(asked_profiles_.size(), 1u);
  EXPECT_EQ(asked_profiles_[0], work_);
  answer_ = McpApproval::kAllowOnce;
  EXPECT_EQ(IsError(Call(*asking, 3, "select_tab",
                         base::DictValue().Set("tab_id", home_id_))),
            false);
  ASSERT_EQ(asked_profiles_.size(), 2u);
  EXPECT_EQ(asked_profiles_[1], profile());
  // Not asked again in either; allowed in Home, it may know the names now.
  std::optional<base::DictValue> denied_again =
      Call(*asking, 4, "select_tab", base::DictValue().Set("tab_id", work_id_));
  EXPECT_EQ(IsError(denied_again), true);
  EXPECT_THAT(TextOf(denied_again), HasSubstr("in profile \"Work\""));
  EXPECT_EQ(asked_profiles_.size(), 2u);
  EXPECT_THAT(TextOf(Call(*asking, 5, "list_tabs")),
              HasSubstr("didn't allow asking"));

  // Turning one profile off keeps the connection, without it.
  bridge()->SetProfileEnabled(work_, false);
  EXPECT_EQ(bridge()->session_count_for_testing(), 2u);
  const std::string after = TextOf(Call(*trusted, 11, "list_tabs"));
  EXPECT_THAT(after, HasSubstr("/page.html"));
  EXPECT_THAT(after, Not(HasSubstr("/other.html")));
  EXPECT_THAT(TextOf(Call(*trusted, 12, "select_tab",
                          base::DictValue().Set("tab_id", work_id_))),
              HasSubstr("No tab"));
}

// The page and developer tools act in the profile of the tab they act on,
// whatever profile they're given; so do tools that fall back to that tab.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, TargetsProfileWins) {
  ASSERT_NO_FATAL_FAILURE(AddWorkProfile());
  work_->GetPrefs()->SetBoolean(prefs::kDevTools, true);
  profile()->GetPrefs()->SetBoolean(prefs::kDevTools, false);
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  ASSERT_NO_FATAL_FAILURE(ReadIds(*trusted, 2));
  EXPECT_EQ(IsError(Call(*trusted, 3, "select_tab",
                         base::DictValue().Set("tab_id", home_id_))),
            false);

  // Home's setting, not Work's.
  std::optional<base::DictValue> storage = Call(
      *trusted, 4, "page_storage", base::DictValue().Set("profile", "Work"));
  EXPECT_EQ(IsError(storage), true);
  EXPECT_THAT(TextOf(storage), HasSubstr("developer tools"));
  // Home's page, read under Home's checks.
  EXPECT_THAT(TextOf(Call(*trusted, 5, "get_page_text",
                          base::DictValue().Set("profile", "Work"))),
              HasSubstr("Hello from the MCP page."));

  // The Home tab isn't sent through Work's devices.
  std::optional<base::DictValue> sent =
      Call(*trusted, 6, "send_tab_to_device",
           base::DictValue().Set("profile", "Work").Set("device", "Phone"));
  EXPECT_EQ(IsError(sent), true);
  EXPECT_THAT(TextOf(sent), HasSubstr("isn't in profile \"Work\""));
}

// Turning MCP off in a profile while its question is open drops the
// question and refuses the call it held; later calls go on in the profiles
// left.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, TurningOffDropsTheQuestion) {
  ASSERT_NO_FATAL_FAILURE(AddWorkProfile());
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  ASSERT_NO_FATAL_FAILURE(ReadIds(*trusted, 2));

  std::unique_ptr<Client> newcomer = Client::Connect(socket_path_);
  ASSERT_TRUE(newcomer);
  Initialize(*newcomer, "newcomer");
  hold_ = true;
  newcomer->Request(
      2, "tools/call",
      base::DictValue()
          .Set("name", "select_tab")
          .Set("arguments", base::DictValue().Set("tab_id", work_id_)));
  ASSERT_TRUE(base::test::RunUntil([&] { return !held_.is_null(); }));
  ASSERT_EQ(asked_profiles_.size(), 1u);
  EXPECT_EQ(asked_profiles_[0], work_);

  hold_ = false;
  answer_ = McpApproval::kAllowOnce;
  bridge()->SetProfileEnabled(work_, false);
  work_ = nullptr;
  // Refused, without asking elsewhere, and without the profile's name: the
  // client was allowed nowhere.
  std::optional<base::DictValue> reply = newcomer->Read();
  EXPECT_EQ(IsError(reply), true);
  EXPECT_EQ(TextOf(reply), "A profile stopped allowing MCP clients.");
  EXPECT_EQ(asked_profiles_.size(), 1u);

  // The dropped question's late answer changes nothing; Home asks for
  // itself.
  std::move(held_).Run(McpApproval::kAlwaysAllow);
  EXPECT_EQ(IsError(Call(*newcomer, 3, "get_page_text")), false);
  ASSERT_EQ(asked_profiles_.size(), 2u);
  EXPECT_EQ(asked_profiles_[1], profile());
}

// A picked tab whose profile stops allowing MCP clients is gone for the
// client, as a closed tab: the page tools don't fall back to the user's tab
// in another profile.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, PickedTabGoesWithItsProfile) {
  ASSERT_NO_FATAL_FAILURE(AddWorkProfile());
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  ASSERT_NO_FATAL_FAILURE(ReadIds(*trusted, 2));
  EXPECT_EQ(IsError(Call(*trusted, 3, "select_tab",
                         base::DictValue().Set("tab_id", work_id_))),
            false);

  bridge()->SetProfileEnabled(work_, false);
  work_ = nullptr;
  std::optional<base::DictValue> text = Call(*trusted, 4, "get_page_text");
  EXPECT_EQ(IsError(text), true);
  EXPECT_THAT(TextOf(text), HasSubstr("closed"));
  EXPECT_THAT(TextOf(text), Not(HasSubstr("Hello from the MCP page.")));

  // Picking a tab again works.
  EXPECT_EQ(IsError(Call(*trusted, 5, "select_tab",
                         base::DictValue().Set("tab_id", home_id_))),
            false);
  EXPECT_THAT(TextOf(Call(*trusted, 6, "get_page_text")),
              HasSubstr("Hello from the MCP page."));
}

// A switch_space denied in the space's profile is refused there, without a
// second question about swapping the front profile's window.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, DeniedSwitchAsksOnce) {
  ASSERT_NO_FATAL_FAILURE(AddWorkProfile());
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  ASSERT_NO_FATAL_FAILURE(ReadIds(*trusted, 2));
  // The space goes in the profile that isn't in front, whose window a
  // switch would swap: that's the question that mustn't follow a Deny.
  const bool home_in_front =
      TextOf(Call(*trusted, 3, "get_page_text"))
          .find("Hello from the MCP page.") != std::string::npos;
  Profile* space_profile = home_in_front ? work_.get() : profile();
  EXPECT_EQ(IsError(Call(*trusted, 4, "create_space",
                         base::DictValue()
                             .Set("profile", home_in_front ? "Work" : "Home")
                             .Set("name", "Only at work"))),
            false);

  std::unique_ptr<Client> asking = Client::Connect(socket_path_);
  ASSERT_TRUE(asking);
  Initialize(*asking, "asking");
  answer_ = McpApproval::kDeny;
  std::optional<base::DictValue> denied =
      Call(*asking, 2, "switch_space",
           base::DictValue().Set("space", "Only at work"));
  EXPECT_EQ(IsError(denied), true);
  EXPECT_THAT(TextOf(denied), HasSubstr("didn't allow"));
  ASSERT_EQ(asked_profiles_.size(), 1u);
  EXPECT_EQ(asked_profiles_[0], space_profile);
}

// switch_space's swap, when the front profile has no window to ask in,
// isn't a refusal: the space's profile's own window switches.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, SwapWithoutWindowStillSwitches) {
  ASSERT_NO_FATAL_FAILURE(AddWorkProfile());
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  ASSERT_NO_FATAL_FAILURE(ReadIds(*trusted, 2));
  const bool home_in_front =
      TextOf(Call(*trusted, 3, "get_page_text"))
          .find("Hello from the MCP page.") != std::string::npos;
  Profile* space_profile = home_in_front ? work_.get() : profile();
  Profile* front_profile = home_in_front ? profile() : work_.get();
  EXPECT_EQ(IsError(Call(*trusted, 4, "create_space",
                         base::DictValue()
                             .Set("profile", home_in_front ? "Work" : "Home")
                             .Set("name", "Elsewhere"))),
            false);
  // "half" is always allowed in the space's profile only: the swap is the
  // one question.
  space_profile->GetPrefs()->SetList(
      prefs::kMcpAllowedClients,
      base::ListValue().Append("trusted").Append("half"));

  std::unique_ptr<Client> half = Client::Connect(socket_path_);
  ASSERT_TRUE(half);
  Initialize(*half, "half");
  answer_ = McpApproval::kNoWindow;
  std::optional<base::DictValue> switched = Call(
      *half, 2, "switch_space", base::DictValue().Set("space", "Elsewhere"));
  EXPECT_EQ(IsError(switched), false) << TextOf(switched);
  ASSERT_EQ(asked_profiles_.size(), 1u);
  EXPECT_EQ(asked_profiles_[0], front_profile);
}

// A space name isn't looked up where the client was denied: the answer
// doesn't tell it that profile has such a space.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, DeniedProfilesSpacesStayHidden) {
  ASSERT_NO_FATAL_FAILURE(AddWorkProfile());
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  ASSERT_NO_FATAL_FAILURE(ReadIds(*trusted, 2));
  EXPECT_EQ(
      IsError(Call(
          *trusted, 3, "create_space",
          base::DictValue().Set("profile", "Work").Set("name", "Acquisition"))),
      false);

  std::unique_ptr<Client> asking = Client::Connect(socket_path_);
  ASSERT_TRUE(asking);
  Initialize(*asking, "asking");
  answer_ = McpApproval::kDeny;
  EXPECT_EQ(IsError(Call(*asking, 2, "select_tab",
                         base::DictValue().Set("tab_id", work_id_))),
            true);
  answer_ = McpApproval::kAllowOnce;
  EXPECT_EQ(IsError(Call(*asking, 3, "select_tab",
                         base::DictValue().Set("tab_id", home_id_))),
            false);
  const size_t asked = asked_profiles_.size();

  std::optional<base::DictValue> probe =
      Call(*asking, 4, "switch_space",
           base::DictValue().Set("space", "Acquisition"));
  EXPECT_EQ(IsError(probe), true);
  EXPECT_THAT(TextOf(probe), HasSubstr("No space \"Acquisition\""));
  EXPECT_THAT(TextOf(probe), Not(HasSubstr("Work")));
  EXPECT_EQ(asked_profiles_.size(), asked);
}

// move_tab reads space only to pin or add to Today: elsewhere an unused
// space doesn't route the call, or fail it.
IN_PROC_BROWSER_TEST_F(McpBridgeBrowserTest, MoveTabIgnoresUnusedSpace) {
  ASSERT_NO_FATAL_FAILURE(AddWorkProfile());
  std::unique_ptr<Client> trusted = Client::Connect(socket_path_);
  ASSERT_TRUE(trusted);
  Initialize(*trusted, "trusted");
  ASSERT_NO_FATAL_FAILURE(ReadIds(*trusted, 2));
  std::optional<base::DictValue> moved =
      Call(*trusted, 3, "move_tab",
           base::DictValue()
               .Set("tab_id", work_id_)
               .Set("to", "favorites")
               .Set("space", "No such space"));
  EXPECT_EQ(IsError(moved), false) << TextOf(moved);
}

}  // namespace
}  // namespace compass::claude
