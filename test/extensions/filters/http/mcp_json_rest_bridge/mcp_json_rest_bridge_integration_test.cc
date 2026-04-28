#include "envoy/http/codec.h"
#include "envoy/network/address.h"

#include "source/common/buffer/buffer_impl.h"

#include "test/integration/http_integration.h"
#include "test/test_common/environment.h"
#include "test/test_common/utility.h"

#include "fmt/format.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp" // IWYU pragma: keep

namespace Envoy {
namespace {

using ::testing::IsEmpty;
using ::testing::StrEq;

class McpJsonRestBridgeIntegrationTest : public testing::TestWithParam<Network::Address::IpVersion>,
                                         public HttpIntegrationTest {
public:
  McpJsonRestBridgeIntegrationTest() : HttpIntegrationTest(Http::CodecType::HTTP2, GetParam()) {}

  void SetUp() override { setUpstreamProtocol(Http::CodecType::HTTP2); }

  void initializeFilter(const std::string& config) {
    config_helper_.prependFilter(config);
    initialize();
  }
};

INSTANTIATE_TEST_SUITE_P(IpVersions, McpJsonRestBridgeIntegrationTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                         TestUtility::ipTestParamsToString);

TEST_P(McpJsonRestBridgeIntegrationTest, InitializeSuccess) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "protocolVersion": "2025-06-18",
      "capabilities": {}
    }
  })";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
  EXPECT_THAT(response->headers().getContentTypeValue(), StrEq("application/json"));
  EXPECT_THAT(response->headers().getContentLengthValue(),
              StrEq(std::to_string(response->body().size())));

  const std::string expected_response = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
      "capabilities": {
        "tools": {
          "listChanged": false
        }
      },
      "protocolVersion": "2025-06-18",
      "serverInfo": {
        "name": "host",
        "version": "1.0.0"
      }
    }
  })";

  EXPECT_EQ(nlohmann::json::parse(response->body()), nlohmann::json::parse(expected_response));
}

TEST_P(McpJsonRestBridgeIntegrationTest, InitializedSuccess) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "method": "notifications/initialized"
  })";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("202"));
  EXPECT_THAT(response->headers().getContentTypeValue(), IsEmpty());
  EXPECT_THAT(response->headers().getContentLengthValue(), IsEmpty());
  EXPECT_THAT(response->body(), IsEmpty());
}

TEST_P(McpJsonRestBridgeIntegrationTest, ToolsCallTranscoding) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
      tool_config:
        tools:
          - name: "create_api_key"
            http_rule:
              post: "/v1/{parent=projects/*}/keys"
              body: "key"
  )EOF";

  initializeFilter(config);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "id": 321,
    "method": "tools/call",
    "params": {
      "name": "create_api_key",
      "arguments": {
        "parent": "projects/foo",
        "key": {
          "displayName": "bar"
        }
      }
    }
  })";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), StrEq("POST"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), StrEq("/v1/projects/foo/keys"));
  EXPECT_THAT(upstream_request_->body().toString(), StrEq(R"({"displayName":"bar"})"));

  Http::TestResponseHeaderMapImpl response_headers;
  response_headers.setStatus(200);
  response_headers.setContentType(Http::Headers::get().ContentTypeValues.Json);

  upstream_request_->encodeHeaders(response_headers, false);

  Buffer::OwnedImpl response_data;
  response_data.add(R"({"displayName":"bar","createTime":"1970-01-01T00:00:22Z"})");
  upstream_request_->encodeData(response_data, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
  EXPECT_THAT(response->headers().getContentTypeValue(), StrEq("application/json"));
  EXPECT_THAT(response->headers().getContentLengthValue(),
              StrEq(std::to_string(response->body().size())));
  const std::string expected_rpc_response = R"({
    "jsonrpc": "2.0",
    "id": 321,
    "result": {
      "content": [
        {
          "type": "text",
          "text": "{\"displayName\":\"bar\",\"createTime\":\"1970-01-01T00:00:22Z\"}"
        }
      ],
      "isError": false
    }
  })";
  EXPECT_EQ(nlohmann::json::parse(response->body()), nlohmann::json::parse(expected_rpc_response));
}

TEST_P(McpJsonRestBridgeIntegrationTest, ToolsCallTranscodingDuplicateKeysEarlyStop) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
      tool_config:
        tools:
          - name: "read_only_tool"
            http_rule:
              post: "/v1/read"
              body: "*"
          - name: "destructive_tool"
            http_rule:
              post: "/v1/destroy"
              body: "*"
  )EOF";

  initializeFilter(config);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  auto encoder_decoder = codec_client_->startRequest(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}});
  auto response = std::move(encoder_decoder.second);

  // Send the first chunk with the first params block.
  Buffer::OwnedImpl chunk1(R"({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {
      "name": "read_only_tool",
      "arguments": {}
    },
  )");
  codec_client_->sendData(encoder_decoder.first, chunk1, false);

  // Send the second chunk with the duplicate params block.
  // Because the parser has already collected all fields (jsonrpc, id, method, params.name,
  // params.arguments), it should stop early and ignore this destructive tool call.
  Buffer::OwnedImpl chunk2(R"(
    "params": {
      "name": "destructive_tool",
      "arguments": {}
    }
  })");
  codec_client_->sendData(encoder_decoder.first, chunk2, true);

  waitForNextUpstreamRequest();

  // Verify that the request was transcoded for the first tool (read_only_tool).
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), StrEq("POST"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), StrEq("/v1/read"));
  EXPECT_THAT(upstream_request_->body().toString(), StrEq("{}"));

  Http::TestResponseHeaderMapImpl response_headers;
  response_headers.setStatus(200);
  response_headers.setContentType(Http::Headers::get().ContentTypeValues.Json);

  upstream_request_->encodeHeaders(response_headers, false);

  Buffer::OwnedImpl response_data;
  response_data.add(R"({"success":true})");
  upstream_request_->encodeData(response_data, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
}

TEST_P(McpJsonRestBridgeIntegrationTest, ToolsCallTranscodingDuplicateKeysSingleChunk) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
      tool_config:
        tools:
          - name: "read_only_tool"
            http_rule:
              post: "/v1/read"
              body: "*"
          - name: "destructive_tool"
            http_rule:
              post: "/v1/destroy"
              body: "*"
  )EOF";

  initializeFilter(config);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  // Send the entire body in a single chunk.
  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {
      "name": "read_only_tool",
      "arguments": {}
    },
    "params": {
      "name": "destructive_tool",
      "arguments": {}
    }
  })";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  waitForNextUpstreamRequest();

  // Verify that the request was transcoded for the FIRST tool (read_only_tool).
  // Even though the entire body is parsed in a single chunk, the McpFieldExtractor
  // ignores all subsequent JSON events once its internal early stop flag is set.
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), StrEq("POST"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), StrEq("/v1/read"));
  EXPECT_THAT(upstream_request_->body().toString(), StrEq("{}"));

  Http::TestResponseHeaderMapImpl response_headers;
  response_headers.setStatus(200);
  response_headers.setContentType(Http::Headers::get().ContentTypeValues.Json);

  upstream_request_->encodeHeaders(response_headers, false);

  Buffer::OwnedImpl response_data;
  response_data.add(R"({"success":true})");
  upstream_request_->encodeData(response_data, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
}

TEST_P(McpJsonRestBridgeIntegrationTest, ToolsListTranscoding) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
      tool_config:
        tool_list_http_rule:
          get: "/v1/tools"
  )EOF";

  initializeFilter(config);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "id": 123,
    "method": "tools/list"
  })";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), StrEq("GET"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), StrEq("/v1/tools"));

  Http::TestResponseHeaderMapImpl response_headers;
  response_headers.setStatus(200);
  response_headers.setContentType(Http::Headers::get().ContentTypeValues.Json);

  upstream_request_->encodeHeaders(response_headers, false);

  Buffer::OwnedImpl response_data;
  const std::string backend_response_body = R"({
    "tools": [
      {
        "annotations": {},
        "description": "Create an API key",
        "inputSchema": {
          "description": "Request message for CreateApiKey method.",
          "properties": {
            "key": {
              "description": "Message for an API key.",
              "properties": {
                "displayName": {
                  "description": "Optional. The display name of the key.",
                  "type": "string"
                }
              },
              "type": "object"
            },
            "parent": {
              "description": "The parent resource must have the format of \"project/*\".",
              "type": "string"
            }
          },
          "type": "object"
        },
        "name": "create_api_key"
      }
    ]
  })";
  response_data.add(backend_response_body);
  upstream_request_->encodeData(response_data, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
  EXPECT_THAT(response->headers().getContentTypeValue(), StrEq("application/json"));
  EXPECT_THAT(response->headers().getContentLengthValue(),
              StrEq(std::to_string(response->body().size())));

  const std::string expected_rpc_response = R"({
    "jsonrpc": "2.0",
    "id": 123,
    "result": {
      "tools": [
        {
          "annotations": {},
          "description": "Create an API key",
          "inputSchema": {
            "description": "Request message for CreateApiKey method.",
            "properties": {
              "key": {
                "description": "Message for an API key.",
                "properties": {
                  "displayName": {
                    "description": "Optional. The display name of the key.",
                    "type": "string"
                  }
                },
                "type": "object"
              },
              "parent": {
                "description": "The parent resource must have the format of \"project/*\".",
                "type": "string"
              }
            },
            "type": "object"
          },
          "name": "create_api_key"
        }
      ]
    }
  })";
  EXPECT_EQ(nlohmann::json::parse(response->body()), nlohmann::json::parse(expected_rpc_response));
}

TEST_P(McpJsonRestBridgeIntegrationTest, ToolsListPassthrough) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "id": 123,
    "method": "tools/list"
  })";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), StrEq("POST"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), StrEq("/mcp"));

  // Verify that the original request body is passed through.
  EXPECT_EQ(nlohmann::json::parse(upstream_request_->body().toString()),
            nlohmann::json::parse(R"({"id":123,"jsonrpc":"2.0","method":"tools/list"})"));

  Http::TestResponseHeaderMapImpl response_headers;
  response_headers.setStatus(200);
  response_headers.setContentType(Http::Headers::get().ContentTypeValues.Json);
  const std::string backend_response_body = R"({
    "jsonrpc": "2.0",
    "id": 123,
    "result": {
      "tools": [
        {
          "name": "passthrough_tool"
        }
      ]
    }
  })";
  response_headers.setContentLength(backend_response_body.size());

  upstream_request_->encodeHeaders(response_headers, false);

  Buffer::OwnedImpl response_data;
  response_data.add(backend_response_body);
  upstream_request_->encodeData(response_data, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
  EXPECT_THAT(response->headers().getContentTypeValue(), StrEq("application/json"));
  EXPECT_THAT(response->headers().getContentLengthValue(),
              StrEq(std::to_string(response->body().size())));

  EXPECT_EQ(nlohmann::json::parse(response->body()), nlohmann::json::parse(backend_response_body));
}

TEST_P(McpJsonRestBridgeIntegrationTest, InvalidJsonSyntaxReturnsBadRequest) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // Missing closing braces and quotes
  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "method": "tools/call
  )";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("400"));
  EXPECT_NE(response->body().find("JSON parse error"), std::string::npos);
}

TEST_P(McpJsonRestBridgeIntegrationTest, IncompleteJsonAtEndStreamReturnsBadRequest) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // Valid JSON syntax so far, but incomplete object
  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "method": "tools/call"
  )";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("400"));
  EXPECT_NE(response->body().find("JSON parse error"), std::string::npos);
}

TEST_P(McpJsonRestBridgeIntegrationTest, IncompleteJsonWithAllRequiredFieldsReturnsBadRequest) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // Has all required fields (jsonrpc, id, method, params.name) but is incomplete
  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/call",
    "params": {
      "name": "echo"
    }
)";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("400"));
  EXPECT_NE(response->body().find("JSON parse error"), std::string::npos);
}

TEST_P(McpJsonRestBridgeIntegrationTest, MissingRequiredMcpFieldsReturnsBadRequest) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // Valid JSON, but missing "jsonrpc" and "method"
  const std::string request_body = R"({
    "id": 1,
    "params": {}
  })";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("400"));
  EXPECT_NE(response->body().find("Missing method field"), std::string::npos);
}

TEST_P(McpJsonRestBridgeIntegrationTest, EmptyBodyReturnsBadRequest) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);
  codec_client_ = makeHttpConnection(lookupPort("http"));

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      "");

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("400"));
}

TEST_P(McpJsonRestBridgeIntegrationTest, LargeJsonPayloadHandledCorrectly) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);
  codec_client_ = makeHttpConnection(lookupPort("http"));

  // Create a large JSON payload (approx 1MB)
  std::string large_array = "[";
  for (int i = 0; i < 100000; ++i) {
    large_array += "1,";
  }
  large_array += "1]";

  std::string request_body = fmt::format(R"({{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/list",
    "params": {{
      "large_data": {}
    }}
  }})",
                                         large_array);

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getMethodValue(), StrEq("POST"));
  EXPECT_THAT(upstream_request_->headers().getPathValue(), StrEq("/mcp"));

  Http::TestResponseHeaderMapImpl response_headers;
  response_headers.setStatus(200);
  response_headers.setContentType(Http::Headers::get().ContentTypeValues.Json);
  const std::string backend_response_body = R"({
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
      "tools": []
    }
  })";
  response_headers.setContentLength(backend_response_body.size());

  upstream_request_->encodeHeaders(response_headers, false);

  Buffer::OwnedImpl response_data;
  response_data.add(backend_response_body);
  upstream_request_->encodeData(response_data, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_TRUE(upstream_request_->complete());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
}

TEST_P(McpJsonRestBridgeIntegrationTest, NonMcpRequestPassesThrough) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);
  codec_client_ = makeHttpConnection(lookupPort("http"));

  const std::string request_body = "some random data";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/not-mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "text/plain"}},
      request_body);

  waitForNextUpstreamRequest();
  EXPECT_THAT(upstream_request_->headers().getPathValue(), StrEq("/not-mcp"));
  EXPECT_THAT(upstream_request_->body().toString(), StrEq(request_body));

  Http::TestResponseHeaderMapImpl response_headers;
  response_headers.setStatus(200);
  upstream_request_->encodeHeaders(response_headers, true);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
}

TEST_P(McpJsonRestBridgeIntegrationTest, InitializeUnsupportedProtocolVersionFallsBackToLatest) {
  const std::string config = R"EOF(
    name: envoy.filters.http.mcp_json_rest_bridge
    typed_config:
      "@type": type.googleapis.com/envoy.extensions.filters.http.mcp_json_rest_bridge.v3.McpJsonRestBridge
  )EOF";

  initializeFilter(config);

  codec_client_ = makeHttpConnection(lookupPort("http"));

  const std::string request_body = R"({
    "jsonrpc": "2.0",
    "id": 100,
    "method": "initialize",
    "params": {
      "protocolVersion": "unsupported-version"
    }
  })";

  auto response = codec_client_->makeRequestWithBody(
      Http::TestRequestHeaderMapImpl{{":method", "POST"},
                                     {":path", "/mcp"},
                                     {":scheme", "http"},
                                     {":authority", "host"},
                                     {"content-type", "application/json"}},
      request_body);

  ASSERT_TRUE(response->waitForEndStream());
  EXPECT_THAT(response->headers().getStatusValue(), StrEq("200"));
  EXPECT_THAT(response->headers().getContentTypeValue(), StrEq("application/json"));
  EXPECT_THAT(response->headers().getContentLengthValue(),
              StrEq(std::to_string(response->body().size())));

  const std::string expected_response = R"({
    "jsonrpc": "2.0",
    "id": 100,
    "result": {
      "capabilities": {
        "tools": {
          "listChanged": false
        }
      },
      "protocolVersion": "2025-11-25",
      "serverInfo": {
        "name": "host",
        "version": "1.0.0"
      }
    }
  })";

  EXPECT_EQ(nlohmann::json::parse(response->body()), nlohmann::json::parse(expected_response));
}

} // namespace
} // namespace Envoy
