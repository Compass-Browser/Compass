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
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::claude {
namespace {

using ::testing::HasSubstr;

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
              "<title>MCP page</title><p>Hello from the MCP page.");
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
          std::move(done).Run(self->answer_);
        },
        this));
  }

  void TearDownOnMainThread() override {
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

  std::optional<base::DictValue> Call(Client& client,
                                      int id,
                                      const std::string& tool) {
    client.Request(id, "tools/call",
                   base::DictValue()
                       .Set("name", tool)
                       .Set("arguments", base::DictValue()));
    return client.Read();
  }

  base::ScopedTempDir dir_;
  base::FilePath socket_path_;
  std::vector<std::string> asked_;
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

}  // namespace
}  // namespace compass::claude
