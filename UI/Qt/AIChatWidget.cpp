/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <AK/StdLibExtras.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebView/PageInfo.h>
#include <UI/Qt/AIChatWidget.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/BrowserWindow.h>
#include <UI/Qt/StringUtils.h>

#include <QApplication>
#include <QBuffer>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace Ladybird {

namespace {

static Optional<double> json_number(JsonObject const& object, StringView key)
{
    if (auto value = object.get_double_with_precision_loss(key); value.has_value())
        return *value;
    if (auto value = object.get_i64(key); value.has_value())
        return static_cast<double>(*value);
    if (auto value = object.get_u64(key); value.has_value())
        return static_cast<double>(*value);
    return {};
}

static Optional<int> json_integer(JsonObject const& object, StringView key)
{
    if (auto value = object.get_i64(key); value.has_value())
        return static_cast<int>(*value);
    if (auto value = object.get_u64(key); value.has_value())
        return static_cast<int>(*value);
    if (auto value = object.get_double_with_precision_loss(key); value.has_value())
        return static_cast<int>(round(*value));
    return {};
}

static Optional<Qt::Key> key_from_name(StringView key_name)
{
    auto upper = key_name.to_ascii_uppercase_string();
    auto upper_view = upper.bytes_as_string_view();

    if (upper_view.length() == 1) {
        auto code_point = upper_view[0];
        if ((code_point >= 'A' && code_point <= 'Z') || (code_point >= '0' && code_point <= '9'))
            return static_cast<Qt::Key>(code_point);
    }

    if (upper_view == "ENTER"sv || upper_view == "RETURN"sv)
        return Qt::Key_Return;
    if (upper_view == "TAB"sv)
        return Qt::Key_Tab;
    if (upper_view == "ESC"sv || upper_view == "ESCAPE"sv)
        return Qt::Key_Escape;
    if (upper_view == "SPACE"sv)
        return Qt::Key_Space;
    if (upper_view == "BACKSPACE"sv)
        return Qt::Key_Backspace;
    if (upper_view == "DELETE"sv || upper_view == "DEL"sv)
        return Qt::Key_Delete;
    if (upper_view == "UP"sv)
        return Qt::Key_Up;
    if (upper_view == "DOWN"sv)
        return Qt::Key_Down;
    if (upper_view == "LEFT"sv)
        return Qt::Key_Left;
    if (upper_view == "RIGHT"sv)
        return Qt::Key_Right;
    if (upper_view == "HOME"sv)
        return Qt::Key_Home;
    if (upper_view == "END"sv)
        return Qt::Key_End;
    if (upper_view == "PAGEUP"sv)
        return Qt::Key_PageUp;
    if (upper_view == "PAGEDOWN"sv)
        return Qt::Key_PageDown;
    return {};
}

static bool is_modifier_key(StringView key_name, Qt::KeyboardModifiers& modifiers)
{
    auto upper = key_name.to_ascii_uppercase_string();

    if (upper == "CTRL"sv || upper == "CONTROL"sv) {
        modifiers |= Qt::ControlModifier;
        return true;
    }
    if (upper == "ALT"sv || upper == "OPTION"sv) {
        modifiers |= Qt::AltModifier;
        return true;
    }
    if (upper == "SHIFT"sv) {
        modifiers |= Qt::ShiftModifier;
        return true;
    }
    if (upper == "META"sv || upper == "CMD"sv || upper == "SUPER"sv) {
        modifiers |= Qt::MetaModifier;
        return true;
    }
    return false;
}

static String truncate_for_context(StringView input, size_t max_length)
{
    if (input.length() <= max_length)
        return MUST(String::from_utf8(input));
    return MUST(String::formatted("{}\n\n[...truncated by Ladybird AI chat...]", input.substring_view(0, max_length)));
}

}

AIChatWidget::AIChatWidget(BrowserWindow& window, QWidget* parent)
    : QWidget(parent)
    , m_window(window)
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(8, 8, 8, 8);
    main_layout->setSpacing(8);

    m_transcript = new QPlainTextEdit(this);
    m_transcript->setReadOnly(true);
    m_transcript->setPlaceholderText("AI chat transcript");

    auto* input_row = new QHBoxLayout;
    input_row->setContentsMargins(0, 0, 0, 0);
    input_row->setSpacing(6);

    m_prompt_input = new QLineEdit(this);
    m_prompt_input->setPlaceholderText("Ask AI to explain or interact with this page...");

    m_send_button = new QPushButton("Send", this);

    input_row->addWidget(m_prompt_input);
    input_row->addWidget(m_send_button);

    main_layout->addWidget(m_transcript, 1);
    main_layout->addLayout(input_row);

    connect(m_send_button, &QPushButton::clicked, this, &AIChatWidget::submit_prompt);
    connect(m_prompt_input, &QLineEdit::returnPressed, this, &AIChatWidget::submit_prompt);

    if (auto key = Core::Environment::get("OPENAI_KEY"sv); key.has_value())
        m_openai_api_key = MUST(String::from_utf8(*key));

    auto endpoint = URL::Parser::basic_parse("https://api.openai.com/v1/responses"sv);
    VERIFY(endpoint.has_value());
    m_responses_endpoint = endpoint.release_value();

    if (m_openai_api_key.is_empty())
        append_status_message("OPENAI_KEY is not set; AI chat cannot send requests."sv);
    else
        append_status_message("AI chat ready. Each prompt includes current page URL, title, and text context."sv);
}

AIChatWidget::~AIChatWidget() = default;

void AIChatWidget::submit_prompt()
{
    if (m_turn_in_flight)
        return;

    auto prompt = ak_string_from_qstring(m_prompt_input->text().trimmed());
    if (prompt.is_empty())
        return;

    m_prompt_input->clear();
    append_user_message(prompt);
    begin_user_turn(AK::move(prompt));
}

void AIChatWidget::begin_user_turn(String prompt)
{
    if (m_openai_api_key.is_empty()) {
        append_status_message("Set OPENAI_KEY in the environment, then restart Ladybird."sv);
        return;
    }

    m_turn_in_flight = true;
    m_tool_loop_iteration = 0;
    set_input_enabled(false);
    request_page_context_and_send_prompt(AK::move(prompt));
}

void AIChatWidget::request_page_context_and_send_prompt(String prompt)
{
    auto* view = current_view();
    if (!view) {
        append_status_message("No active tab is available."sv);
        finish_turn();
        return;
    }

    auto url = view->url();
    auto title = view->title().to_well_formed_utf8();

    view->request_internal_page_info(WebView::PageInfoType::Text)
        ->when_resolved([this, prompt = AK::move(prompt), url = AK::move(url), title = AK::move(title)](String const& page_text) {
            JsonObject message;
            message.set("role"sv, "user"sv);
            message.set("content"sv, build_user_prompt_with_context(prompt, url, title, page_text));

            JsonArray input;
            input.must_append(AK::move(message));
            send_responses_request(AK::move(input), m_last_response_id);
        })
        .when_rejected([this](Error const& error) {
            append_status_message(MUST(String::formatted("Failed to read page context: {}", error)));
            finish_turn();
        });
}

void AIChatWidget::send_responses_request(JsonArray input, Optional<String> previous_response_id)
{
    auto headers = HTTP::HeaderList::create();
    headers->set(HTTP::Header { "Authorization"sv, ByteString::formatted("Bearer {}", m_openai_api_key) });
    headers->set(HTTP::Header { "Content-Type"sv, "application/json"sv });

    JsonObject payload;
    payload.set("model"sv, "gpt-4o"sv);
    payload.set("tools"sv, build_tools_payload());
    payload.set("truncation"sv, "auto"sv);
    payload.set("input"sv, AK::move(input));
    payload.set("instructions"sv, "You are an AI assistant in Ladybird. Use computer tool calls to interact with the active webpage when necessary, and explain briefly what you are doing."sv);
    if (previous_response_id.has_value())
        payload.set("previous_response_id"sv, *previous_response_id);

    auto payload_body = payload.serialized();
    auto request = WebView::Application::request_server_client().start_request("POST"sv, m_responses_endpoint, *headers, payload_body.bytes());
    if (!request) {
        append_status_message("Unable to start OpenAI request."sv);
        finish_turn();
        return;
    }

    m_active_request = request.release_nonnull();
    m_active_request->set_buffered_request_finished_callback(
        [this](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const&, Optional<u32> response_code, Optional<String> const& reason_phrase, ReadonlyBytes payload_bytes) {
            Core::deferred_invoke([this]() { m_active_request.clear(); });

            if (network_error.has_value()) {
                append_status_message(MUST(String::formatted("OpenAI request failed: {}", Requests::network_error_to_string(*network_error))));
                finish_turn();
                return;
            }

            auto payload_or_error = String::from_utf8(payload_bytes);
            if (payload_or_error.is_error()) {
                append_status_message("OpenAI returned a non-UTF8 payload."sv);
                finish_turn();
                return;
            }
            auto payload = payload_or_error.release_value();

            if (response_code.has_value() && *response_code >= 400) {
                String payload_suffix;
                if (!payload.is_empty())
                    payload_suffix = MUST(String::formatted("\n{}", payload));

                append_status_message(MUST(String::formatted("OpenAI returned HTTP {}: {}{}", *response_code, reason_phrase.value_or(""_string), payload_suffix)));
                finish_turn();
                return;
            }

            if (auto result = handle_responses_payload(payload); result.is_error()) {
                append_status_message(MUST(String::formatted("Could not parse OpenAI response: {}", result.error())));
                finish_turn();
            }
        });
}

void AIChatWidget::finish_turn()
{
    m_turn_in_flight = false;
    m_tool_loop_iteration = 0;
    set_input_enabled(true);
    m_prompt_input->setFocus();
}

void AIChatWidget::append_user_message(StringView message)
{
    m_transcript->appendPlainText(QString("You: %1").arg(qstring_from_ak_string(message)));
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
}

void AIChatWidget::append_assistant_message(StringView message)
{
    m_transcript->appendPlainText(QString("AI: %1").arg(qstring_from_ak_string(message)));
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
}

void AIChatWidget::append_status_message(StringView message)
{
    m_transcript->appendPlainText(QString("[System] %1").arg(qstring_from_ak_string(message)));
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
}

void AIChatWidget::set_input_enabled(bool enabled)
{
    m_prompt_input->setEnabled(enabled);
    m_send_button->setEnabled(enabled);
}

String AIChatWidget::build_user_prompt_with_context(StringView prompt, URL::URL const& url, StringView title, StringView page_text) const
{
    auto const clipped_page_text = truncate_for_context(page_text, 12 * 1024);
    return MUST(String::formatted(
        "User request:\n{}\n\nCurrent page context:\n- URL: {}\n- Title: {}\n- Extracted page text:\n{}\n\nUse the computer tool when actions are required.",
        prompt, url.serialize(), title, clipped_page_text));
}

JsonArray AIChatWidget::build_tools_payload() const
{
    JsonObject tool;
    tool.set("type"sv, "computer_use_preview"sv);
    tool.set("environment"sv, "browser"sv);

    auto* view = current_view();
    tool.set("display_width"sv, view ? std::max(1, view->width()) : 1024);
    tool.set("display_height"sv, view ? std::max(1, view->height()) : 768);

    JsonArray tools;
    tools.must_append(AK::move(tool));
    return tools;
}

String AIChatWidget::extract_assistant_text(JsonObject const& response) const
{
    if (auto output_text = response.get_string("output_text"sv); output_text.has_value() && !output_text->is_empty())
        return *output_text;

    StringBuilder builder;

    auto output = response.get_array("output"sv);
    if (!output.has_value())
        return {};

    output->for_each([&](JsonValue const& item) {
        if (!item.is_object())
            return;

        auto const& object = item.as_object();
        auto type = object.get_string("type"sv);
        if (!type.has_value() || *type != "message"sv)
            return;

        auto content = object.get_array("content"sv);
        if (!content.has_value())
            return;

        content->for_each([&](JsonValue const& content_item) {
            if (!content_item.is_object())
                return;

            auto const& content_object = content_item.as_object();
            auto content_type = content_object.get_string("type"sv);
            if (!content_type.has_value() || *content_type != "output_text"sv)
                return;

            auto text = content_object.get_string("text"sv);
            if (!text.has_value() || text->is_empty())
                return;

            if (!builder.is_empty())
                builder.append("\n\n"sv);
            builder.append(*text);
        });
    });

    return MUST(builder.to_string());
}

Vector<AIChatWidget::PendingComputerCall> AIChatWidget::extract_computer_calls(JsonObject const& response) const
{
    Vector<PendingComputerCall> calls;

    auto output = response.get_array("output"sv);
    if (!output.has_value())
        return calls;

    output->for_each([&](JsonValue const& item) {
        if (!item.is_object())
            return;

        auto const& object = item.as_object();
        auto type = object.get_string("type"sv);
        if (!type.has_value() || *type != "computer_call"sv)
            return;

        auto call_id = object.get_string("call_id"sv);
        auto action = object.get_object("action"sv);
        if (!call_id.has_value() || !action.has_value())
            return;

        Optional<JsonArray> pending_safety_checks;
        if (auto checks = object.get_array("pending_safety_checks"sv); checks.has_value())
            pending_safety_checks = JsonArray { *checks };

        PendingComputerCall call;
        call.call_id = *call_id;
        call.action = JsonObject { *action };
        call.pending_safety_checks = AK::move(pending_safety_checks);
        calls.append(AK::move(call));
    });

    return calls;
}

ErrorOr<void> AIChatWidget::handle_responses_payload(StringView payload)
{
    auto parsed_response = TRY(JsonValue::from_string(payload));
    if (!parsed_response.is_object())
        return Error::from_string_literal("expected JSON object");

    handle_responses_json(parsed_response.as_object());
    return {};
}

void AIChatWidget::handle_responses_json(JsonObject const& response)
{
    auto response_id = response.get_string("id"sv);
    if (response_id.has_value())
        m_last_response_id = *response_id;

    auto text = extract_assistant_text(response);
    if (!text.is_empty())
        append_assistant_message(text);

    auto calls = extract_computer_calls(response);
    if (calls.is_empty()) {
        finish_turn();
        return;
    }

    ++m_tool_loop_iteration;
    if (m_tool_loop_iteration > s_max_tool_iterations) {
        append_status_message("Stopped after too many computer-use iterations."sv);
        finish_turn();
        return;
    }

    JsonArray outputs;
    for (auto const& call : calls) {
        auto output = build_computer_call_output(call);
        if (output.is_error()) {
            append_status_message(MUST(String::formatted("Failed to execute computer action: {}", output.error())));
            finish_turn();
            return;
        }
        outputs.must_append(output.release_value());
    }

    if (!response_id.has_value()) {
        append_status_message("OpenAI response is missing an id for follow-up tool output."sv);
        finish_turn();
        return;
    }

    send_responses_request(AK::move(outputs), *response_id);
}

ErrorOr<JsonObject> AIChatWidget::build_computer_call_output(PendingComputerCall const& call)
{
    auto screenshot_data_url = TRY(execute_computer_action_and_capture(call));

    JsonObject output_item;
    output_item.set("type"sv, "computer_call_output"sv);
    output_item.set("call_id"sv, call.call_id);

    JsonObject output;
    output.set("type"sv, "input_image"sv);
    output.set("image_url"sv, screenshot_data_url);
    output_item.set("output"sv, AK::move(output));

    if (call.pending_safety_checks.has_value() && !call.pending_safety_checks->is_empty())
        output_item.set("acknowledged_safety_checks"sv, *call.pending_safety_checks);

    return output_item;
}

ErrorOr<String> AIChatWidget::execute_computer_action_and_capture(PendingComputerCall const& call)
{
    auto action_type = call.action.get_string("type"sv);
    if (!action_type.has_value())
        return Error::from_string_literal("computer action missing type");

    if (*action_type == "click"sv || *action_type == "double_click"sv) {
        auto x = json_number(call.action, "x"sv);
        auto y = json_number(call.action, "y"sv);
        if (!x.has_value() || !y.has_value())
            return Error::from_string_literal("click action missing coordinates");

        StringView button_name = "left"sv;
        if (auto button = call.action.get_string("button"sv); button.has_value())
            button_name = button->bytes_as_string_view();
        auto button = mouse_button_from_string(button_name);
        execute_click(*x, *y, button, *action_type == "double_click"sv);
    } else if (*action_type == "scroll"sv) {
        auto x = json_number(call.action, "x"sv).value_or(static_cast<double>(width() / 2));
        auto y = json_number(call.action, "y"sv).value_or(static_cast<double>(height() / 2));
        auto delta_x = json_integer(call.action, "scroll_x"sv)
                           .value_or(json_integer(call.action, "delta_x"sv).value_or(0));
        auto delta_y = json_integer(call.action, "scroll_y"sv)
                           .value_or(json_integer(call.action, "delta_y"sv).value_or(0));
        execute_scroll(x, y, delta_x, delta_y);
    } else if (*action_type == "type"sv) {
        auto text = call.action.get_string("text"sv);
        if (!text.has_value())
            return Error::from_string_literal("type action missing text");
        execute_type_text(*text);
    } else if (*action_type == "keypress"sv) {
        execute_keypress(call.action);
    } else if (*action_type == "wait"sv) {
        auto milliseconds = std::clamp(json_integer(call.action, "milliseconds"sv).value_or(500), 50, 5000);
        QEventLoop wait_loop;
        QTimer::singleShot(milliseconds, &wait_loop, &QEventLoop::quit);
        wait_loop.exec();
    } else if (*action_type == "screenshot"sv) {
        // No-op; we capture a screenshot below.
    } else {
        append_status_message(MUST(String::formatted("Unsupported computer action '{}', returning fresh screenshot.", *action_type)));
    }

    auto screenshot_data_url = capture_view_as_data_url();
    if (!screenshot_data_url.has_value())
        return Error::from_string_literal("failed to capture screenshot");
    return screenshot_data_url.release_value();
}

void AIChatWidget::execute_click(double x, double y, Qt::MouseButton button, bool double_click)
{
    auto* view = current_view();
    if (!view)
        return;

    auto local = clamp_to_view(x, y);
    auto local_position = QPointF(local);
    auto global_position = QPointF(view->mapToGlobal(local));

    QMouseEvent move_event(QEvent::MouseMove, local_position, global_position, Qt::NoButton, button, Qt::NoModifier);
    QApplication::sendEvent(view, &move_event);

    QMouseEvent press_event(QEvent::MouseButtonPress, local_position, global_position, button, button, Qt::NoModifier);
    QApplication::sendEvent(view, &press_event);

    if (double_click) {
        QMouseEvent double_click_event(QEvent::MouseButtonDblClick, local_position, global_position, button, button, Qt::NoModifier);
        QApplication::sendEvent(view, &double_click_event);
    }

    QMouseEvent release_event(QEvent::MouseButtonRelease, local_position, global_position, button, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view, &release_event);
}

void AIChatWidget::execute_scroll(double x, double y, int delta_x, int delta_y)
{
    auto* view = current_view();
    if (!view)
        return;

    auto local = clamp_to_view(x, y);
    auto local_position = QPointF(local);
    auto global_position = QPointF(view->mapToGlobal(local));

    QWheelEvent wheel_event(local_position, global_position, QPoint(delta_x, delta_y), QPoint(delta_x, delta_y), Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(view, &wheel_event);
}

void AIChatWidget::execute_type_text(StringView text)
{
    auto* view = current_view();
    if (!view)
        return;

    auto qt_text = qstring_from_ak_string(text);
    for (auto const& character : qt_text) {
        int key = 0;

        if (character == '\n' || character == '\r') {
            key = Qt::Key_Return;
        } else if (character == '\t') {
            key = Qt::Key_Tab;
        } else if (character == '\b') {
            key = Qt::Key_Backspace;
        } else if (character.isLetterOrNumber()) {
            key = character.toUpper().unicode();
        }

        QString typed_character(character);
        QKeyEvent key_press_event(QEvent::KeyPress, key, Qt::NoModifier, typed_character);
        QApplication::sendEvent(view, &key_press_event);

        QKeyEvent key_release_event(QEvent::KeyRelease, key, Qt::NoModifier, typed_character);
        QApplication::sendEvent(view, &key_release_event);
    }
}

void AIChatWidget::execute_keypress(JsonObject const& action)
{
    auto* view = current_view();
    if (!view)
        return;

    Vector<String> keys;
    if (auto key_array = action.get_array("keys"sv); key_array.has_value()) {
        key_array->for_each([&](JsonValue const& key) {
            if (key.is_string())
                keys.append(key.as_string());
        });
    }

    if (keys.is_empty()) {
        if (auto key = action.get_string("key"sv); key.has_value())
            keys.append(*key);
    }

    if (keys.is_empty())
        return;

    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    Optional<Qt::Key> main_key;

    for (auto const& key_name : keys) {
        if (is_modifier_key(key_name, modifiers))
            continue;

        if (auto key = key_from_name(key_name); key.has_value())
            main_key = *key;
    }

    if (!main_key.has_value())
        return;

    QKeyEvent key_press_event(QEvent::KeyPress, static_cast<int>(*main_key), modifiers);
    QApplication::sendEvent(view, &key_press_event);

    QKeyEvent key_release_event(QEvent::KeyRelease, static_cast<int>(*main_key), modifiers);
    QApplication::sendEvent(view, &key_release_event);
}

Optional<String> AIChatWidget::capture_view_as_data_url() const
{
    auto* view = current_view();
    if (!view)
        return {};

    auto pixmap = view->grab();
    if (pixmap.isNull())
        return {};

    QByteArray image_data;
    QBuffer buffer(&image_data);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    if (!pixmap.save(&buffer, "PNG"))
        return {};

    auto encoded = image_data.toBase64();
    return MUST(String::formatted("data:image/png;base64,{}", ak_byte_string_from_qbytearray(encoded)));
}

WebContentView* AIChatWidget::current_view() const
{
    auto* tab = m_window.current_tab();
    if (!tab)
        return nullptr;
    return &tab->view();
}

QPoint AIChatWidget::clamp_to_view(double x, double y) const
{
    auto* view = current_view();
    if (!view)
        return {};

    auto clamped_x = std::clamp(static_cast<int>(lround(x)), 0, std::max(0, view->width() - 1));
    auto clamped_y = std::clamp(static_cast<int>(lround(y)), 0, std::max(0, view->height() - 1));
    return { clamped_x, clamped_y };
}

Qt::MouseButton AIChatWidget::mouse_button_from_string(StringView button)
{
    auto lower = button.to_ascii_lowercase_string();
    if (lower == "right"sv)
        return Qt::RightButton;
    if (lower == "middle"sv)
        return Qt::MiddleButton;
    return Qt::LeftButton;
}

}
