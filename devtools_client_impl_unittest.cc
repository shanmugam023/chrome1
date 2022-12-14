// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/chromedriver/chrome/devtools_client_impl.h"

#include <list>
#include <memory>
#include <string>

#include "base/bind.h"
#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/test/chromedriver/chrome/devtools_event_listener.h"
#include "chrome/test/chromedriver/chrome/status.h"
#include "chrome/test/chromedriver/net/sync_websocket.h"
#include "chrome/test/chromedriver/net/sync_websocket_factory.h"
#include "chrome/test/chromedriver/net/timeout.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace {
class MockSyncWebSocket : public SyncWebSocket {
 public:
  MockSyncWebSocket()
      : connected_(false),
        id_(-1),
        queued_messages_(3),
        add_script_received_(false),
        runtime_eval_received_(false),
        connect_complete_(false) {}
  ~MockSyncWebSocket() override {}

  bool IsConnected() override { return connected_; }

  bool Connect(const GURL& url) override {
    EXPECT_STREQ("http://url/", url.possibly_invalid_spec().c_str());
    connected_ = true;
    return true;
  }

  bool Send(const std::string& message) override {
    EXPECT_TRUE(connected_);
    std::unique_ptr<base::DictionaryValue> dict;
    std::string method;
    if (SendHelper(message, &dict, &method)) {
      EXPECT_STREQ("method", method.c_str());
      base::DictionaryValue* params = nullptr;
      EXPECT_TRUE(dict->GetDictionary("params", &params));
      if (!params)
        return false;
      int param = params->GetDict().FindInt("param").value_or(-1);
      EXPECT_EQ(1, param);
    }
    return true;
  }

  /** Completes standard Send processing for ConnectIfNecessary. Returns true
   *  if connection is complete, or false if connection is still pending.
   */
  bool SendHelper(const std::string& message,
                  std::unique_ptr<base::DictionaryValue>* dict,
                  std::string* method) {
    std::unique_ptr<base::Value> value =
        base::JSONReader::ReadDeprecated(message);
    base::DictionaryValue* temp_dict;
    EXPECT_TRUE(value->GetAsDictionary(&temp_dict));
    *dict = base::DictionaryValue::From(
        base::Value::ToUniquePtrValue(temp_dict->Clone()));
    if (!dict)
      return false;
    absl::optional<int> maybe_id = (*dict)->GetDict().FindInt("id");
    EXPECT_TRUE(maybe_id);
    if (!maybe_id)
      return false;
    id_ = *maybe_id;
    EXPECT_TRUE((*dict)->GetString("method", method));
    // Because ConnectIfNecessary is not waiting for the response, Send can
    // set connect_complete to true
    if (add_script_received_ && runtime_eval_received_)
      connect_complete_ = true;
    if (connect_complete_)
      return true;
    else if (*method == "Page.addScriptToEvaluateOnNewDocument")
      add_script_received_ = true;
    else if (*method == "Runtime.evaluate")
      runtime_eval_received_ = true;
    return false;
  }

  SyncWebSocket::StatusCode ReceiveNextMessage(
      std::string* message,
      const Timeout& timeout) override {
    if (timeout.IsExpired())
      return SyncWebSocket::StatusCode::kTimeout;
    if (ReceiveHelper(message)) {
      base::Value::Dict response;
      response.Set("id", id_);
      base::Value result{base::Value::Type::DICT};
      result.GetDict().Set("param", 1);
      response.Set("result", result.Clone());
      base::JSONWriter::Write(base::Value(std::move(response)), message);
    }
    --queued_messages_;
    return SyncWebSocket::StatusCode::kOk;
  }

  /** Completes standard Receive processing for ConnectIfNecessary. Returns true
   *  if connection is complete, or false if connection is still pending.
   */
  bool ReceiveHelper(std::string* message) {
    if (connect_complete_) {
      return true;
    } else if (add_script_received_ && runtime_eval_received_) {
      connect_complete_ = true;
    }
    // Handle connectIfNecessary commands
    base::Value::Dict response;
    response.Set("id", id_);
    base::Value result{base::Value::Type::DICT};
    result.GetDict().Set("param", 1);
    response.Set("result", result.Clone());
    base::JSONWriter::Write(base::Value(std::move(response)), message);
    return false;
  }

  bool HasNextMessage() override { return queued_messages_ > 0; }

 protected:
  bool connected_;
  int id_;
  int queued_messages_;
  bool add_script_received_;
  bool runtime_eval_received_;
  bool connect_complete_;
};

template <typename T>
std::unique_ptr<SyncWebSocket> CreateMockSyncWebSocket() {
  return std::unique_ptr<SyncWebSocket>(new T());
}

class DevToolsClientImplTest : public testing::Test {
 protected:
  DevToolsClientImplTest() : long_timeout_(base::Minutes(5)) {}

  const base::TimeDelta long_timeout_;
};

}  // namespace

TEST_F(DevToolsClientImplTest, SendCommand) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  base::DictionaryValue params;
  params.GetDict().Set("param", 1);
  ASSERT_EQ(kOk, client.SendCommand("method", params).code());
}

TEST_F(DevToolsClientImplTest, SendCommandAndGetResult) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  base::DictionaryValue params;
  params.GetDict().Set("param", 1);
  base::Value result;
  Status status = client.SendCommandAndGetResult("method", params, &result);
  ASSERT_EQ(kOk, status.code());
  std::string json;
  base::JSONWriter::Write(result, &json);
  ASSERT_STREQ("{\"param\":1}", json.c_str());
}

namespace {

class MockSyncWebSocket2 : public SyncWebSocket {
 public:
  MockSyncWebSocket2() = default;
  ~MockSyncWebSocket2() override = default;

  bool IsConnected() override { return false; }

  bool Connect(const GURL& url) override { return false; }

  bool Send(const std::string& message) override {
    EXPECT_TRUE(false);
    return false;
  }

  SyncWebSocket::StatusCode ReceiveNextMessage(
      std::string* message,
      const Timeout& timeout) override {
    EXPECT_TRUE(false);
    return SyncWebSocket::StatusCode::kDisconnected;
  }

  bool HasNextMessage() override { return true; }
};

}  // namespace

TEST_F(DevToolsClientImplTest, ConnectIfNecessaryConnectFails) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket2>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kDisconnected, client.ConnectIfNecessary().code());
}

namespace {

class MockSyncWebSocket3 : public MockSyncWebSocket {
 public:
  explicit MockSyncWebSocket3(bool send_returns_after_connect)
      : send_returns_after_connect_(send_returns_after_connect) {}
  ~MockSyncWebSocket3() override = default;

  bool IsConnected() override { return connected_; }

  bool Connect(const GURL& url) override {
    connected_ = true;
    return true;
  }

  bool Send(const std::string& message) override {
    std::string method;
    std::unique_ptr<base::DictionaryValue> dict;
    if (SendHelper(message, &dict, &method)) {
      return send_returns_after_connect_;
    }
    return true;
  }

  SyncWebSocket::StatusCode ReceiveNextMessage(
      std::string* message,
      const Timeout& timeout) override {
    if (ReceiveHelper(message)) {
      return SyncWebSocket::StatusCode::kDisconnected;
    } else {
      return SyncWebSocket::StatusCode::kOk;
    }
  }

  bool HasNextMessage() override { return true; }

 private:
  bool connected_ = false;
  bool send_returns_after_connect_;
};

template <typename T>
std::unique_ptr<SyncWebSocket> CreateMockSyncWebSocket_B(bool b1) {
  return std::unique_ptr<SyncWebSocket>(new T(b1));
}

}  // namespace

TEST_F(DevToolsClientImplTest, SendCommandSendFails) {
  SyncWebSocketFactory factory = base::BindRepeating(
      &CreateMockSyncWebSocket_B<MockSyncWebSocket3>, false);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  base::DictionaryValue params;
  ASSERT_TRUE(client.SendCommand("method", params).IsError());
}

TEST_F(DevToolsClientImplTest, SendCommandReceiveNextMessageFails) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket_B<MockSyncWebSocket3>, true);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  base::DictionaryValue params;
  ASSERT_TRUE(client.SendCommand("method", params).IsError());
}

namespace {

class FakeSyncWebSocket : public MockSyncWebSocket {
 public:
  FakeSyncWebSocket() = default;
  ~FakeSyncWebSocket() override = default;

  bool IsConnected() override { return connected_; }

  bool Connect(const GURL& url) override {
    EXPECT_FALSE(connected_);
    connected_ = true;
    return true;
  }

  bool Send(const std::string& message) override {
    std::unique_ptr<base::DictionaryValue> dict;
    std::string method;
    SendHelper(message, &dict, &method);
    return true;
  }

  SyncWebSocket::StatusCode ReceiveNextMessage(
      std::string* message,
      const Timeout& timeout) override {
    ReceiveHelper(message);
    return SyncWebSocket::StatusCode::kOk;
  }

  bool HasNextMessage() override { return true; }

 private:
  bool connected_ = false;
};

bool ReturnCommand(const std::string& message,
                   int expected_id,
                   std::string* session_id,
                   internal::InspectorMessageType* type,
                   internal::InspectorEvent* event,
                   internal::InspectorCommandResponse* command_response) {
  *type = internal::kCommandResponseMessageType;
  session_id->clear();
  command_response->id = expected_id;
  command_response->result = std::make_unique<base::DictionaryValue>();
  return true;
}

bool ReturnBadResponse(const std::string& message,
                       int expected_id,
                       std::string* session_id,
                       internal::InspectorMessageType* type,
                       internal::InspectorEvent* event,
                       internal::InspectorCommandResponse* command_response) {
  *type = internal::kCommandResponseMessageType;
  session_id->clear();
  command_response->id = expected_id;
  command_response->result = std::make_unique<base::DictionaryValue>();
  return false;
}

bool ReturnCommandBadId(const std::string& message,
                        int expected_id,
                        std::string* session_id,
                        internal::InspectorMessageType* type,
                        internal::InspectorEvent* event,
                        internal::InspectorCommandResponse* command_response) {
  *type = internal::kCommandResponseMessageType;
  session_id->clear();
  command_response->id = expected_id + 100;
  command_response->result = std::make_unique<base::DictionaryValue>();
  return true;
}

bool ReturnUnexpectedIdThenResponse(
    bool* first,
    const std::string& message,
    int expected_id,
    std::string* session_id,
    internal::InspectorMessageType* type,
    internal::InspectorEvent* event,
    internal::InspectorCommandResponse* command_response) {
  session_id->clear();
  if (*first) {
    *type = internal::kCommandResponseMessageType;
    command_response->id = expected_id + 100;
    command_response->error = "{\"code\":-32001,\"message\":\"ERR\"}";
  } else {
    *type = internal::kCommandResponseMessageType;
    command_response->id = expected_id;
    base::DictionaryValue params;
    command_response->result = std::make_unique<base::DictionaryValue>();
    command_response->result->GetDict().Set("key", 2);
  }
  *first = false;
  return true;
}

bool ReturnCommandError(const std::string& message,
                        int expected_id,
                        std::string* session_id,
                        internal::InspectorMessageType* type,
                        internal::InspectorEvent* event,
                        internal::InspectorCommandResponse* command_response) {
  *type = internal::kCommandResponseMessageType;
  session_id->clear();
  command_response->id = expected_id;
  command_response->error = "err";
  return true;
}

class MockListener : public DevToolsEventListener {
 public:
  MockListener() : called_(false) {}
  ~MockListener() override { EXPECT_TRUE(called_); }

  Status OnConnected(DevToolsClient* client) override { return Status(kOk); }

  Status OnEvent(DevToolsClient* client,
                 const std::string& method,
                 const base::DictionaryValue& params) override {
    called_ = true;
    EXPECT_STREQ("method", method.c_str());
    EXPECT_TRUE(params.GetDict().Find("key"));
    return Status(kOk);
  }

 private:
  bool called_;
};

bool ReturnEventThenResponse(
    bool* first,
    const std::string& message,
    int expected_id,
    std::string* session_id,
    internal::InspectorMessageType* type,
    internal::InspectorEvent* event,
    internal::InspectorCommandResponse* command_response) {
  session_id->clear();
  if (*first) {
    *type = internal::kEventMessageType;
    event->method = "method";
    event->params = std::make_unique<base::DictionaryValue>();
    event->params->GetDict().Set("key", 1);
  } else {
    *type = internal::kCommandResponseMessageType;
    command_response->id = expected_id;
    base::DictionaryValue params;
    command_response->result = std::make_unique<base::DictionaryValue>();
    command_response->result->GetDict().Set("key", 2);
  }
  *first = false;
  return true;
}

bool ReturnEvent(const std::string& message,
                 int expected_id,
                 std::string* session_id,
                 internal::InspectorMessageType* type,
                 internal::InspectorEvent* event,
                 internal::InspectorCommandResponse* command_response) {
  *type = internal::kEventMessageType;
  event->method = "method";
  event->params = std::make_unique<base::DictionaryValue>();
  event->params->GetDict().Set("key", 1);
  return true;
}

bool ReturnOutOfOrderResponses(
    int* recurse_count,
    DevToolsClient* client,
    const std::string& message,
    int expected_id,
    std::string* session_id,
    internal::InspectorMessageType* type,
    internal::InspectorEvent* event,
    internal::InspectorCommandResponse* command_response) {
  int key = 0;
  base::DictionaryValue params;
  params.GetDict().Set("param", 1);
  switch ((*recurse_count)++) {
    case 0:
      client->SendCommand("method", params);
      *type = internal::kEventMessageType;
      event->method = "method";
      event->params = std::make_unique<base::DictionaryValue>();
      event->params->GetDict().Set("key", 1);
      return true;
    case 1:
      command_response->id = expected_id - 1;
      key = 2;
      break;
    case 2:
      command_response->id = expected_id;
      key = 3;
      break;
  }
  *type = internal::kCommandResponseMessageType;
  command_response->result = std::make_unique<base::DictionaryValue>();
  command_response->result->GetDict().Set("key", key);
  return true;
}

bool ReturnError(const std::string& message,
                 int expected_id,
                 std::string* session_id,
                 internal::InspectorMessageType* type,
                 internal::InspectorEvent* event,
                 internal::InspectorCommandResponse* command_response) {
  return false;
}

Status AlwaysTrue(bool* is_met) {
  *is_met = true;
  return Status(kOk);
}

Status AlwaysError(bool* is_met) {
  return Status(kUnknownError);
}

}  // namespace

TEST_F(DevToolsClientImplTest, SendCommandOnlyConnectsOnce) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<FakeSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnCommand));
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  base::DictionaryValue params;
  ASSERT_TRUE(client.SendCommand("method", params).IsOk());
  ASSERT_TRUE(client.SendCommand("method", params).IsOk());
}

TEST_F(DevToolsClientImplTest, SendCommandBadResponse) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<FakeSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnBadResponse));
  base::DictionaryValue params;
  ASSERT_TRUE(client.SendCommand("method", params).IsError());
}

TEST_F(DevToolsClientImplTest, SendCommandBadId) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<FakeSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnCommandBadId));
  base::DictionaryValue params;
  ASSERT_TRUE(client.SendCommand("method", params).IsError());
}

TEST_F(DevToolsClientImplTest, SendCommandUnexpectedId) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<FakeSyncWebSocket>);
  bool first = true;
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(
      base::BindRepeating(&ReturnUnexpectedIdThenResponse, &first));
  base::DictionaryValue params;
  ASSERT_TRUE(client.SendCommand("method", params).IsOk());
}

TEST_F(DevToolsClientImplTest, SendCommandResponseError) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<FakeSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnCommandError));
  base::DictionaryValue params;
  ASSERT_TRUE(client.SendCommand("method", params).IsError());
}

TEST_F(DevToolsClientImplTest, SendCommandEventBeforeResponse) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<FakeSyncWebSocket>);
  MockListener listener;
  bool first = true;
  DevToolsClientImpl client("id", "", "http://url", factory);
  client.AddListener(&listener);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(
      base::BindRepeating(&ReturnEventThenResponse, &first));
  base::DictionaryValue params;
  base::Value result;
  ASSERT_TRUE(client.SendCommandAndGetResult("method", params, &result).IsOk());
  ASSERT_TRUE(result.is_dict());
  absl::optional<int> key = result.GetDict().FindInt("key");
  ASSERT_TRUE(key);
  ASSERT_EQ(2, key.value());
}

TEST(ParseInspectorMessage, NonJson) {
  internal::InspectorMessageType type;
  internal::InspectorEvent event;
  internal::InspectorCommandResponse response;
  std::string session_id;
  ASSERT_FALSE(internal::ParseInspectorMessage("hi", 0, &session_id, &type,
                                               &event, &response));
}

TEST(ParseInspectorMessage, NeitherCommandNorEvent) {
  internal::InspectorMessageType type;
  internal::InspectorEvent event;
  internal::InspectorCommandResponse response;
  std::string session_id;
  ASSERT_FALSE(internal::ParseInspectorMessage("{}", 0, &session_id, &type,
                                               &event, &response));
}

TEST(ParseInspectorMessage, EventNoParams) {
  internal::InspectorMessageType type;
  internal::InspectorEvent event;
  internal::InspectorCommandResponse response;
  std::string session_id;
  ASSERT_TRUE(internal::ParseInspectorMessage(
      "{\"method\":\"method\"}", 0, &session_id, &type, &event, &response));
  ASSERT_EQ(internal::kEventMessageType, type);
  ASSERT_STREQ("method", event.method.c_str());
  ASSERT_TRUE(event.params->is_dict());
}

TEST(ParseInspectorMessage, EventNoParamsWithSessionId) {
  internal::InspectorMessageType type;
  internal::InspectorEvent event;
  internal::InspectorCommandResponse response;
  std::string session_id;
  ASSERT_TRUE(internal::ParseInspectorMessage(
      "{\"method\":\"method\",\"sessionId\":\"B221AF2\"}", 0, &session_id,
      &type, &event, &response));
  ASSERT_EQ(internal::kEventMessageType, type);
  ASSERT_STREQ("method", event.method.c_str());
  ASSERT_TRUE(event.params->is_dict());
  EXPECT_EQ("B221AF2", session_id);
}

TEST(ParseInspectorMessage, EventWithParams) {
  internal::InspectorMessageType type;
  internal::InspectorEvent event;
  internal::InspectorCommandResponse response;
  std::string session_id;
  ASSERT_TRUE(internal::ParseInspectorMessage(
      "{\"method\":\"method\",\"params\":{\"key\":100},\"sessionId\":\"AB3A\"}",
      0, &session_id, &type, &event, &response));
  ASSERT_EQ(internal::kEventMessageType, type);
  ASSERT_STREQ("method", event.method.c_str());
  int key = event.params->GetDict().FindInt("key").value_or(-1);
  ASSERT_EQ(100, key);
  EXPECT_EQ("AB3A", session_id);
}

TEST(ParseInspectorMessage, CommandNoErrorOrResult) {
  internal::InspectorMessageType type;
  internal::InspectorEvent event;
  internal::InspectorCommandResponse response;
  std::string session_id;
  // As per Chromium issue 392577, DevTools does not necessarily return a
  // "result" dictionary for every valid response. If neither "error" nor
  // "result" keys are present, a blank result dictionary should be inferred.
  ASSERT_TRUE(
      internal::ParseInspectorMessage("{\"id\":1,\"sessionId\":\"AB2AF3C\"}", 0,
                                      &session_id, &type, &event, &response));
  ASSERT_TRUE(response.result->DictEmpty());
  EXPECT_EQ("AB2AF3C", session_id);
}

TEST(ParseInspectorMessage, CommandError) {
  internal::InspectorMessageType type;
  internal::InspectorEvent event;
  internal::InspectorCommandResponse response;
  std::string session_id;
  ASSERT_TRUE(internal::ParseInspectorMessage(
      "{\"id\":1,\"error\":{}}", 0, &session_id, &type, &event, &response));
  ASSERT_EQ(internal::kCommandResponseMessageType, type);
  ASSERT_EQ(1, response.id);
  ASSERT_TRUE(response.error.length());
  ASSERT_FALSE(response.result);
}

TEST(ParseInspectorMessage, Command) {
  internal::InspectorMessageType type;
  internal::InspectorEvent event;
  internal::InspectorCommandResponse response;
  std::string session_id;
  ASSERT_TRUE(
      internal::ParseInspectorMessage("{\"id\":1,\"result\":{\"key\":1}}", 0,
                                      &session_id, &type, &event, &response));
  ASSERT_EQ(internal::kCommandResponseMessageType, type);
  ASSERT_EQ(1, response.id);
  ASSERT_FALSE(response.error.length());
  int key = response.result->GetDict().FindInt("key").value_or(-1);
  ASSERT_EQ(1, key);
}

TEST(ParseInspectorError, EmptyError) {
  Status status = internal::ParseInspectorError("");
  ASSERT_EQ(kUnknownError, status.code());
  ASSERT_EQ("unknown error: inspector error with no error message",
            status.message());
}

TEST(ParseInspectorError, InvalidUrlError) {
  Status status = internal::ParseInspectorError(
      "{\"message\": \"Cannot navigate to invalid URL\"}");
  ASSERT_EQ(kInvalidArgument, status.code());
}

TEST(ParseInspectorError, InvalidArgumentCode) {
  Status status = internal::ParseInspectorError(
      "{\"code\": -32602, \"message\": \"Error description\"}");
  ASSERT_EQ(kInvalidArgument, status.code());
  ASSERT_EQ("invalid argument: Error description", status.message());
}

TEST(ParseInspectorError, UnknownError) {
  const std::string error("{\"code\": 10, \"message\": \"Error description\"}");
  Status status = internal::ParseInspectorError(error);
  ASSERT_EQ(kUnknownError, status.code());
  ASSERT_EQ("unknown error: unhandled inspector error: " + error,
            status.message());
}

TEST(ParseInspectorError, CdpNotImplementedError) {
  const std::string error("{\"code\":-32601,\"message\":\"SOME MESSAGE\"}");
  Status status = internal::ParseInspectorError(error);
  ASSERT_EQ(kUnknownCommand, status.code());
  ASSERT_EQ("unknown command: SOME MESSAGE", status.message());
}

TEST(ParseInspectorError, NoSuchFrameError) {
  // As the server returns the generic error code: SERVER_ERROR = -32000
  // we have to rely on the error message content.
  // A real scenario where this error message occurs is WPT test:
  // 'cookies/samesite/iframe-reload.https.html'
  // The error is thrown by InspectorDOMAgent::getFrameOwner
  // (inspector_dom_agent.cc).
  const std::string error(
      "{\"code\":-32000,"
      "\"message\":\"Frame with the given id was not found.\"}");
  Status status = internal::ParseInspectorError(error);
  ASSERT_EQ(kNoSuchFrame, status.code());
  ASSERT_EQ("no such frame: Frame with the given id was not found.",
            status.message());
}

TEST(ParseInspectorError, SessionNotFoundError) {
  const std::string error("{\"code\":-32001,\"message\":\"SOME MESSAGE\"}");
  Status status = internal::ParseInspectorError(error);
  ASSERT_EQ(kNoSuchFrame, status.code());
  ASSERT_EQ("no such frame: SOME MESSAGE", status.message());
}

TEST_F(DevToolsClientImplTest, HandleEventsUntil) {
  MockListener listener;
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  client.AddListener(&listener);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnEvent));
  Status status = client.HandleEventsUntil(base::BindRepeating(&AlwaysTrue),
                                           Timeout(long_timeout_));
  ASSERT_EQ(kOk, status.code());
}

TEST_F(DevToolsClientImplTest, HandleEventsUntilTimeout) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnEvent));
  Status status = client.HandleEventsUntil(base::BindRepeating(&AlwaysTrue),
                                           Timeout(base::TimeDelta()));
  ASSERT_EQ(kTimeout, status.code());
}

TEST_F(DevToolsClientImplTest, WaitForNextEventCommand) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnCommand));
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  Status status = client.HandleEventsUntil(base::BindRepeating(&AlwaysTrue),
                                           Timeout(long_timeout_));
  ASSERT_EQ(kUnknownError, status.code());
}

TEST_F(DevToolsClientImplTest, WaitForNextEventError) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnError));
  Status status = client.HandleEventsUntil(base::BindRepeating(&AlwaysTrue),
                                           Timeout(long_timeout_));
  ASSERT_EQ(kUnknownError, status.code());
}

TEST_F(DevToolsClientImplTest, WaitForNextEventConditionalFuncReturnsError) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(base::BindRepeating(&ReturnEvent));
  Status status = client.HandleEventsUntil(base::BindRepeating(&AlwaysError),
                                           Timeout(long_timeout_));
  ASSERT_EQ(kUnknownError, status.code());
}

TEST_F(DevToolsClientImplTest, NestedCommandsWithOutOfOrderResults) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket>);
  int recurse_count = 0;
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  client.SetParserFuncForTesting(
      base::BindRepeating(&ReturnOutOfOrderResponses, &recurse_count, &client));
  base::DictionaryValue params;
  params.GetDict().Set("param", 1);
  base::Value result;
  ASSERT_TRUE(client.SendCommandAndGetResult("method", params, &result).IsOk());
  ASSERT_TRUE(result.is_dict());
  absl::optional<int> key = result.GetDict().FindInt("key");
  ASSERT_TRUE(key);
  ASSERT_EQ(2, key.value());
}

namespace {

class OnConnectedListener : public DevToolsEventListener {
 public:
  OnConnectedListener(const std::string& method, DevToolsClient* client)
      : method_(method),
        client_(client),
        on_connected_called_(false),
        on_event_called_(false) {
    client_->AddListener(this);
  }
  ~OnConnectedListener() override {}

  void VerifyCalled() {
    EXPECT_TRUE(on_connected_called_);
    EXPECT_TRUE(on_event_called_);
  }

  Status OnConnected(DevToolsClient* client) override {
    EXPECT_EQ(client_, client);
    EXPECT_STREQ("onconnected-id", client->GetId().c_str());
    EXPECT_FALSE(on_connected_called_);
    EXPECT_FALSE(on_event_called_);
    on_connected_called_ = true;
    base::DictionaryValue params;
    return client_->SendCommand(method_, params);
  }

  Status OnEvent(DevToolsClient* client,
                 const std::string& method,
                 const base::DictionaryValue& params) override {
    EXPECT_EQ(client_, client);
    EXPECT_STREQ("onconnected-id", client->GetId().c_str());
    EXPECT_TRUE(on_connected_called_);
    on_event_called_ = true;
    return Status(kOk);
  }

 private:
  std::string method_;
  raw_ptr<DevToolsClient> client_;
  bool on_connected_called_;
  bool on_event_called_;
};

class OnConnectedSyncWebSocket : public MockSyncWebSocket {
 public:
  OnConnectedSyncWebSocket() : connected_(false) {}
  ~OnConnectedSyncWebSocket() override {}

  bool IsConnected() override { return connected_; }

  bool Connect(const GURL& url) override {
    connected_ = true;
    return true;
  }

  bool Send(const std::string& message) override {
    EXPECT_TRUE(connected_);
    std::unique_ptr<base::DictionaryValue> dict;
    std::string method;
    if (SendHelper(message, &dict, &method)) {
      base::Value::Dict response;
      response.Set("id", id_);
      response.Set("result", base::DictionaryValue());
      std::string json_response;
      base::JSONWriter::Write(base::Value(std::move(response)), &json_response);
      queued_response_.push_back(json_response);

      // Push one event.
      base::Value::Dict event;
      event.Set("method", "updateEvent");
      event.Set("params", base::DictionaryValue());
      std::string json_event;
      base::JSONWriter::Write(base::Value(std::move(event)), &json_event);
      queued_response_.push_back(json_event);
    }
    return true;
  }

  SyncWebSocket::StatusCode ReceiveNextMessage(
      std::string* message,
      const Timeout& timeout) override {
    if (ReceiveHelper(message)) {
      if (queued_response_.empty())
        return SyncWebSocket::StatusCode::kDisconnected;
      *message = queued_response_.front();
      queued_response_.pop_front();
    }
    return SyncWebSocket::StatusCode::kOk;
  }

  bool HasNextMessage() override { return !queued_response_.empty(); }

 private:
  bool connected_;
  std::list<std::string> queued_response_;
};

}  // namespace

TEST_F(DevToolsClientImplTest, ProcessOnConnectedFirstOnCommand) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<OnConnectedSyncWebSocket>);
  DevToolsClientImpl client("onconnected-id", "", "http://url", factory);
  OnConnectedListener listener1("DOM.getDocument", &client);
  OnConnectedListener listener2("Runtime.enable", &client);
  OnConnectedListener listener3("Page.enable", &client);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  base::DictionaryValue params;
  EXPECT_EQ(kOk, client.SendCommand("Runtime.execute", params).code());
  listener1.VerifyCalled();
  listener2.VerifyCalled();
  listener3.VerifyCalled();
}

TEST_F(DevToolsClientImplTest, ProcessOnConnectedFirstOnHandleEventsUntil) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<OnConnectedSyncWebSocket>);
  DevToolsClientImpl client("onconnected-id", "", "http://url", factory);
  OnConnectedListener listener1("DOM.getDocument", &client);
  OnConnectedListener listener2("Runtime.enable", &client);
  OnConnectedListener listener3("Page.enable", &client);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  EXPECT_EQ(kOk, client.HandleReceivedEvents().code());
  listener1.VerifyCalled();
  listener2.VerifyCalled();
  listener3.VerifyCalled();
}

namespace {

class MockSyncWebSocket5 : public SyncWebSocket {
 public:
  MockSyncWebSocket5() = default;
  ~MockSyncWebSocket5() override = default;

  bool IsConnected() override { return connected_; }

  bool Connect(const GURL& url) override {
    connected_ = true;
    return true;
  }

  bool Send(const std::string& message) override { return true; }

  SyncWebSocket::StatusCode ReceiveNextMessage(
      std::string* message,
      const Timeout& timeout) override {
    if (request_no_ == 0) {
      *message = "{\"method\": \"m\", \"params\": {}}";
    } else {
      *message = base::StringPrintf(
          "{\"result\": {}, \"id\": %d}", request_no_);
    }
    request_no_++;
    return SyncWebSocket::StatusCode::kOk;
  }

  bool HasNextMessage() override { return false; }

 private:
  int request_no_ = 0;
  bool connected_ = false;
};

class OtherEventListener : public DevToolsEventListener {
 public:
  OtherEventListener() : received_event_(false) {}
  ~OtherEventListener() override {}

  Status OnConnected(DevToolsClient* client) override { return Status(kOk); }
  Status OnEvent(DevToolsClient* client,
                 const std::string& method,
                 const base::DictionaryValue& params) override {
    received_event_ = true;
    return Status(kOk);
  }

  bool received_event_;
};

class OnEventListener : public DevToolsEventListener {
 public:
  OnEventListener(DevToolsClient* client,
                  OtherEventListener* other_listener)
      : client_(client),
        other_listener_(other_listener) {}
  ~OnEventListener() override {}

  Status OnConnected(DevToolsClient* client) override {
    EXPECT_EQ(client_, client);
    return Status(kOk);
  }

  Status OnEvent(DevToolsClient* client,
                 const std::string& method,
                 const base::DictionaryValue& params) override {
    EXPECT_EQ(client_, client);
    client_->SendCommand("method", params);
    EXPECT_TRUE(other_listener_->received_event_);
    return Status(kOk);
  }

 private:
  raw_ptr<DevToolsClient> client_;
  raw_ptr<OtherEventListener> other_listener_;
};

}  // namespace

TEST_F(DevToolsClientImplTest, ProcessOnEventFirst) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket5>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  OtherEventListener listener2;
  OnEventListener listener1(&client, &listener2);
  client.AddListener(&listener1);
  client.AddListener(&listener2);
  Status status = client.ConnectIfNecessary();
  ASSERT_EQ(kOk, status.code()) << status.message();
  base::DictionaryValue params;
  EXPECT_EQ(kOk, client.SendCommand("method", params).code());
}

namespace {

class DisconnectedSyncWebSocket : public MockSyncWebSocket {
 public:
  DisconnectedSyncWebSocket() : connection_count_(0), command_count_(0) {}
  ~DisconnectedSyncWebSocket() override {}

  bool Connect(const GURL& url) override {
    connection_count_++;
    connected_ = connection_count_ != 2;
    return connected_;
  }

  bool Send(const std::string& message) override {
    std::unique_ptr<base::DictionaryValue> dict;
    std::string method;
    if (SendHelper(message, &dict, &method)) {
      command_count_++;
      if (command_count_ == 1) {
        connected_ = false;
        add_script_received_ = false;
        runtime_eval_received_ = false;
        connect_complete_ = false;
        return false;
      }
      return MockSyncWebSocket::Send(message);
    }
    return true;
  }

 private:
  int connection_count_;
  int command_count_;
};

Status CheckCloserFuncCalled(bool* is_called) {
  *is_called = true;
  return Status(kOk);
}

}  // namespace

TEST_F(DevToolsClientImplTest, Reconnect) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<DisconnectedSyncWebSocket>);
  bool is_called = false;
  DevToolsClientImpl client("id", "", "http://url", factory);
  client.SetFrontendCloserFunc(
      base::BindRepeating(&CheckCloserFuncCalled, &is_called));
  ASSERT_FALSE(is_called);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  ASSERT_FALSE(is_called);
  base::DictionaryValue params;
  params.GetDict().Set("param", 1);
  is_called = false;
  ASSERT_EQ(kDisconnected, client.SendCommand("method", params).code());
  ASSERT_FALSE(is_called);
  ASSERT_EQ(kDisconnected, client.HandleReceivedEvents().code());
  ASSERT_FALSE(is_called);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  ASSERT_TRUE(is_called);
  is_called = false;
  ASSERT_EQ(kOk, client.SendCommand("method", params).code());
  ASSERT_FALSE(is_called);
}

namespace {

class MockSyncWebSocket6 : public MockSyncWebSocket {
 public:
  explicit MockSyncWebSocket6(std::list<std::string>* messages)
      : messages_(messages) {}
  ~MockSyncWebSocket6() override = default;

  bool IsConnected() override { return connected_; }

  bool Connect(const GURL& url) override {
    connected_ = true;
    return true;
  }

  bool Send(const std::string& message) override { return true; }

  SyncWebSocket::StatusCode ReceiveNextMessage(
      std::string* message,
      const Timeout& timeout) override {
    if (messages_->empty())
      return SyncWebSocket::StatusCode::kDisconnected;
    *message = messages_->front();
    messages_->pop_front();
    return SyncWebSocket::StatusCode::kOk;
  }

  bool HasNextMessage() override { return messages_->size(); }

 private:
  raw_ptr<std::list<std::string>> messages_;
  bool connected_ = false;
};

class MockDevToolsEventListener : public DevToolsEventListener {
 public:
  MockDevToolsEventListener() = default;
  ~MockDevToolsEventListener() override = default;

  Status OnConnected(DevToolsClient* client) override { return Status(kOk); }

  Status OnEvent(DevToolsClient* client,
                 const std::string& method,
                 const base::DictionaryValue& params) override {
    DevToolsClientImpl* client_impl = static_cast<DevToolsClientImpl*>(client);
    int msg_id = client_impl->NextMessageId();

    Status status = client->SendCommand("hello", params);

    if (msg_id == expected_blocked_id_) {
      EXPECT_EQ(kUnexpectedAlertOpen, status.code());
    } else {
      EXPECT_EQ(kOk, status.code());
    }
    return Status(kOk);
  }

  void SetExpectedBlockedId(int value) { expected_blocked_id_ = value; }

 private:
  int expected_blocked_id_ = -1;
};

std::unique_ptr<SyncWebSocket> CreateMockSyncWebSocket6(
    std::list<std::string>* messages) {
  return std::make_unique<MockSyncWebSocket6>(messages);
}

}  // namespace

TEST_F(DevToolsClientImplTest, BlockedByAlert) {
  std::list<std::string> msgs;
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket6, &msgs);
  DevToolsClientImpl client("id", "", "http://url", factory);
  Status status = client.ConnectIfNecessary();
  ASSERT_EQ(kOk, status.code()) << status.message();
  msgs.push_back(
      "{\"method\": \"Page.javascriptDialogOpening\", \"params\": {}}");
  msgs.push_back("{\"id\": 2, \"result\": {}}");
  base::DictionaryValue params;
  ASSERT_EQ(kUnexpectedAlertOpen,
            client.SendCommand("first", params).code());
}

TEST_F(DevToolsClientImplTest, CorrectlyDeterminesWhichIsBlockedByAlert) {
  // OUT                 | IN
  //                       FirstEvent
  // hello (id1)
  //                       SecondEvent
  // hello (id2)
  //                       ThirdEvent
  // hello (id3)
  //                       FourthEvent
  // hello (id4)
  //                       response for id1
  //                       alert
  // hello (id5)
  // round trip command (id6)
  //                       response for id2
  //                       response for id4
  //                       response for id5
  //                       response for id6
  std::list<std::string> msgs;
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket6, &msgs);
  DevToolsClientImpl client("id", "", "http://url", factory);
  MockDevToolsEventListener listener;
  client.AddListener(&listener);
  Status status = client.ConnectIfNecessary();
  ASSERT_EQ(kOk, status.code()) << status.message();
  int next_msg_id = client.NextMessageId();
  msgs.push_back("{\"method\": \"FirstEvent\", \"params\": {}}");
  msgs.push_back("{\"method\": \"SecondEvent\", \"params\": {}}");
  msgs.push_back("{\"method\": \"ThirdEvent\", \"params\": {}}");
  msgs.push_back("{\"method\": \"FourthEvent\", \"params\": {}}");
  msgs.push_back((std::stringstream()
                  << "{\"id\": " << next_msg_id++ << ", \"result\": {}}")
                     .str());
  msgs.push_back(
      "{\"method\": \"Page.javascriptDialogOpening\", \"params\": {}}");
  msgs.push_back((std::stringstream()
                  << "{\"id\": " << next_msg_id++ << ", \"result\": {}}")
                     .str());
  listener.SetExpectedBlockedId(next_msg_id++);
  msgs.push_back((std::stringstream()
                  << "{\"id\": " << next_msg_id++ << ", \"result\": {}}")
                     .str());
  msgs.push_back((std::stringstream()
                  << "{\"id\": " << next_msg_id++ << ", \"result\": {}}")
                     .str());
  msgs.push_back((std::stringstream()
                  << "{\"id\": " << next_msg_id++ << ", \"result\": {}}")
                     .str());
  ASSERT_EQ(kOk, client.HandleReceivedEvents().code());
}

namespace {

class MockCommandListener : public DevToolsEventListener {
 public:
  MockCommandListener() {}
  ~MockCommandListener() override {}

  Status OnEvent(DevToolsClient* client,
                 const std::string& method,
                 const base::DictionaryValue& params) override {
    msgs_.push_back(method);
    return Status(kOk);
  }

  Status OnCommandSuccess(DevToolsClient* client,
                          const std::string& method,
                          const base::DictionaryValue* result,
                          const Timeout& command_timeout) override {
    msgs_.push_back(method);
    if (!callback_.is_null())
      callback_.Run(client);
    return Status(kOk);
  }

  base::RepeatingCallback<void(DevToolsClient*)> callback_;
  std::list<std::string> msgs_;
};

void HandleReceivedEvents(DevToolsClient* client) {
  EXPECT_EQ(kOk, client->HandleReceivedEvents().code());
}

}  // namespace

TEST_F(DevToolsClientImplTest, ReceivesCommandResponse) {
  std::list<std::string> msgs;
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket6, &msgs);
  DevToolsClientImpl client("id", "", "http://url", factory);
  MockCommandListener listener1;
  listener1.callback_ = base::BindRepeating(&HandleReceivedEvents);
  MockCommandListener listener2;
  client.AddListener(&listener1);
  client.AddListener(&listener2);
  Status status = client.ConnectIfNecessary();
  ASSERT_EQ(kOk, status.code()) << status.message();
  int next_msg_id = client.NextMessageId();
  msgs.push_back((std::stringstream()
                  << "{\"id\": " << next_msg_id++ << ", \"result\": {}}")
                     .str());
  msgs.push_back("{\"method\": \"event\", \"params\": {}}");
  base::DictionaryValue params;
  ASSERT_EQ(kOk, client.SendCommand("cmd", params).code());
  ASSERT_EQ(2u, listener2.msgs_.size());
  ASSERT_EQ("cmd", listener2.msgs_.front());
  ASSERT_EQ("event", listener2.msgs_.back());
}

namespace {

class MockSyncWebSocket7 : public SyncWebSocket {
 public:
  MockSyncWebSocket7() : id_(-1), sent_messages_(0), sent_responses_(0) {}
  ~MockSyncWebSocket7() override {}

  bool IsConnected() override { return true; }

  bool Connect(const GURL& url) override { return true; }

  bool Send(const std::string& message) override {
    std::unique_ptr<base::Value> value =
        base::JSONReader::ReadDeprecated(message);
    base::Value::Dict* dict = value->GetIfDict();
    EXPECT_TRUE(dict);
    if (!dict)
      return false;
    absl::optional<int> maybe_id = dict->FindInt("id");
    EXPECT_TRUE(maybe_id);
    if (!maybe_id)
      return false;
    id_ = *maybe_id;
    std::string* method = dict->FindString("method");
    EXPECT_TRUE(method);
    EXPECT_STREQ("method", method->c_str());
    base::Value::Dict* params = dict->FindDict("params");
    if (!params)
      return false;
    sent_messages_++;
    return true;
  }

  SyncWebSocket::StatusCode ReceiveNextMessage(
      std::string* message,
      const Timeout& timeout) override {
    EXPECT_LE(sent_responses_, 1);
    EXPECT_EQ(sent_messages_, 2);
    base::Value::Dict response;
    response.Set("id", (sent_responses_ == 0) ? 1 : 2);
    base::Value result{base::Value::Type::DICT};
    result.GetDict().Set("param", 1);
    response.Set("result", result.Clone());
    base::JSONWriter::Write(base::Value(std::move(response)), message);
    sent_responses_++;
    return SyncWebSocket::StatusCode::kOk;
  }

  bool HasNextMessage() override { return sent_messages_ > sent_responses_; }

 private:
  int id_;
  int sent_messages_;
  int sent_responses_;
};

}  // namespace

TEST_F(DevToolsClientImplTest, SendCommandAndIgnoreResponse) {
  SyncWebSocketFactory factory =
      base::BindRepeating(&CreateMockSyncWebSocket<MockSyncWebSocket7>);
  DevToolsClientImpl client("id", "", "http://url", factory);
  ASSERT_EQ(kOk, client.ConnectIfNecessary().code());
  base::DictionaryValue params;
  params.GetDict().Set("param", 1);
  ASSERT_EQ(kOk, client.SendCommandAndIgnoreResponse("method", params).code());
  ASSERT_EQ(kOk, client.SendCommand("method", params).code());
}
