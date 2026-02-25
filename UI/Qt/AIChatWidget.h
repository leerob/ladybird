/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibRequests/Forward.h>
#include <LibURL/URL.h>

#include <QWidget>

class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace Ladybird {

class BrowserWindow;
class WebContentView;

class AIChatWidget final : public QWidget {
    Q_OBJECT

public:
    explicit AIChatWidget(BrowserWindow&, QWidget* parent = nullptr);
    virtual ~AIChatWidget() override;

private:
    struct PendingComputerCall {
        String call_id;
        JsonObject action;
    };

    void submit_prompt();
    void begin_user_turn(String prompt);
    void request_page_context_and_send_prompt(String prompt);
    void send_responses_request();
    void finish_turn();

    void append_user_message(StringView message);
    void append_assistant_message(StringView message);
    void append_status_message(StringView message);
    void set_input_enabled(bool enabled);

    String build_user_prompt_with_context(StringView prompt, URL::URL const& url, StringView title, StringView page_text) const;
    JsonArray build_tools_payload() const;
    String extract_assistant_text(JsonObject const&) const;
    Vector<PendingComputerCall> extract_computer_calls(JsonObject const&) const;

    ErrorOr<void> handle_responses_payload(StringView payload);
    void handle_responses_json(JsonObject const&);

    ErrorOr<JsonObject> build_computer_call_output(PendingComputerCall const&);
    ErrorOr<String> execute_computer_action_and_capture(PendingComputerCall const&);

    void execute_click(double x, double y, Qt::MouseButton button, bool double_click);
    void execute_scroll(double x, double y, int delta_x, int delta_y);
    void execute_type_text(StringView text);
    void execute_keypress(JsonObject const& action);
    Optional<String> capture_view_as_data_url() const;

    WebContentView* current_view() const;
    QPoint clamp_to_view(double x, double y) const;
    static Qt::MouseButton mouse_button_from_string(StringView button);

    BrowserWindow& m_window;
    QPlainTextEdit* m_transcript { nullptr };
    QLineEdit* m_prompt_input { nullptr };
    QPushButton* m_send_button { nullptr };

    String m_openai_api_key;
    URL::URL m_responses_endpoint;
    JsonArray m_conversation_history;

    RefPtr<Requests::Request> m_active_request;
    bool m_turn_in_flight { false };
    size_t m_tool_loop_iteration { 0 };

    static constexpr size_t s_max_tool_iterations = 16;
};

}
