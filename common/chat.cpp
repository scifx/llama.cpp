#include "chat.hpp"
#include "chat-template.hpp"
#include "json-schema-to-grammar.h"
#include "log.h"
#include "minja.hpp"

std::string common_chat_format_name(common_chat_format format) {
    switch (format) {
        case COMMON_CHAT_FORMAT_CONTENT_ONLY: return "Content-only";
        case COMMON_CHAT_FORMAT_GENERIC: return "Generic";
        case COMMON_CHAT_FORMAT_MISTRAL_NEMO: return "Mistral Nemo";
        case COMMON_CHAT_FORMAT_LLAMA_3_X: return "Llama 3.x";
        case COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS: return "Llama 3.x with builtin tools";
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1: return "DeepSeek R1";
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1_EXTRACT_REASONING: return "DeepSeek R1 (extract reasoning)";
        case COMMON_CHAT_FORMAT_FIREFUNCTION_V2: return "FireFunction v2";
        case COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2: return "Functionary v3.2";
        case COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1: return "Functionary v3.1 Llama 3.1";
        case COMMON_CHAT_FORMAT_HERMES_2_PRO: return "Hermes 2 Pro";
        case COMMON_CHAT_FORMAT_COMMAND_R7B: return "Command R7B";
        case COMMON_CHAT_FORMAT_COMMAND_R7B_EXTRACT_REASONING: return "Command R7B (extract reasoning)";
        default:
            throw std::runtime_error("Unknown chat format");
    }
}

const common_grammar_options grammar_options {
    /* .dotall = */ false,
    /* .compact_spaces = */ false,
    // /* .compact_spaces = */ true,
};

static bool parse_json(std::string::const_iterator & it, const std::string::const_iterator & end, json & out) {
    // // https://json.nlohmann.me/features/parsing/sax_interface/
    struct json_error_locator : public nlohmann::json_sax<json> {
        std::size_t position;
        bool found_error;

        json_error_locator() : position(0), found_error(false) {}

        bool parse_error(std::size_t position, const std::string &, const json::exception &) override {
            this->position = position - 1;
            this->found_error = true;
            return false;
        }
        bool null() override { return true; }
        bool boolean(bool) override { return true; }
        bool number_integer(number_integer_t) override { return true; }
        bool number_unsigned(number_unsigned_t) override { return true; }
        bool number_float(number_float_t, const string_t &) override { return true; }
        bool string(string_t &) override { return true; }
        bool binary(binary_t &) override { return true; }
        bool start_object(std::size_t) override { return true; }
        bool key(string_t &) override { return true; }
        bool end_object() override { return true; }
        bool start_array(std::size_t) override { return true; }
        bool end_array() override { return true; }
    };
    json_error_locator err_loc;
    json::sax_parse(it, end, &err_loc);

    std::string::const_iterator temptative_end;
    if (err_loc.found_error) {
        temptative_end = it + err_loc.position;
    } else {
        temptative_end = end;
    }
    std::string json_sub {it, temptative_end};
    try {
        out = json::parse(json_sub);
        it = temptative_end;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}


/**
 * Takes a prefix regex that must have 1 group to capture the function name, a closing suffix, and expects json parameters in between.
 * Aggregates the prefix, suffix and in-between text into the content.
 */
static common_chat_msg parse_json_tool_calls(
    const std::string& input,
    const std::optional<std::regex> & trigger_opt,
    const std::regex & function_regex,
    const std::regex & close_regex) {
    std::smatch match;

    common_chat_msg result;
    result.role = "assistant";


    auto end = input.end();
    auto it = input.begin();

    if (trigger_opt) {
        if (!std::regex_search(it, end, match, *trigger_opt)) {
            result.content = input;
            return result;
        }
        result.content = match.prefix().str();
        it = match.suffix().first;
    }

    while (it != end) {
        std::sregex_iterator rend;
        std::sregex_iterator rit(it, end, function_regex);
        if (rit == rend) {
            result.content += std::string(it, end);
            break;
        }
        auto name = rit->str(1);
        result.content += std::string(it, rit->prefix().second);
        it = rit->suffix().first;

        json arguments;
        if (!parse_json(it, end, arguments)) {
            throw std::runtime_error("Failed to parse json tool call arguments: " + input);
        }
        if (!std::regex_search(it, end, match, close_regex)) {
            throw std::runtime_error("Malformed input, missing closing pattern: " + input);
        }
        it = match.suffix().first;
        result.tool_calls.push_back({name, arguments.is_string() ? arguments.get<std::string>() : arguments.dump(), /* id= */ ""});
    }

    if (!result.tool_calls.empty()) {
        if (!string_strip(result.content).empty()) {
            LOG_WRN("Content found with tool calls: %s\n", result.content.c_str());
        }
        result.content = "";
    }
    return result;
}

static common_chat_msg parse_prefixed_json_tool_call_array(const std::string& input, const std::string & prefix, size_t rstrip_prefix = 0) {
    auto content_end = input.find(prefix);
    size_t tc_start = std::string::npos;

    common_chat_msg result;
    result.role = "assistant";
    const auto process_tool_calls = [&](const json & tool_calls) {
        for (const auto & tool_call : tool_calls) {
            const auto & arguments = tool_call.at("arguments");
            result.tool_calls.push_back({
                tool_call.at("name"),
                arguments.is_string() ? arguments.get<std::string>() : arguments.dump(),
                tool_call.contains("id") ? tool_call.at("id") : "",
            });
        }
    };
    if (content_end == std::string::npos) {
        result.content = input;
    } else {
        tc_start = content_end + prefix.size() - rstrip_prefix;
        result.content = input.substr(0, content_end);
        auto tool_calls = json::parse(input.substr(tc_start));
        process_tool_calls(tool_calls);
    }
    return result;
}

static void foreach_function(const json & tools, const std::function<void(const json &)> & fn) {
    for (const auto & tool : tools) {
        if (!tool.contains("type") || tool.at("type") != "function" || !tool.contains("function")) {
            LOG_INF("Skipping tool without function: %s", tool.dump(2).c_str());
            continue;
        }
        fn(tool);
    }
}

static std::string apply(
    const common_chat_template & tmpl,
    const nlohmann::ordered_json & messages,
    const nlohmann::ordered_json & tools,
    bool add_generation_prompt,
    const nlohmann::ordered_json & extra_context = nlohmann::ordered_json())
{
    minja::chat_template_inputs tmpl_inputs;
    tmpl_inputs.messages = messages;
    tmpl_inputs.tools = tools;
    tmpl_inputs.add_generation_prompt = add_generation_prompt;
    tmpl_inputs.extra_context = extra_context;
    // TODO: add flag to control date/time, if only for testing purposes.
    // tmpl_inputs.now = std::chrono::system_clock::now();

    minja::chat_template_options tmpl_opts;
    tmpl_opts.use_bos_token = false;
    tmpl_opts.use_eos_token = false;

    return tmpl.apply(tmpl_inputs, tmpl_opts);
}

static common_chat_params common_chat_params_init_generic(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    common_chat_params data;

    auto tool_call_schemas = json::array();
    foreach_function(inputs.tools, [&](const json & tool) {
        const auto & function = tool.at("function");
        auto tool_schema = json {
            {"type", "object"},
            {"properties", {
                {"name", {
                    {"type", "string"},
                    {"const", function.at("name")},
                }},
                {"arguments", function.at("parameters")},
            }},
            {"required", json::array({"name", "arguments"})},
        };
        if (function.contains("description")) {
            tool_schema["description"] = function.at("description");
        }
        if (inputs.parallel_tool_calls) {
            tool_schema.at("properties")["id"] = {
                {"type", "string"},
                {"minLength", 4},
            };
            tool_schema.at("required").push_back("id");
        }
        tool_call_schemas.emplace_back(tool_schema);
    });
    const auto tool_call =
        inputs.parallel_tool_calls
            ? json {
                {"type", "object"},
                {"properties", {
                    {"tool_calls", {
                        {"type", "array"},
                        {"items", tool_call_schemas.size() == 1 ? tool_call_schemas[0] : json {
                            {"anyOf", tool_call_schemas},
                        }},
                        {"minItems", 1},
                    }},
                }},
                {"required", json::array({"tool_calls"})},
            }
            : json {
                {"type", "object"},
                {"properties", {
                    {"tool_call", tool_call_schemas.size() == 1 ? tool_call_schemas[0] : json {
                        {"anyOf", tool_call_schemas},
                    }},
                }},
                {"required", json::array({"tool_call"})},
            };
    const auto schema =
        inputs.tool_choice != "required"
            ? json {
                {"anyOf", json::array({
                    tool_call,
                    {
                        {"type", "object"},
                        {"properties", {
                            {"response", inputs.json_schema.is_null()
                                ? json {{"type", "string"}}
                                : inputs.json_schema
                            },
                        }},
                        {"required", json::array({"response"})},
                    },
                })}
            }
            : tool_call;

    data.grammar_lazy = false;
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        builder.add_schema("root", schema);
    }, grammar_options);

    auto tweaked_messages = common_chat_template::add_system(
        inputs.messages,
        "Respond in JSON format, either with `tool_call` (a request to call tools) or with `response` reply to the user's request");

    data.prompt = apply(tmpl, tweaked_messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_GENERIC;
    return data;
}
static common_chat_msg common_chat_parse_generic(const std::string & input) {
    json data = json::parse(input);
    common_chat_msg result;
    result.role = "assistant";
    if (data.contains("tool_calls")) {
        for (const auto & tool_call : data.at("tool_calls")) {
            result.tool_calls.push_back({
                tool_call.at("name"),
                tool_call.at("arguments").dump(),
                tool_call.contains("id") ? tool_call.at("id") : "",
            });
        }
    } else if (data.contains("tool_call")) {
        result.tool_calls.push_back({
            data.at("tool_call").at("name"),
            data.at("tool_call").at("arguments").dump(),
            /* id= */ "",
        });
    } else if (data.contains("response")) {
        const auto & response = data.at("response");
        result.content = response.is_string() ? response.get<std::string>() : response.dump(2);
    }
    return result;
}

static common_chat_params common_chat_params_init_mistral_nemo(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    common_chat_params data;
    data.grammar_lazy = inputs.tool_choice != "required";
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        auto schemas = json::array();
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            schemas.push_back({
                {"type", "object"},
                {"properties", {
                    // Important note: the model is probably trained to take a JSON stringified arguments value.
                    // It's hard to constrain that for now (while reusing the JSON schema conversion), so we're just expecting a plain object.
                    {"name", {
                        {"type", "string"},
                        {"const", function.at("name")},
                    }},
                    {"arguments", function.at("parameters")},
                    {"id", {
                        {"type", "string"},
                        // Nemo's template expects a 9-character alphanumeric ID.
                        {"pattern", "^[a-zA-Z0-9]{9}$"},
                    }},
                }},
                {"required", json::array({"name", "arguments", "id"})},
            });
        });
        auto schema = json {
            {"type", "array"},
            {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
            {"minItems", 1},
        };
        if (!inputs.parallel_tool_calls) {
            schema["maxItems"] = 1;
        }
        builder.add_rule("root", "\"[TOOL_CALLS]\" " + builder.add_schema("tool_calls", schema));
    }, grammar_options);
    data.grammar_triggers.push_back({"[TOOL_CALLS]", /* .at_start = */ true});
    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_MISTRAL_NEMO;
    return data;
}
static common_chat_msg common_chat_parse_mistral_nemo(const std::string & input) {
    return parse_prefixed_json_tool_call_array(input, "[TOOL_CALLS]");
}

static common_chat_params common_chat_params_init_command_r7b(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    common_chat_params data;
    data.grammar_lazy = inputs.tool_choice != "required";
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        auto schemas = json::array();
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            schemas.push_back({
                {"type", "object"},
                {"properties", {
                    {"tool_call_id", {
                        {"type", "string"},
                        // Command-R's template expects an integer string.
                        {"pattern", "^[0-9]{1,10}$"},
                    }},
                    {"tool_name", {
                        {"type", "string"},
                        {"const", function.at("name")},
                    }},
                    {"parameters", function.at("parameters")},
                }},
                {"required", json::array({"tool_call_id", "tool_name", "parameters"})},
            });
        });
        auto schema = json {
            {"type", "array"},
            {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
            {"minItems", 1},
        };
        if (!inputs.parallel_tool_calls) {
            schema["maxItems"] = 1;
        }
        builder.add_rule("root", "\"<|START_ACTION|>\" " + builder.add_schema("tool_calls", schema) + " \"<|END_ACTION|>\"");
    }, grammar_options);
    data.grammar_triggers.push_back({"<|START_ACTION|>", /* .at_start = */ false});
    data.preserved_tokens = {
        "<|START_RESPONSE|>",
        "<|END_RESPONSE|>",
        "<|START_THINKING|>",
        "<|END_THINKING|>",
        "<|END_ACTION|>",
    };
    auto adjusted_messages = json::array();
    for (const auto & msg : inputs.messages) {
        auto has_reasoning_content = msg.contains("reasoning_content") && msg.at("reasoning_content").is_string();
        auto has_tool_calls = msg.contains("tool_calls") && msg.at("tool_calls").is_array();
        if (has_reasoning_content && has_tool_calls) {
            auto adjusted_message = msg;
            adjusted_message["tool_plan"] = msg.at("reasoning_content");
            adjusted_message.erase("reasoning_content");
            adjusted_messages.push_back(adjusted_message);
        } else {
            adjusted_messages.push_back(msg);
        }
    }
    data.prompt = apply(tmpl, adjusted_messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt, {});
    data.format = inputs.extract_reasoning ? COMMON_CHAT_FORMAT_COMMAND_R7B_EXTRACT_REASONING : COMMON_CHAT_FORMAT_COMMAND_R7B;
    return data;
}
static common_chat_msg common_chat_parse_command_r7b(const std::string & input, bool extract_reasoning) {
    static std::regex thought_regex("(<\\|START_THINKING\\|>([\\s\\S\\n\\r]*?)<\\|END_THINKING\\|>)([\\s\\S\\n\\r]*)");
    static std::regex action_regex("<\\|START_ACTION\\|>([\\s\\S\\n\\r]*?)<\\|END_ACTION\\|>");
    static std::regex response_regex("(?:<\\|START_RESPONSE\\|>)?([\\s\\S\\n\\r]*?)<\\|END_RESPONSE\\|>");

    std::smatch match;

    common_chat_msg result;
    result.role = "assistant";

    std::string rest = input;

    if (std::regex_match(rest, match, thought_regex)) {
        if (extract_reasoning) {
            result.reasoning_content = match[2].str();
        } else if (!match[2].str().empty()) {
            // Let the unparsed thinking tags through in content only if their insides aren't empty.
            result.content = match[1].str();
        }
        rest = match[3].str();
    }
    if (std::regex_match(rest, match, action_regex)) {
        auto actions_str = match[1].str();
        auto actions = json::parse(actions_str);
        for (const auto & action : actions) {
            result.tool_calls.push_back({
                /* .name = */      action.at("tool_name"),
                /* .arguments = */ action.at("parameters").dump(),
                /* .id = */        action.at("tool_call_id"),
            });
        }
    } else if (std::regex_match(rest, match, response_regex)) {
        auto response = match[1].str();
        result.content += response;
    } else {
        result.content += rest;
    }
    return result;
}

static void expect_tool_parameters(const std::string & name, const json & parameters, const std::vector<std::string> & expected_properties) {
    if (!parameters.is_object() || !parameters.contains("type") || parameters.at("type") != "object" || !parameters.contains("properties") || !parameters.contains("required")) {
        throw std::runtime_error("Parameters of tool " + name + " must be an object w/ required properties");
    }
    const auto & parameters_properties = parameters.at("properties");
    const auto & parameters_required = parameters.at("required");
    for (const auto & prop : expected_properties) {
        if (!parameters_properties.contains(prop)) {
            throw std::runtime_error("Parameters of tool " + name + " is missing property: " + prop);
        }
        if (std::find(parameters_required.begin(), parameters_required.end(), json(prop)) == parameters_required.end()) {
            throw std::runtime_error("Parameters of tool " + name + " must have property marked as required: " + prop);
        }
    }
    if (parameters_properties.size() != expected_properties.size()) {
        throw std::runtime_error("Parameters of tool " + name + " must only have these properties:" + string_join(expected_properties, ", "));
    }
}

static common_chat_params common_chat_params_init_llama_3_1_tool_calls(const common_chat_template & tmpl, const struct common_chat_inputs & inputs, bool allow_python_tag_builtin_tools) {
    auto builtin_tools = json::array();
    common_chat_params data;
    data.grammar_lazy = inputs.tool_choice != "required";
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        std::vector<std::string> tool_rules;

        auto handle_builtin_tool = [&](const std::string & name, const json & parameters) {
            if (name == "wolfram_alpha") {
                // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/remote/tool_runtime/wolfram_alpha/wolfram_alpha.py
                expect_tool_parameters(name, parameters, {"query"});
            } else if (name == "web_search" || name == "brave_search") {
                // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/remote/tool_runtime/brave_search/brave_search.py
                expect_tool_parameters(name, parameters, {"query"});
            } else if (name == "python" || name == "code_interpreter") {
                // https://github.com/meta-llama/llama-stack/blob/main/llama_stack/providers/inline/tool_runtime/code_interpreter/code_interpreter.py
                expect_tool_parameters(name, parameters, {"code"});
            } else {
                return false;
            }

            std::vector<std::string> kvs;
            for (const auto & [key, value] : parameters.at("properties").items()) {
                kvs.push_back("\"" + key + "=\" " + builder.add_schema(name + "-args-" + key, value));
            }

            tool_rules.push_back(
                builder.add_rule(
                    name + "-call",
                    "\"<|python_tag|>" + name + ".call(\" " + string_join(kvs, " \", \" ") + " \")\""));
            builtin_tools.push_back(name);

            return true;
        };

        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            std::string name = function.at("name");
            auto parameters = function.at("parameters");
            builder.resolve_refs(parameters);

            // https://github.com/meta-llama/llama-stack/tree/main/llama_stack/providers/remote/tool_runtime
            if (allow_python_tag_builtin_tools) {
                handle_builtin_tool(name, parameters);
            }
            tool_rules.push_back(
                builder.add_rule(
                    name + "-call",
                    "\"{\" space "
                    "( \"\\\"type\\\":\" space \"\\\"function\\\",\" space )? "
                    "\"\\\"name\\\": \\\"" + name + "\\\", \\\"parameters\\\": \" " +
                        builder.add_schema(name + "-args", parameters) +
                    " \"}\""));
            data.grammar_triggers.push_back({"{\"name\": \"" + name + "\"", /* .at_start = */ true});
        });
        data.grammar_triggers.push_back({"{\"name\":", /* .at_start = */ true});
        data.grammar_triggers.push_back({"{\n  \"name\":", /* .at_start = */ true});
        data.grammar_triggers.push_back({"{\n    \"name\":", /* .at_start = */ true});
        data.grammar_triggers.push_back({"{\"type\": \"function\"", /* .at_start = */ true});
        data.grammar_triggers.push_back({"{\n  \"type\": \"function\"", /* .at_start = */ true});
        data.grammar_triggers.push_back({"{\n    \"type\": \"function\"", /* .at_start = */ true});
        if (!builtin_tools.empty()) {
            data.grammar_triggers.push_back({"<|python_tag|>", /* .at_start = */ false});
        }
        builder.add_rule("root", string_join(tool_rules, " | "));
    }, grammar_options);
    data.additional_stops.push_back("<|eom_id|>");
    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt, {
        {"tools_in_user_message", false},
        {"builtin_tools", builtin_tools.empty() ? json() : builtin_tools},
    });
    data.format = allow_python_tag_builtin_tools && !builtin_tools.empty()
        ? COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS
        : COMMON_CHAT_FORMAT_LLAMA_3_X;
    return data;
}
static common_chat_msg common_chat_parse_llama_3_1(const std::string & input, bool with_builtin_tools = false) {
    // TODO: tighten & simplify the parser, don't accept leading text context.
    static std::regex function_regex("\\{[\\s\\n\\r]*(?:\"type\"[\\s\\n\\r]*:[\\s\\n\\r]*\"function\"[\\s\\n\\r]*,[\\s\\n\\r]*|[\\s\\n\\r]*)\"name\"[\\s\\n\\r]*:[\\s\\n\\r]*\"([^\"]+)\"[\\s\\n\\r]*,[\\s\\n\\r]*\"parameters\": ");
    static std::regex close_regex("\\}");
    static std::regex builtin_call_regex("<\\|python_tag\\|>([^.(]+)\\.call\\((.*)\\)");

    if (with_builtin_tools) {
        std::smatch match;
        if (std::regex_match(input, match, builtin_call_regex)) {
            auto name = match[1].str();
            auto raw_args = match[2].str();

            // TODO: if/when builtin tools start accepting more than 1 argument, use parse_json for real parsing.
            auto it_eq = raw_args.find('=');
            auto arg_name = raw_args.substr(0, it_eq);
            auto arg_value_str = raw_args.substr(it_eq + 1);
            auto arg_value = json::parse(arg_value_str);

            return {
                /* .role = */ "assistant",
                /* .content = */ match.prefix().str(),
                /* .tool_calls = */ {
                    {
                        /* .name = */ match[1],
                        /* .arguments = */ (json {
                            {arg_name, arg_value},
                        }).dump(),
                        /* .id = */ "",
                    },
                },
            };
        }
    }
    return parse_json_tool_calls(input, std::nullopt, function_regex, close_regex);
}

static common_chat_params common_chat_params_init_deepseek_r1(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    common_chat_params data;
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != "required" && inputs.json_schema.is_null();
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                auto args_rule = builder.add_schema(name + "-args", parameters);
                tool_rules.push_back(builder.add_rule(name + "-call",
                    "\"<｜tool▁call▁begin｜>function<｜tool▁sep｜>" + name + "\\n"
                    "```json\\n\" " + args_rule + " \"```<｜tool▁call▁end｜>\""));
            });
            // Distill Qwen 7B & 32B models seem confused re/ syntax of their tool call opening tag,
            // so we accept common variants (then it's all constrained)
            builder.add_rule("root",
                "( \"<｜tool▁calls▁begin｜>\" | \"<｜tool_calls_begin｜>\" | \"<｜tool calls begin｜>\" | \"<｜tool\\\\_calls\\\\_begin｜>\" ) "
                "(" + string_join(tool_rules, " | ") + ")" + (inputs.parallel_tool_calls ? "*" : "") + " "
                "\"<｜tool▁calls▁end｜>\""
                " space");
            data.grammar_triggers.push_back({"<｜tool▁calls▁begin｜>", /* .at_start = */ false});
            data.grammar_triggers.push_back({"<｜tool_calls_begin｜>", /* .at_start = */ false});
            data.grammar_triggers.push_back({"<｜tool calls begin｜>", /* .at_start = */ false});
            data.grammar_triggers.push_back({"<｜tool\\_calls\\_begin｜>", /* .at_start = */ false});
            data.preserved_tokens = {
                "<think>",
                "</think>",
                "<｜tool▁sep｜>",
                "<｜tool▁calls▁end｜",
                "<｜tool▁call▁end｜>",
            };
        }, grammar_options);
    }
    auto prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);

    // Hacks to fix the official (broken) prompt.
    // It is advisable to use --chat-template-file models/templates/llama-cpp-deepseek-r1.jinja instead,
    // until the official template is fixed.
    if (tmpl.source().find("{% if ns.is_tool %}{{'<｜tool▁outputs▁end｜>'}}") != std::string::npos) {
        // Don't leave the chat dangling after tool results
        if (string_ends_with(prompt, "<｜tool▁outputs▁end｜>")) {
            prompt += "<｜end▁of▁sentence｜>";
            if (inputs.add_generation_prompt) {
                prompt += "<｜Assistant｜>";
            }
        }
        // Fix up tool call delta example added by Minja
        prompt = std::regex_replace(
            prompt,
            std::regex("(<｜tool▁call▁end｜>)[\\s\\r\\n]*(<｜tool▁outputs▁begin｜>|<｜User｜>)"),
            "$1<｜tool▁calls▁end｜><｜end▁of▁sentence｜>$2");
    }
    data.prompt = prompt;
    data.format = inputs.extract_reasoning ? COMMON_CHAT_FORMAT_DEEPSEEK_R1_EXTRACT_REASONING : COMMON_CHAT_FORMAT_DEEPSEEK_R1;
    return data;
}
static common_chat_msg common_chat_parse_deepseek_r1(const std::string & input, bool extract_reasoning) {
    static std::regex function_regex("<｜tool▁call▁begin｜>function<｜tool▁sep｜>([^\n]+)\n```json\n");
    static std::regex close_regex("```[\\s\\r\\n]*<｜tool▁call▁end｜>");
    static std::regex reasoning_content_regex("((?:<think>)?([\\s\\S\\r\\n]*?)</think>)?([\\s\\S\\r\\n]*)");
    static std::regex tool_calls_regex("[\\s\\r\\n]*(?:<｜tool▁calls▁begin｜>|<｜tool_calls_begin｜>|<｜tool calls begin｜>|<｜tool\\\\_calls\\\\_begin｜>)([\\s\\S\\r\\n]*?)<｜tool▁calls▁end｜>");
    common_chat_msg msg;
    msg.role = "assistant";
    std::smatch match;
    if (std::regex_match(input, match, reasoning_content_regex)) {
        std::string rest;
        if (extract_reasoning) {
            msg.reasoning_content = string_strip(match[2].str());
        } else {
            msg.content = match[1].str();
        }
        rest = match[3].str();

        if (std::regex_search(rest, match, tool_calls_regex)) {
            auto tool_calls = match[1].str();
            auto msg2 = parse_json_tool_calls(tool_calls, std::nullopt, function_regex, close_regex);
            msg.tool_calls = std::move(msg2.tool_calls);
        } else {
            msg.content += std::string(rest.begin() + rest.find_first_not_of(" \r\n"), rest.end());
        }
    } else {
        msg.content = input;
    }
    return msg;
}

static common_chat_params common_chat_params_init_firefunction_v2(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    fprintf(stderr, "%s\n", __func__);
    common_chat_params data;
    data.prompt = apply(tmpl, inputs.messages, /* tools= */ nullptr, inputs.add_generation_prompt, {
        {"datetime", "Jan 29 2025 13:00:00 GMT"},
        {"functions", json(inputs.tools.empty() ? "" : inputs.tools.dump(2))},
    });
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != "required";
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            auto schemas = json::array();
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                schemas.push_back({
                    {"type", "object"},
                    {"properties", {
                        {"name", {
                            {"type", "string"},
                            {"const", function.at("name")},
                        }},
                        {"arguments", function.at("parameters")},
                    }},
                    {"required", json::array({"name", "arguments", "id"})},
                });
            });
            auto schema = json {
                {"type", "array"},
                {"items", schemas.size() == 1 ? schemas[0] : json {{"anyOf", schemas}}},
                {"minItems", 1},
            };
            if (!inputs.parallel_tool_calls) {
                schema["maxItems"] = 1;
            }
            builder.add_rule("root", "\" functools\"? " + builder.add_schema("tool_calls", schema));
        }, grammar_options);
        data.grammar_triggers.push_back({" functools[", /* .at_start = */ false});
        data.format = COMMON_CHAT_FORMAT_FIREFUNCTION_V2;
    } else {
        data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    }
    return data;
}
static common_chat_msg common_chat_parse_firefunction_v2(const std::string & input) {
    return parse_prefixed_json_tool_call_array(input, " functools[", /* rstrip_prefix= */ 1);
}

static common_chat_params common_chat_params_init_functionary_v3_2(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    // >>>all\nlet's call functions>>>fn1\n{"arg1": 1...}\n>>>fn2\n{"arg1": 1...}...
    // Using ">>>f1\n", ">>>f2\n"... as trigger words for the grammar
    common_chat_params data;
    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2;
    if (inputs.tools.is_array() && !inputs.tools.empty()) {
        data.grammar_lazy = inputs.tool_choice != "required";
        data.grammar = build_grammar([&](const common_grammar_builder & builder) {
            std::vector<std::string> first_tool_rules;
            std::vector<std::string> subsequent_tool_rules;
            foreach_function(inputs.tools, [&](const json & tool) {
                const auto & function = tool.at("function");
                std::string name = function.at("name");
                auto parameters = function.at("parameters");
                auto args_rule = builder.add_schema(name + "-args", parameters);
                first_tool_rules.push_back(builder.add_rule(name + "-call", "\"" + name + "\\n\" " + args_rule));
                subsequent_tool_rules.push_back(builder.add_rule(name + "-call2", "\">>>" + name + "\\n\" " + args_rule));
                data.grammar_triggers.push_back({name, /* .at_start = */ true});
                data.grammar_triggers.push_back({">>>" + name, /* .at_start = */ false});
            });
            auto first_rule = first_tool_rules.empty() ? "" : builder.add_rule("first_tool_call", string_join(first_tool_rules, " | ")) + " space";
            if (inputs.parallel_tool_calls) {
                auto subsequent_rule = builder.add_rule("subsequent_tool_call", string_join(subsequent_tool_rules, " | ")) + " space";
                builder.add_rule("root", first_rule + " (" + subsequent_rule + ")*");
            } else {
                builder.add_rule("root", first_rule);
            }

        }, grammar_options);
    }
    return data;
}

static bool consume(std::string::const_iterator & it, const std::string::const_iterator & end, const std::string & expected) {
    auto expected_it = expected.begin();
    auto tmp_it = it;
    while (tmp_it != end && expected_it != expected.end() && *tmp_it == *expected_it) {
        ++tmp_it;
        ++expected_it;
    }
    if (expected_it == expected.end()) {
        it = tmp_it;
        return true;
    }
    return false;
}

static common_chat_msg common_chat_parse_functionary_v3_2(const std::string & input) {
    static std::regex function_regex(R"((?:>>>)?(\w+)\n)");
    static std::regex close_regex(R"($|(?=>>>))");

    std::string content;
    auto it = input.begin();
    const auto end = input.end();

    if (consume(it, end, "all\n")) {
        std::smatch match;
        if (std::regex_search(it, end, match, function_regex)) {
            auto fun_it = match.prefix().second;
            content = std::string(it, fun_it);
            it = fun_it;
        } else {
            common_chat_msg res;
            res.role = "assistant";
            res.content = std::string(it, end);
            return res;
        }
    }
    // TODO: tighten & simplify.
    try {
        auto res = parse_json_tool_calls(std::string(it, end), std::nullopt, function_regex, close_regex);
        res.content = content + res.content;
        return res;
    } catch (const std::exception & e) {
        LOG_ERR("Failed to parse functionary v3.2 input: %s\n", e.what());
        common_chat_msg res;
        res.role = "assistant";
        res.content = input;
        return res;
    }
}

static common_chat_params common_chat_params_init_functionary_v3_1_llama_3_1(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    // https://github.com/MeetKai/functionary/blob/main/tests/prompt_test_v3-llama3.1.txt
    common_chat_params data;
    json tools = inputs.tools.is_null() ? inputs.tools : json::array();
    std::string python_code_argument_name;
    auto has_raw_python = false;

    data.grammar_lazy = inputs.tool_choice != "required";
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        std::vector<std::string> tool_rules;
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            const auto & parameters = function.at("parameters");
            std::string name = function.at("name");
            if (name == "python" || name == "ipython") {
                if (!parameters.contains("type")) {
                    throw std::runtime_error("Missing type in python tool");
                }
                has_raw_python = true;
                auto type = parameters.at("type");
                if (type == "object") {
                    auto properties = parameters.at("properties");
                    for (auto it = properties.begin(); it != properties.end(); ++it) {
                        if (it.value().at("type") == "string") {
                            if (!python_code_argument_name.empty()) {
                                throw std::runtime_error("Multiple string arguments found in python tool");
                            }
                            python_code_argument_name = it.key();
                        }
                    }
                    if (python_code_argument_name.empty()) {
                        throw std::runtime_error("No string argument found in python tool");
                    }
                } else if (type != "string") {
                    throw std::runtime_error("Invalid type in python tool: " + type.dump());
                }
            }
            tool_rules.push_back(builder.add_rule(name + "-call", "\"<function=" + name + ">\" " + builder.add_schema(name + "-args", parameters) + " \"</function>\" space"));
        });
        if (has_raw_python) {
            tool_rules.push_back(builder.add_rule("python-call", "\"<|python_tag|>\" .*"));
            data.grammar_triggers.push_back({"<|python_tag|>", /* .at_start = */ false});
        }
        auto tool_call = builder.add_rule("tool_call", string_join(tool_rules, " | ")) + " space";
        builder.add_rule("root", inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call);
        data.grammar_triggers.push_back({"<function=", /* .at_start = */ false});
    }, grammar_options);

    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    // TODO: if (has_raw_python)
    data.format = COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1;
    return data;
}
static common_chat_msg common_chat_parse_functionary_v3_1_llama_3_1(const std::string & input) {
    // This version of Functionary still supports the llama 3.1 tool call format for the python tool.
    static std::regex python_tag_regex(R"(<\|python_tag\|>([\s\S\n]*)$)");
    std::smatch match;
    if (std::regex_search(input, match, python_tag_regex)) {
        auto code = match[1].str();
        return {
            /* .role = */ "assistant",
            /* .content = */ match.prefix().str(),
            /* .tool_calls = */ {
                {
                    /* .name = */ "python",
                    /* .arguments = */ (json {{"code", code}}).dump(),
                    /* .id = */ "",
                },
            }
        };
    }
    static std::regex function_regex(R"(<function=(\w+)>)");
    static std::regex close_regex(R"(</function>)");
    // TODO: tighten & simplify.
    return parse_json_tool_calls(input, std::nullopt, function_regex, close_regex);
}

static common_chat_params common_chat_params_init_hermes_2_pro(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    common_chat_params data;
    // (content)?(<tool_call>{"name": "foo", "arguments": {"a": 1}}</tool_call>)*
    data.grammar_lazy = inputs.tool_choice != "required";
    data.grammar = build_grammar([&](const common_grammar_builder & builder) {
        std::vector<std::string> tool_rules;
        foreach_function(inputs.tools, [&](const json & tool) {
            const auto & function = tool.at("function");
            std::string name = function.at("name");
            auto parameters = function.at("parameters");
            builder.resolve_refs(parameters);
            tool_rules.push_back(builder.add_schema(name + "-call", {
                {"type", "object"},
                {"properties", json {
                    {"name", json {{"const", name}}},
                    {"arguments", parameters},
                }},
                {"required", json::array({"name", "arguments"})},
            }));
        });
        auto tool_call = "\"<tool_call>\" space " + builder.add_rule("tool_call", string_join(tool_rules, " | ")) + " \"</tool_call>\" space";
        builder.add_rule("root", inputs.parallel_tool_calls ? "(" + tool_call + ")+" : tool_call);
        data.grammar_triggers.push_back({"<tool_call>", /* .at_start = */ false});
        data.preserved_tokens = { "</tool_call>" };
    }, grammar_options);

    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_HERMES_2_PRO;
    return data;
}
static common_chat_msg common_chat_parse_hermes_2_pro(const std::string & input) {
    try {
        std::regex start_pattern(R"([\n\s]*<tool_call>)");
        std::regex middle_pattern(R"([\n\s]*</tool_call>[\n\s]*<tool_call>)");
        std::regex end_pattern(R"([\n\s]*</tool_call>[\n\s]*$)");

        auto end = input.end();
        std::sregex_iterator rend;
        std::sregex_iterator rit(input.begin(), end, start_pattern);
        if (rit == rend) {
            return {
                /* .role = */ "assistant",
                /* .content = */ input,
                /* .tool_calls = */ {},
            };
        }

        common_chat_msg result;
        result.role = "assistant";
        result.content = rit->prefix();

        auto it = rit->suffix().first;
        while (it != end) {
            json call;
            if (!parse_json(it, end, call)) {
                throw std::runtime_error("Failed to parse json tool call");
            }
            const auto & arguments = call.at("arguments");
            result.tool_calls.push_back({
                call.at("name"),
                arguments.dump(),
                // arguments.is_string() ? arguments.get<std::string>() : arguments.dump(),
                /* id= */ "",
            });
            rit = {it, end, middle_pattern};
            if (rit != rend) {
                it = rit->suffix().first;
            } else {
                rit = {it, end, end_pattern};
                if (rit == rend) {
                    throw std::runtime_error("Malformed input, missing </tool_call>");
                }
                break;
            }
        }
        return result;
    } catch (const std::exception & e) {
        return {
            /* .role = */ "assistant",
            /* .content = */ input,
            /* .tool_calls = */ {},
        };
    }
}

static common_chat_params common_chat_params_init_without_tools(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    common_chat_params data;
    data.prompt = apply(tmpl, inputs.messages, inputs.tools.empty() ? json() : inputs.tools, inputs.add_generation_prompt);
    data.format = COMMON_CHAT_FORMAT_CONTENT_ONLY;
    data.grammar_lazy = false;
    if (!inputs.json_schema.is_null()) {
        if (!inputs.grammar.empty()) {
            throw std::runtime_error("Either \"json_schema\" or \"grammar\" can be specified, but not both");
        }
        data.grammar = json_schema_to_grammar(inputs.json_schema);
    } else {
        data.grammar = inputs.grammar;
    }
    return data;
}

common_chat_params common_chat_params_init(const common_chat_template & tmpl, const struct common_chat_inputs & inputs) {
    const auto & src = tmpl.source();
    const auto & caps = tmpl.original_caps();

    if (inputs.tools.is_array()) {
        if (inputs.tool_choice != "none" && !inputs.grammar.empty()) {
            throw std::runtime_error("Cannot specify grammar with tools");
        }
        if (caps.supports_tool_calls && !caps.supports_tools) {
            LOG_WRN("Template supports tool calls but does not natively describe tools. The fallback behaviour used may produce bad results, inspect prompt w/ --verbose & consider overriding the template.\n");
        }
    }

    // DeepSeek R1: use handler in all cases except json schema (thinking / tools).
    if (src.find("<｜tool▁calls▁begin｜>") != std::string::npos && inputs.json_schema.is_null()) {
        return common_chat_params_init_deepseek_r1(tmpl, inputs);
    }

    // Command R7B: : use handler in all cases except json schema (thinking / tools).
    if (src.find("<|END_THINKING|><|START_ACTION|>") != std::string::npos && inputs.json_schema.is_null()) {
        return common_chat_params_init_command_r7b(tmpl, inputs);
    }

    // Use generic handler when mixing tools + JSON schema.
    // TODO: support that mix in handlers below.
    if ((!inputs.tools.is_array() && inputs.json_schema.is_object())) {
        return common_chat_params_init_generic(tmpl, inputs);
    }

    // Functionary prepends "all\n" to plain content outputs, so we use its handler in all cases.
    if (src.find(">>>all") != std::string::npos) {
        return common_chat_params_init_functionary_v3_2(tmpl, inputs);
    }

    // Firefunction v2 requires datetime and functions in the context even w/o tools, so we also use its handler in all cases.
    if (src.find(" functools[") != std::string::npos) {
        return common_chat_params_init_firefunction_v2(tmpl, inputs);
    }

    // Plain handler (no tools)
    if (inputs.tools.is_null() || inputs.tool_choice == "none") {
        return common_chat_params_init_without_tools(tmpl, inputs);
    }

    // Hermes 2/3 Pro, Qwen 2.5 Instruct (w/ tools)
    if (src.find("<tool_call>") != std::string::npos) {
        return common_chat_params_init_hermes_2_pro(tmpl, inputs);
    }

    // Functionary v3.1 (w/ tools)
    if (src.find("<|start_header_id|>") != std::string::npos
        && src.find("<function=") != std::string::npos) {
        return common_chat_params_init_functionary_v3_1_llama_3_1(tmpl, inputs);
    }

    // Llama 3.1, 3.2, 3.3 (w/ tools)
    if (src.find("<|start_header_id|>ipython<|end_header_id|>") != std::string::npos) {
        auto allow_python_tag_builtin_tools = src.find("<|python_tag|>") != std::string::npos;
        return common_chat_params_init_llama_3_1_tool_calls(tmpl, inputs, allow_python_tag_builtin_tools);
    }

    // Mistral Nemo (w/ tools)
    if (src.find("[TOOL_CALLS]") != std::string::npos) {
        return common_chat_params_init_mistral_nemo(tmpl, inputs);
    }

    // Generic fallback
    return common_chat_params_init_generic(tmpl, inputs);
}

static common_chat_msg common_chat_parse_content_only(const std::string & input) {
    return {
        /* .role = */ "assistant",
        /* .content = */ input,
        /* .tool_calls = */ {},
    };
}

common_chat_msg common_chat_parse(const std::string & input, common_chat_format format) {
    switch (format) {
        case COMMON_CHAT_FORMAT_CONTENT_ONLY:
            return common_chat_parse_content_only(input);
        case COMMON_CHAT_FORMAT_GENERIC:
            return common_chat_parse_generic(input);
        case COMMON_CHAT_FORMAT_MISTRAL_NEMO:
            return common_chat_parse_mistral_nemo(input);
        case COMMON_CHAT_FORMAT_LLAMA_3_X:
            return common_chat_parse_llama_3_1(input);
        case COMMON_CHAT_FORMAT_LLAMA_3_X_WITH_BUILTIN_TOOLS:
            return common_chat_parse_llama_3_1(input, /* with_builtin_tools= */ true);
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1:
            return common_chat_parse_deepseek_r1(input, /* extract_reasoning= */ false);
        case COMMON_CHAT_FORMAT_DEEPSEEK_R1_EXTRACT_REASONING:
            return common_chat_parse_deepseek_r1(input, /* extract_reasoning= */ true);
        case COMMON_CHAT_FORMAT_FUNCTIONARY_V3_2:
            return common_chat_parse_functionary_v3_2(input);
        case COMMON_CHAT_FORMAT_FUNCTIONARY_V3_1_LLAMA_3_1:
            return common_chat_parse_functionary_v3_1_llama_3_1(input);
        case COMMON_CHAT_FORMAT_HERMES_2_PRO:
            return common_chat_parse_hermes_2_pro(input);
        case COMMON_CHAT_FORMAT_FIREFUNCTION_V2:
            return common_chat_parse_firefunction_v2(input);
        case COMMON_CHAT_FORMAT_COMMAND_R7B:
            return common_chat_parse_command_r7b(input, /* extract_reasoning= */ false);
        case COMMON_CHAT_FORMAT_COMMAND_R7B_EXTRACT_REASONING:
            return common_chat_parse_command_r7b(input, /* extract_reasoning= */ true);
        default:
            throw std::runtime_error("Unsupported format: " + common_chat_format_name(format));
    }
}
