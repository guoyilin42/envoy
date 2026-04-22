#include "source/extensions/filters/http/mcp_json_rest_bridge/mcp_json_rest_bridge_filter.h"

#include <cmath>

#include "envoy/extensions/filters/http/mcp_json_rest_bridge/v3/mcp_json_rest_bridge.pb.h"
#include "envoy/grpc/status.h"
#include "envoy/http/codes.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"

#include "source/common/http/headers.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/common/mcp/constants.h"
#include "source/extensions/filters/http/mcp_json_rest_bridge/http_request_builder.h"

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "fmt/format.h"
#include "utf8_validity.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace McpJsonRestBridge {
namespace {

using ::nlohmann::json;
namespace McpConstants = Envoy::Extensions::Filters::Common::Mcp::McpConstants;
namespace Mcp = Envoy::Extensions::HttpFilters::Mcp;

constexpr double MaxSafeInteger = (1ULL << std::numeric_limits<double>::digits) - 1;

bool isMcpProtocolVersionSupported(absl::string_view protocol_version) {
  static const absl::NoDestructor<absl::flat_hash_set<absl::string_view>> supported_mcp_versions({
      McpConstants::LATEST_SUPPORTED_MCP_VERSION,
      McpConstants::FALLBACK_PROTOCOL_VERSION,
      McpConstants::MCP_VERSION_2024_11_05,
      McpConstants::MCP_VERSION_2025_06_18,
  });
  return supported_mcp_versions->contains(protocol_version);
}

absl::StatusOr<json> getSessionId(const Protobuf::Value& v) {
  switch (v.kind_case()) {
  case Protobuf::Value::kStringValue:
    return v.string_value();
  case Protobuf::Value::kNumberValue: {
    double d = v.number_value();
    // 2^53 - 1 is the maximum safe integer in a 64-bit float (IEEE 754).
    // Values outside this range will lose precision and cannot be safely cast.
    if (d >= -MaxSafeInteger && d <= MaxSafeInteger && d == std::floor(d)) {
      return static_cast<int64_t>(d);
    }
    return absl::InvalidArgumentError("JSON-RPC request ID is not an integer or a string.");
  }
  default:
    return absl::InvalidArgumentError(
        "JSON-RPC request (except notification) does not have an ID.");
  }
}

json translateJsonRestResponseToJsonRpc(absl::string_view tool_call_response,
                                        const json& session_id, bool is_error) {
  return json{
      {McpConstants::JSONRPC_FIELD, McpConstants::JSONRPC_VERSION},
      {McpConstants::ID_FIELD, session_id},
      {McpConstants::RESULT_FIELD,
       {
           {McpConstants::CONTENT_FIELD,
            json::array({{{McpConstants::TYPE_FIELD, "text"},
                          {McpConstants::TEXT_FIELD, tool_call_response}}})},
           {McpConstants::IS_ERROR_FIELD, is_error},
       }},
  };
}

json generateInitializeResponse(const json& session_id, absl::string_view server_name,
                                absl::string_view protocol_version) {
  absl::string_view negotiated_protocol_version = McpConstants::LATEST_SUPPORTED_MCP_VERSION;
  if (isMcpProtocolVersionSupported(protocol_version)) {
    negotiated_protocol_version = protocol_version;
  }

  json ret;
  ret[McpConstants::JSONRPC_FIELD] = McpConstants::JSONRPC_VERSION;
  ret[McpConstants::ID_FIELD] = session_id;

  json result;
  result[McpConstants::PROTOCOL_VERSION_FIELD] = negotiated_protocol_version;
  // TODO(guoyilin42): Support list_changed from ServerToolConfig and description from ServerInfo.
  result[McpConstants::CAPABILITIES_FIELD][McpConstants::TOOLS_FIELD]
        [McpConstants::LIST_CHANGED_FIELD] = false;
  result[McpConstants::SERVER_INFO_FIELD][McpConstants::NAME_FIELD] = server_name;
  result[McpConstants::SERVER_INFO_FIELD][McpConstants::VERSION_FIELD] =
      McpConstants::DEFAULT_SERVER_VERSION;
  ret[McpConstants::RESULT_FIELD] = result;
  return ret;
}

json generateErrorJsonResponse(int error_code, absl::string_view error_message) {
  return json{
      {McpConstants::ERROR_CODE_FIELD, error_code},
      {McpConstants::ERROR_MESSAGE_FIELD, error_message},
  };
}

int getResponseCode(Http::ResponseHeaderMapOptConstRef response_headers) {
  if (!response_headers.has_value()) {
    return static_cast<int>(Http::Code::InternalServerError);
  }
  int status_code;
  if (!absl::SimpleAtoi(response_headers->getStatusValue(), &status_code)) {
    return static_cast<int>(Http::Code::InternalServerError);
  }
  return status_code;
}

bool validateRequestMcpVersion(absl::string_view method,
                               Http::RequestHeaderMapOptConstRef request_headers,
                               absl::string_view fallback_protocol_version) {
  static const absl::NoDestructor<Http::LowerCaseString> mcp_protocol_version_header(
      McpConstants::MCP_PROTOCOL_VERSION_HEADER);
  // The initialize request is not expected to have MCP protocol version header.
  // So we will not check the protocol version for this request.
  if (method == McpConstants::Methods::INITIALIZE) {
    return true;
  }
  absl::string_view protocol_version = fallback_protocol_version;
  if (request_headers.has_value()) {
    auto headers = request_headers->get(*mcp_protocol_version_header);
    if (!headers.empty()) {
      protocol_version = headers[0]->value().getStringView();
    }
  }
  return isMcpProtocolVersionSupported(protocol_version);
}

} // namespace

McpJsonRestBridgeFilterConfig::McpJsonRestBridgeFilterConfig(
    const envoy::extensions::filters::http::mcp_json_rest_bridge::v3::McpJsonRestBridge&
        proto_config)
    : proto_config_(proto_config), fallback_protocol_version_(PROTOBUF_GET_WRAPPED_OR_DEFAULT(
                                       proto_config_.server_info(), fallback_protocol_version,
                                       std::string(McpConstants::FALLBACK_PROTOCOL_VERSION))) {
  for (const auto& tool : proto_config.tool_config().tools()) {
    tool_to_http_rule_[tool.name()] = tool.http_rule();
  }

  parser_config_ = Mcp::McpParserConfig::createDefault();
  parser_config_.addMethodConfig(
      McpConstants::Methods::TOOLS_CALL,
      {Mcp::McpParserConfig::AttributeExtractionRule(std::string(McpConstants::Paths::PARAMS_NAME)),
       Mcp::McpParserConfig::AttributeExtractionRule(
           std::string(McpConstants::Paths::PARAMS_ARGUMENTS))});

  ENVOY_LOG(debug, "Received MCP JSON REST Bridge config: {}", proto_config_.DebugString());
}

absl::StatusOr<envoy::extensions::filters::http::mcp_json_rest_bridge::v3::HttpRule>
McpJsonRestBridgeFilterConfig::getHttpRule(absl::string_view tool_name) const {
  auto it = tool_to_http_rule_.find(tool_name);
  if (it == tool_to_http_rule_.end()) {
    return absl::InvalidArgumentError(
        fmt::format("Failed to find http rule for tool_name: {}", tool_name));
  }
  return it->second;
}

absl::StatusOr<envoy::extensions::filters::http::mcp_json_rest_bridge::v3::HttpRule>
McpJsonRestBridgeFilterConfig::getToolsListHttpRule() const {
  if (!proto_config_.tool_config().has_tool_list_http_rule()) {
    return absl::NotFoundError("tools_list_http_rule is not configured.");
  }
  return proto_config_.tool_config().tool_list_http_rule();
}

Http::FilterHeadersStatus
McpJsonRestBridgeFilter::decodeHeaders(Http::RequestHeaderMap& request_headers, bool) {
  absl::string_view path = request_headers.getPathValue();
  auto query_idx = path.find('?');
  if (query_idx != absl::string_view::npos) {
    path = path.substr(0, query_idx);
  }
  // TODO(guoyilin42): Make the MCP endpoint configurable.
  if (path != "/mcp") {
    // Only intercept /mcp requests and pass through other requests.
    return Http::FilterHeadersStatus::Continue;
  }

  mcp_operation_ = McpOperation::Undecided;
  // TODO(guoyilin42): Strip port number from server_name_.
  server_name_ = std::string(request_headers.getHostValue());

  if (request_headers.getMethodValue() != Http::Headers::get().MethodValues.Post) {
    ENVOY_STREAM_LOG(warn, "Only POST method is supported for MCP. Received: {}",
                     *decoder_callbacks_, request_headers.getMethodValue());
    decoder_callbacks_->sendLocalReply(
        Http::Code::MethodNotAllowed, "Method Not Allowed",
        [](Http::ResponseHeaderMap& response_headers) {
          response_headers.addCopy(Http::LowerCaseString("allow"),
                                   Http::Headers::get().MethodValues.Post);
        },
        Grpc::Status::WellKnownGrpcStatus::InvalidArgument, "mcp_json_rest_bridge_filter_not_post");
    return Http::FilterHeadersStatus::StopIteration;
  }

  return Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus McpJsonRestBridgeFilter::decodeData(Buffer::Instance& data,
                                                           bool end_stream) {
  if (mcp_operation_ == McpOperation::Unspecified) {
    return Http::FilterDataStatus::Continue;
  }

  if (!parser_) {
    parser_ = std::make_unique<Mcp::McpJsonParser>(config_->parserConfig());
  }

  // TODO(guoyilin42): Add hard limit for the request and response body sizes.
  for (const auto& slice : data.getRawSlices()) {
    const char* start = static_cast<const char*>(slice.mem_);
    size_t len = slice.len_;

    absl::Status status = parser_->parse({start, len});
    if (!status.ok()) {
      // Ignore parse errors if all required fields are collected. Early termination
      // leaves JSON incomplete, causing `FinishParse()` to fail. Continuing
      // effectively skips remaining data.
      if (parser_->isAllFieldsCollected()) {
        ENVOY_STREAM_LOG(debug, "Early parse termination: found all fields (ignoring parse error)",
                         *decoder_callbacks_);
        continue;
      }

      ENVOY_STREAM_LOG(error, "Failed to parse JSON-RPC request body: {}", *decoder_callbacks_,
                       status.ToString());
      sendErrorResponse(Http::Code::BadRequest,
                        "mcp_json_rest_bridge_filter_failed_to_parse_json_rpc_request",
                        generateErrorJsonResponse(-32700, "JSON parse error").dump());
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }
  }

  // Consume the data in the buffer.
  data.drain(data.length());

  if (end_stream) {
    if (absl::Status status = parser_->finishParse(); !status.ok()) {
      ENVOY_STREAM_LOG(error, "Failed to finish parsing JSON-RPC request body: status: {}",
                       *decoder_callbacks_, status.ToString());
      sendErrorResponse(Http::Code::BadRequest,
                        "mcp_json_rest_bridge_filter_failed_to_parse_json_rpc_request",
                        generateErrorJsonResponse(-32700, "JSON parse error").dump());
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }
    handleMcpMethod(decoder_callbacks_->requestHeaders());
    data.add(request_body_str_);
    request_body_str_.clear();

    if (mcp_operation_ == McpOperation::Initialization ||
        mcp_operation_ == McpOperation::InitializationAck ||
        mcp_operation_ == McpOperation::OperationFailed) {
      return Http::FilterDataStatus::StopIterationNoBuffer;
    }

    return Http::FilterDataStatus::Continue;
  }

  return Http::FilterDataStatus::StopIterationAndWatermark;
}

Http::FilterHeadersStatus McpJsonRestBridgeFilter::encodeHeaders(Http::ResponseHeaderMap&,
                                                                 bool end_stream) {
  switch (mcp_operation_) {
  case McpOperation::Unspecified:
  case McpOperation::Undecided:
  case McpOperation::Initialization:
  // The response for InitializedNotification is empty body so we don't need
  // to modify the response headers.
  case McpOperation::InitializationAck:
    return Http::FilterHeadersStatus::Continue;
  default:
    break;
  }

  // TODO(guoyilin42): Handle headers-only upstream responses (e.g., 204 No Content).
  // Currently, these cases bypass transcoding, which can cause MCP SDKs to timeout
  // or throw exceptions because they expect a valid JSON-RPC response with a
  // matching ID. Envoy should generate a synthetic JSON-RPC response (e.g., an
  // empty ToolResult or a generic error) to ensure client stability.
  return end_stream ? Http::FilterHeadersStatus::Continue
                    : Http::FilterHeadersStatus::StopIteration;
}

Http::FilterDataStatus McpJsonRestBridgeFilter::encodeData(Buffer::Instance& data,
                                                           bool end_stream) {
  // No need to encode the response body for Initialization and InitializationAck.
  if (mcp_operation_ == McpOperation::Unspecified ||
      mcp_operation_ == McpOperation::Initialization ||
      mcp_operation_ == McpOperation::InitializationAck) {
    return Http::FilterDataStatus::Continue;
  }
  response_body_.move(data);

  if (!end_stream) {
    return Http::FilterDataStatus::StopIterationNoBuffer;
  }

  encodeJsonRpcData(encoder_callbacks_->responseHeaders());
  data.add(response_body_str_);
  response_body_str_.clear();
  return Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus McpJsonRestBridgeFilter::encodeTrailers(Http::ResponseTrailerMap&) {
  // TODO(guoyilin42): Add support for transcoding upstream responses that include HTTP trailers.
  // Currently, if a response contains trailers (i.e., end_stream is false when the body arrives),
  // the encodeJsonRpcData logic will not execute and transcoding will fail. While rare for
  // standard REST/JSON APIs, trailers are a native part of the HTTP spec and need to be
  // handled properly.
  return Http::FilterTrailersStatus::Continue;
}

void McpJsonRestBridgeFilter::handleMcpMethod(Http::RequestHeaderMapOptRef request_headers) {
  ENVOY_STREAM_LOG(debug, "Handling MCP JSON-RPC via streaming parser", *decoder_callbacks_);

  if (!validateJsonRpcIdAndMethod().ok()) {
    return;
  }

  absl::string_view method =
      parser_->getNestedValue(std::string(McpConstants::METHOD_FIELD))->string_value();

  if (!validateRequestMcpVersion(method, request_headers, config_->fallbackProtocolVersion())) {
    sendErrorResponse(Http::Code::BadRequest,
                      "mcp_json_rest_bridge_filter_unsupported_protocol_version",
                      generateErrorJsonResponse(-32602, "Unsupported protocol version").dump());
    return;
  }
  // TODO(guoyilin42): Consider supporting local response for tools/list in addition to the GET.
  if (method == McpConstants::Methods::TOOLS_LIST) {
    absl::StatusOr<envoy::extensions::filters::http::mcp_json_rest_bridge::v3::HttpRule> http_rule =
        config_->getToolsListHttpRule();
    if (http_rule.ok() && !http_rule->get().empty()) {
      mcp_operation_ = McpOperation::ToolsList;
      // We don't support pagination for the tools/list request for now.
      if (request_headers.has_value()) {
        request_headers->setPath(http_rule->get());
        request_headers->setMethod(Http::Headers::get().MethodValues.Get);
        request_headers->removeTransferEncoding();
        request_headers->removeContentLength();
        request_headers->removeContentType();
        // Set AcceptEncoding to "identity" to prevent server encoding the response.
        request_headers->setCopy(Http::CustomHeaders::get().AcceptEncoding,
                                 Http::CustomHeaders::get().AcceptEncodingValues.Identity);
      }

      if (decoder_callbacks_->downstreamCallbacks().has_value()) {
        decoder_callbacks_->downstreamCallbacks()->clearRouteCache();
      }
    } else {
      // If the tools/list http rule is not configured, the request should be passed through.
      mcp_operation_ = McpOperation::Unspecified;
      json ret = {
          {McpConstants::JSONRPC_FIELD, McpConstants::JSONRPC_VERSION},
          {McpConstants::ID_FIELD, session_id_},
          {McpConstants::METHOD_FIELD, McpConstants::Methods::TOOLS_LIST},
      };
      request_body_str_ = ret.dump();
    }
  } else if (method == McpConstants::Methods::INITIALIZE) {
    mcp_operation_ = McpOperation::Initialization;
    if (const Protobuf::Value* params_val =
            parser_->getNestedValue(std::string(McpConstants::PARAMS_FIELD));
        params_val && params_val->has_struct_value()) {
      if (auto it = params_val->struct_value().fields().find(McpConstants::PROTOCOL_VERSION_FIELD);
          it != params_val->struct_value().fields().end() && it->second.has_string_value()) {
        decoder_callbacks_->sendLocalReply(
            Http::Code::OK,
            generateInitializeResponse(session_id_, server_name_, it->second.string_value()).dump(),
            [](Http::ResponseHeaderMap& headers) {
              headers.setContentType(Http::Headers::get().ContentTypeValues.Json);
            },
            Grpc::Status::WellKnownGrpcStatus::Ok, "mcp_json_rest_bridge_filter_initialize");
        return;
      }
    }
    sendErrorResponse(
        Http::Code::BadRequest, "mcp_json_rest_bridge_filter_initialize_request_not_valid",
        generateErrorJsonResponse(-32602, "Missing valid protocolVersion in initialize request")
            .dump());
  } else if (method == McpConstants::Methods::NOTIFICATION_INITIALIZED) {
    mcp_operation_ = McpOperation::InitializationAck;
    // TODO(guoyilin42): We may need to explicitly set `content-length: 0` to prevent curl from
    // hanging. `modify_headers` fails here as `sendLocalReply` removes it for empty bodies.
    decoder_callbacks_->sendLocalReply(Http::Code::Accepted, "", nullptr,
                                       Grpc::Status::WellKnownGrpcStatus::Ok,
                                       "mcp_json_rest_bridge_filter_initialize_ack");
  } else if (method == McpConstants::Methods::TOOLS_CALL) {
    mcp_operation_ = McpOperation::ToolsCall;
    mapMcpToolToApiBackend();
  } else {
    sendErrorResponse(
        Http::Code::BadRequest, "mcp_json_rest_bridge_filter_method_not_supported",
        generateErrorJsonResponse(-32601, absl::StrCat("Method ", method, " is not supported"))
            .dump());
    return;
  }
}

void McpJsonRestBridgeFilter::encodeJsonRpcData(Http::ResponseHeaderMapOptRef response_headers) {
  const size_t total_size = response_body_.length();
  const char* json_ptr = static_cast<const char*>(response_body_.linearize(total_size));
  ENVOY_STREAM_LOG(debug, "Encoding Json-RPC data from response body: {}", *encoder_callbacks_,
                   absl::string_view(json_ptr, total_size));
  switch (mcp_operation_) {
  case McpOperation::ToolsList: {
    json tools = json::parse(json_ptr, json_ptr + total_size, /*parser_callback_t=*/nullptr,
                             /*allow_exceptions=*/false);
    if (tools.is_discarded() ||
        getResponseCode(response_headers) >= static_cast<int>(Http::Code::BadRequest)) {
      ENVOY_STREAM_LOG(error, "Tool list response is invalid or has error status code.",
                       *encoder_callbacks_);
      json ret = {
          {McpConstants::JSONRPC_FIELD, McpConstants::JSONRPC_VERSION},
          {McpConstants::ID_FIELD, session_id_},
          {McpConstants::ERROR_FIELD, generateErrorJsonResponse(-32000, "Server error")},
      };
      response_body_str_ = ret.dump();
      break;
    }
    json ret = {
        {McpConstants::JSONRPC_FIELD, McpConstants::JSONRPC_VERSION},
        {McpConstants::ID_FIELD, session_id_},
        {McpConstants::RESULT_FIELD, tools},
    };
    response_body_str_ = ret.dump();
    break;
  }
  case McpOperation::ToolsCall: {
    // The tool call response is in JSON REST format. Translates it to JSON-RPC.
    if (!utf8_range::IsStructurallyValid(absl::string_view(json_ptr, total_size))) {
      ENVOY_STREAM_LOG(
          warn,
          "API backend returns an invalid UTF-8 payload response. Returns error back to client.",
          *encoder_callbacks_);
      response_body_str_ =
          translateJsonRestResponseToJsonRpc("Backend response returns an invalid UTF-8 payload.",
                                             session_id_, true)
              .dump();
    } else {
      response_body_str_ =
          translateJsonRestResponseToJsonRpc(absl::string_view(json_ptr, total_size), session_id_,
                                             getResponseCode(response_headers) >=
                                                 static_cast<int>(Http::Code::BadRequest))
              .dump();
    }
    break;
  }
  case McpOperation::OperationFailed: {
    // TODO(guoyilin42): Construct the full JSON-RPC error response directly in `sendErrorResponse`
    // to avoid this inefficient serialization-then-deserialization cycle and simplify the code.
    json error = json::parse(json_ptr, json_ptr + total_size, /*parser_callback_t=*/nullptr,
                             /*allow_exceptions=*/false);
    if (error.is_discarded()) {
      ENVOY_STREAM_LOG(error, "Failed to parse error response.", *encoder_callbacks_);
      return;
    }
    json ret = {{McpConstants::JSONRPC_FIELD, McpConstants::JSONRPC_VERSION},
                // If the ID is missing in the request, the ID in the response should be null.
                {McpConstants::ID_FIELD, session_id_},
                {McpConstants::ERROR_FIELD, error}};
    response_body_str_ = ret.dump();
    break;
  }
  default:
    break;
  }

  if (response_headers.has_value()) {
    const auto transfer_encoding = response_headers->TransferEncoding();
    const bool is_chunked =
        transfer_encoding != nullptr &&
        absl::EqualsIgnoreCase(transfer_encoding->value().getStringView(),
                               Http::Headers::get().TransferEncodingValues.Chunked);
    if (is_chunked) {
      response_headers->removeContentLength();
    } else {
      response_headers->setContentLength(response_body_str_.size());
    }
    response_headers->setContentType(Http::Headers::get().ContentTypeValues.Json);
  }
}

void McpJsonRestBridgeFilter::mapMcpToolToApiBackend() {
  const Protobuf::Value* name_val = parser_->getNestedValue("params.name");
  if (!name_val || !name_val->has_string_value()) {
    ENVOY_STREAM_LOG(error, "Failed to get the name of the tool call request.",
                     *decoder_callbacks_);
    sendErrorResponse(Http::Code::BadRequest, "mcp_json_rest_bridge_filter_tool_name_not_found",
                      generateErrorJsonResponse(-32602, "Tool name not found").dump());
    return;
  }
  absl::string_view tool_name = name_val->string_value();

  absl::StatusOr<envoy::extensions::filters::http::mcp_json_rest_bridge::v3::HttpRule> http_rule =
      config_->getHttpRule(tool_name);
  if (!http_rule.ok()) {
    ENVOY_STREAM_LOG(error, "Failed to get http rule for method: {}", *decoder_callbacks_,
                     tool_name);
    sendErrorResponse(Http::Code::BadRequest, "mcp_json_rest_bridge_filter_unknown_tool",
                      generateErrorJsonResponse(-32602, "Unknown tool").dump());
    return;
  }

  const Protobuf::Value* arguments_val =
      parser_->getNestedValue(std::string(McpConstants::Paths::PARAMS_ARGUMENTS));
  if (arguments_val && !arguments_val->has_struct_value()) {
    ENVOY_STREAM_LOG(error, "The arguments of the tool call request must be an object.",
                     *decoder_callbacks_);
    sendErrorResponse(Http::Code::BadRequest, "mcp_json_rest_bridge_filter_tool_arguments_invalid",
                      generateErrorJsonResponse(-32602, "Tool arguments must be an object").dump());
    return;
  }

  Protobuf::Value empty_val;
  empty_val.mutable_struct_value();
  const Protobuf::Value& arguments = arguments_val ? *arguments_val : empty_val;

  absl::StatusOr<HttpRequest> http_request = buildHttpRequest(*http_rule, arguments);
  if (!http_request.ok()) {
    ENVOY_STREAM_LOG(error, "Failed to build HTTP request for method: {} with status: {}",
                     *decoder_callbacks_, tool_name, http_request.status());
    sendErrorResponse(Http::Code::BadRequest, "mcp_json_rest_bridge_filter_invalid_tool_arguments",
                      generateErrorJsonResponse(-32602, "Invalid tool arguments").dump());
    return;
  }

  request_body_str_ = http_request->body;
  ENVOY_STREAM_LOG(debug, "Mapping MCP tool to HTTP request url: {} method: {} body: {}",
                   *decoder_callbacks_, http_request->url, http_request->method, request_body_str_);

  auto request_headers = decoder_callbacks_->requestHeaders();
  if (request_headers.has_value()) {
    request_headers->setPath(http_request->url);
    request_headers->setMethod(http_request->method);
    const auto transfer_encoding = request_headers->TransferEncoding();
    const bool is_chunked =
        transfer_encoding != nullptr &&
        absl::EqualsIgnoreCase(transfer_encoding->value().getStringView(),
                               Http::Headers::get().TransferEncodingValues.Chunked);
    if (is_chunked) {
      request_headers->removeContentLength();
    } else {
      request_headers->setContentLength(request_body_str_.size());
    }
    request_headers->setContentType(Http::Headers::get().ContentTypeValues.Json);
    // Set AcceptEncoding to "identity" to prevent server encoding the response.
    request_headers->setCopy(Http::CustomHeaders::get().AcceptEncoding,
                             Http::CustomHeaders::get().AcceptEncodingValues.Identity);
  }

  if (decoder_callbacks_->downstreamCallbacks().has_value()) {
    decoder_callbacks_->downstreamCallbacks()->clearRouteCache();
  }
}

void McpJsonRestBridgeFilter::sendErrorResponse(Http::Code response_code,
                                                absl::string_view response_code_details,
                                                absl::string_view response_body) {
  ENVOY_STREAM_LOG(error, "Sending error response with response code details: {}",
                   *decoder_callbacks_, response_code_details);
  mcp_operation_ = McpOperation::OperationFailed;
  decoder_callbacks_->sendLocalReply(response_code, response_body, nullptr,
                                     Grpc::Status::WellKnownGrpcStatus::Internal,
                                     response_code_details);
}

absl::Status McpJsonRestBridgeFilter::validateJsonRpcIdAndMethod() {
  if (const Protobuf::Value* id_val = parser_->getNestedValue(std::string(McpConstants::ID_FIELD));
      id_val) {
    absl::StatusOr<json> json_id = getSessionId(*id_val);
    if (!json_id.ok()) {
      ENVOY_STREAM_LOG(debug,
                       "Failed to parse JSON-RPC request ID: {}. This is expected for notification "
                       "requests, which do not have an ID.",
                       *decoder_callbacks_, json_id.status().message());
    } else {
      session_id_ = *json_id;
    }
  }

  const Protobuf::Value* method_val =
      parser_->getNestedValue(std::string(McpConstants::METHOD_FIELD));
  if (!method_val) {
    sendErrorResponse(Http::Code::BadRequest, "mcp_json_rest_bridge_filter_method_not_found",
                      generateErrorJsonResponse(-32601, "Missing method field").dump());
    return absl::InvalidArgumentError("Missing method field");
  }
  if (!method_val->has_string_value()) {
    sendErrorResponse(Http::Code::BadRequest, "mcp_json_rest_bridge_filter_method_not_string",
                      generateErrorJsonResponse(-32601, "Method field is not a string").dump());
    return absl::InvalidArgumentError("Method field is not a string");
  }

  if (method_val->string_value() == McpConstants::Methods::NOTIFICATION_INITIALIZED) {
    // The notifications/initialized request is not required to have an ID
    // field.
  } else if (session_id_.is_null()) {
    // The JSON-RPC 2.0 specification strongly discourages the use of Null as a request ID.
    // To prevent ambiguity between missing IDs (notifications) and explicit Null IDs,
    // we reject requests that use Null as an ID.
    sendErrorResponse(Http::Code::BadRequest, "mcp_json_rest_bridge_filter_id_not_found",
                      generateErrorJsonResponse(-32600, "Missing ID field").dump());
    return absl::InvalidArgumentError("Missing ID field");
  }
  return absl::OkStatus();
}

} // namespace McpJsonRestBridge
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
