#include "source/extensions/filters/http/mcp_json_rest_bridge/http_request_builder.h"

#include "source/common/http/utility.h"
#include "source/common/protobuf/protobuf.h" // IWYU pragma: keep

#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "re2/re2.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace McpJsonRestBridge {
namespace {

absl::StatusOr<const Protobuf::Value*> getProtobufValue(const Protobuf::Value& data,
                                                        absl::string_view path) {
  if (path.empty()) {
    return &data;
  }
  const Protobuf::Value* current = &data;

  for (absl::string_view part : absl::StrSplit(path, '.')) {
    if (!current->has_struct_value()) {
      return absl::InvalidArgumentError(absl::StrCat("Path element is not an object: ", part));
    }

    auto it = current->struct_value().fields().find(part);
    if (it == current->struct_value().fields().end()) {
      return absl::InvalidArgumentError(absl::StrCat("Could not find value for path: ", path));
    }
    current = &it->second;
  }
  return current;
}

absl::StatusOr<std::string> protobufValueToString(const Protobuf::Value& v) {
  // Extract the raw string directly. If we use MessageToJsonString for a string value,
  // it will add surrounding double quotes (e.g., "value"), which would then be
  // incorrectly URL-encoded as %22value%22 in URL paths and query parameters.
  if (v.kind_case() == Protobuf::Value::kStringValue) {
    return v.string_value();
  }
  std::string json_str;
  if (absl::Status status = Protobuf::util::MessageToJsonString(v, &json_str); !status.ok()) {
    return absl::InvalidArgumentError("Failed to convert Protobuf::Value to JSON string");
  }
  return json_str;
}

// Key and value for HTTP query parameter.
struct QueryParam {
  std::string key;
  std::string value;
};

absl::Status constructQueryParams(std::vector<QueryParam>& query_params,
                                  absl::string_view body_rule, const Protobuf::Value& arguments,
                                  const absl::flat_hash_set<std::string>& templates,
                                  absl::string_view path) {
  // Skip if it's a URL path template
  if (templates.contains(path)) {
    return absl::OkStatus();
  }

  // Skip if it's part of the body
  if (!body_rule.empty()) {
    if (path == body_rule || (absl::StartsWith(path, body_rule) && path[body_rule.size()] == '.')) {
      return absl::OkStatus();
    }
  }

  switch (arguments.kind_case()) {
  case Protobuf::Value::kStructValue:
    for (const auto& [key, value] : arguments.struct_value().fields()) {
      RETURN_IF_NOT_OK(constructQueryParams(query_params, body_rule, value, templates,
                                            path.empty() ? key : absl::StrCat(path, ".", key)));
    }
    return absl::OkStatus();
  case Protobuf::Value::kListValue:
    for (const auto& list_item : arguments.list_value().values()) {
      RETURN_IF_NOT_OK(constructQueryParams(query_params, body_rule, list_item, templates, path));
    }
    return absl::OkStatus();
  case Protobuf::Value::kNullValue:
  case Protobuf::Value::kNumberValue:
  case Protobuf::Value::kStringValue:
  case Protobuf::Value::kBoolValue: {
    absl::StatusOr<std::string> value = protobufValueToString(arguments);
    RETURN_IF_NOT_OK(value.status());
    // Uses Http::Utility::PercentEncoding::urlEncode to escape the value.
    query_params.push_back({std::string(path), Http::Utility::PercentEncoding::urlEncode(*value)});
    return absl::OkStatus();
  }
  case Protobuf::Value::KIND_NOT_SET:
    return absl::InvalidArgumentError("Invalid Protobuf::Value: KIND_NOT_SET");
  }
  return absl::OkStatus();
}

void appendQueryParamsToBaseUrl(std::string& url, absl::Span<const QueryParam> query_params) {
  if (query_params.empty()) {
    return;
  }
  url += "?";
  url += absl::StrJoin(query_params, "&", [](std::string* out, const QueryParam& query_param) {
    absl::StrAppend(out, Http::Utility::PercentEncoding::urlEncode(query_param.key), "=",
                    query_param.value);
  });
}

// Removes a path from a Protobuf Struct.
void removeProtobufPath(Protobuf::Struct& data, absl::string_view path) {
  if (path.empty()) {
    return;
  }

  std::vector<Protobuf::Struct*> structs;
  structs.push_back(&data);

  std::vector<absl::string_view> parts = absl::StrSplit(path, '.');
  for (size_t i = 0; i < parts.size() - 1; ++i) {
    auto it = structs.back()->mutable_fields()->find(parts[i]);
    if (it == structs.back()->mutable_fields()->end() || !it->second.has_struct_value()) {
      return;
    }
    structs.push_back(it->second.mutable_struct_value());
  }

  // Remove the leaf node.
  structs.back()->mutable_fields()->erase(parts.back());

  // Clean up empty parent structs.
  for (int i = structs.size() - 1; i > 0; --i) {
    if (structs[i]->fields().empty()) {
      structs[i - 1]->mutable_fields()->erase(parts[i - 1]);
    } else {
      break;
    }
  }
}

absl::StatusOr<std::string> constructRequestBody(absl::string_view body_rule,
                                                 const absl::flat_hash_set<std::string>& templates,
                                                 const Protobuf::Value& arguments) {
  if (body_rule.empty()) {
    return "";
  }
  if (body_rule == "*") {
    Protobuf::Struct body = arguments.struct_value();
    for (const auto& path : templates) {
      removeProtobufPath(body, path);
    }
    std::string json_str;
    if (absl::Status status = Protobuf::util::MessageToJsonString(body, &json_str); !status.ok()) {
      return absl::InvalidArgumentError("Failed to convert body to JSON string");
    }
    return json_str;
  }

  absl::StatusOr<const Protobuf::Value*> value = getProtobufValue(arguments, body_rule);
  RETURN_IF_NOT_OK(value.status());

  return protobufValueToString(**value);
}

} // namespace

absl::StatusOr<std::string> constructBaseUrl(absl::string_view pattern,
                                             const absl::flat_hash_set<std::string>& templates,
                                             const Protobuf::Value& arguments) {
  std::string base_url = std::string(pattern);
  for (const auto& element : templates) {
    absl::StatusOr<const Protobuf::Value*> value = getProtobufValue(arguments, element);
    RETURN_IF_NOT_OK(value.status());
    absl::StatusOr<std::string> template_str = protobufValueToString(**value);
    RETURN_IF_NOT_OK(template_str.status());
    // Non-visible ASCII characters are always escaped by Http::Utility::PercentEncoding::encode,
    // in addition to the specified reserved characters.
    std::string value_str = Http::Utility::PercentEncoding::encode(*template_str, ReservedChars);
    std::string var_pattern = absl::StrCat("\\{", RE2::QuoteMeta(element), "(?:=[^}]+)?\\}");
    RE2::GlobalReplace(&base_url, var_pattern, value_str);
  }
  return base_url;
}

absl::StatusOr<HttpRequest> buildHttpRequest(
    const envoy::extensions::filters::http::mcp_json_rest_bridge::v3::HttpRule& http_rule,
    const Protobuf::Value& arguments) {
  std::string pattern;
  std::string method;
  // TODO(guoyilin42): Add validation to ensure exactly one HTTP method is specified.
  if (!http_rule.get().empty()) {
    method = "GET";
    pattern = http_rule.get();
  } else if (!http_rule.put().empty()) {
    method = "PUT";
    pattern = http_rule.put();
  } else if (!http_rule.post().empty()) {
    method = "POST";
    pattern = http_rule.post();
  } else if (!http_rule.delete_().empty()) {
    method = "DELETE";
    pattern = http_rule.delete_();
  } else if (!http_rule.patch().empty()) {
    method = "PATCH";
    pattern = http_rule.patch();
  } else {
    return absl::InvalidArgumentError("Unsupported HTTP method in HttpRule");
  }
  absl::string_view url_template = pattern;
  absl::flat_hash_set<std::string> templates;
  std::string template_capture;
  static const LazyRE2 template_regex = {R"(\{([a-zA-Z0-9_.]+)(?:=.*?)?\})"};
  while (RE2::FindAndConsume(&url_template, *template_regex, &template_capture)) {
    templates.insert(template_capture);
  }
  absl::StatusOr<std::string> url = constructBaseUrl(pattern, templates, arguments);
  RETURN_IF_NOT_OK(url.status());

  std::vector<QueryParam> query_params;
  if (http_rule.body() != "*") {
    RETURN_IF_NOT_OK(
        constructQueryParams(query_params, http_rule.body(), arguments, templates, ""));
  }
  appendQueryParamsToBaseUrl(*url, query_params);

  absl::StatusOr<std::string> http_body =
      constructRequestBody(http_rule.body(), templates, arguments);
  RETURN_IF_NOT_OK(http_body.status());

  return HttpRequest{
      .url = *std::move(url),
      .method = std::move(method),
      .body = *std::move(http_body),
  };
}

} // namespace McpJsonRestBridge
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
