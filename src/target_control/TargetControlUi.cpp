#include "TargetControlUi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#if !defined(__APPLE__)
#  include <backends/imgui_impl_opengl3.h>
#endif

#include "Theme.hpp"

namespace target_control {
namespace {

const char* target_type_name(radar::types::TargetType type) {
    switch (type) {
    case radar::types::TargetType::TARGET_FIGHTER: return "FIGHTER";
    case radar::types::TargetType::TARGET_BOMBER: return "BOMBER";
    case radar::types::TargetType::TARGET_MISSILE: return "MISSILE";
    case radar::types::TargetType::TARGET_SHIP: return "SHIP";
    case radar::types::TargetType::TARGET_DRONE: return "DRONE";
    case radar::types::TargetType::TARGET_DECOY: return "DECOY";
    default: return "UNKNOWN";
    }
}

std::string label_for(
        const radar::types::TargetControlSnapshot& snapshot,
        const std::string& template_name) {
    const auto descriptor = std::find_if(
        snapshot.catalog.begin(), snapshot.catalog.end(),
        [&](const auto& entry) { return entry.name == template_name; });
    return descriptor == snapshot.catalog.end()
        ? template_name : descriptor->label;
}

} // namespace

TargetControlUi::~TargetControlUi() {
    shutdown();
}

bool TargetControlUi::init() {
    if (!glfwInit()) {
        std::cerr << "GLFW init failed\n";
        return false;
    }

#if defined(__APPLE__)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#  if defined(_WIN32)
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#  endif
#endif

    int width = 1500;
    int height = 900;
#if defined(_WIN32)
    int work_x = 0;
    int work_y = 0;
    int work_width = 1920;
    int work_height = 1080;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        glfwGetMonitorWorkarea(
            monitor, &work_x, &work_y, &work_width, &work_height);
        width = std::min(width, std::max(900, work_width * 95 / 100));
        height = std::min(height, std::max(650, work_height * 90 / 100));
    }
#endif

    window_ = glfwCreateWindow(
        width, height, "Target Generator Control", nullptr, nullptr);
    if (!window_) {
        std::cerr << "GLFW window creation failed\n";
        return false;
    }
#if defined(_WIN32)
    glfwSetWindowPos(
        window_, work_x + (work_width - width) / 2,
        work_y + (work_height - height) / 2);
#endif
#if !defined(__APPLE__)
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    update_content_scale();

#if defined(__APPLE__)
    ImGui_ImplGlfw_InitForOther(window_, true);
    if (!metal_.init(window_)) {
        std::cerr << "Metal init failed\n";
        return false;
    }
#else
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
#endif
    return true;
}

void TargetControlUi::update_content_scale() {
    float sx = 1.0f;
    float sy = 1.0f;
    glfwGetWindowContentScale(window_, &sx, &sy);
    if (!(sx >= 1.0f && sx <= 4.0f) || !std::isfinite(sx))
        sx = 1.0f;
    if (std::fabs(sx - content_scale_) < 0.01f)
        return;
    content_scale_ = sx;
    ImGuiIO& io = ImGui::GetIO();
#if defined(__APPLE__)
    radar::ui::theme::configure_default_font(sx);
    io.FontGlobalScale = 1.0f;
    radar::ui::theme::apply_style(1.0f);
    if (!metal_.rebuild_font_texture())
        std::cerr << "Failed to rebuild Retina font texture\n";
#else
    io.FontGlobalScale = sx;
    radar::ui::theme::apply_style(sx);
#endif
}

void TargetControlUi::shutdown() {
    if (!window_)
        return;
#if defined(__APPLE__)
    metal_.shutdown();
#else
    ImGui_ImplOpenGL3_Shutdown();
#endif
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;
}

void TargetControlUi::render() {
    for (const auto& reply : client_.take_replies()) {
        const char* result =
            reply.result ==
                    radar::types::TargetControlResult::
                        TARGET_CONTROL_ACCEPTED
                ? "accepted" : "rejected";
        event_log_.push_front(
            "#" + std::to_string(reply.request_id) + " " + result +
            ": " + reply.message);
        if (event_log_.size() > 12)
            event_log_.pop_back();
    }

    const bool connected = client_.connected();
    const auto snapshot = client_.snapshot();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(
        "Target Generator Control", nullptr,
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

    ImGui::TextUnformatted("TARGET GENERATOR CONTROL");
    ImGui::SameLine();
    ImGui::TextColored(
        connected ? ImVec4(0.2f, 0.9f, 0.3f, 1.0f)
                  : ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
        connected ? "CONNECTED" : "WAITING FOR TARGET_GEN");
    ImGui::SameLine();
    ImGui::TextDisabled(
        "control domain %d", client_.domain_id());
    if (snapshot) {
        ImGui::SameLine();
        ImGui::TextDisabled(
            "revision %lld | %zu scenarios | %zu targets",
            static_cast<long long>(snapshot->revision),
            snapshot->scenarios.size(), snapshot->targets.size());
    }

    ImGui::SeparatorText("SCENARIO CATALOG");
    if (!snapshot) {
        ImGui::TextWrapped(
            "Waiting for the transient target inventory. Start target_gen "
            "with this control domain.");
    } else {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        // Four columns keep the seven-template catalog to two rows in the
        // standard 1500px window while still allowing descriptions to wrap.
        const std::size_t cards_per_row =
            std::min<std::size_t>(4, snapshot->catalog.size());
        const float card_width = std::max(
            210.0f,
            (ImGui::GetContentRegionAvail().x -
             gap * static_cast<float>(
                 cards_per_row > 0
                     ? cards_per_row - 1 : 0)) /
                std::max(1.0f,
                    static_cast<float>(cards_per_row)));
        for (std::size_t i = 0; i < snapshot->catalog.size(); ++i) {
            const auto& descriptor = snapshot->catalog[i];
            if (i % cards_per_row != 0)
                ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginChild(
                "scenario_card", ImVec2(card_width, 150.0f), true);
            ImGui::TextUnformatted(descriptor.label.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", descriptor.description.c_str());
            int32_t& count = target_counts_[descriptor.name];
            if (count == 0)
                count = descriptor.default_target_count;
            if (descriptor.configurable_target_count) {
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("Targets", &count);
                count = std::clamp(
                    count, descriptor.minimum_target_count,
                    descriptor.maximum_target_count);
            } else {
                ImGui::Dummy(ImVec2(1.0f, ImGui::GetFrameHeight()));
            }
            ImGui::BeginDisabled(!connected);
            if (ImGui::Button("ADD", ImVec2(-1.0f, 0.0f))) {
                client_.add_scenario(descriptor.name, count);
            }
            ImGui::EndDisabled();
            ImGui::EndChild();
            ImGui::PopID();
        }
    }

    const float lower_height =
        std::max(250.0f, ImGui::GetContentRegionAvail().y - 120.0f);
    const float left_width =
        ImGui::GetContentRegionAvail().x * 0.36f;

    ImGui::BeginChild(
        "active_scenarios", ImVec2(left_width, lower_height), true);
    ImGui::SeparatorText("ACTIVE SCENARIO INSTANCES");
    if (snapshot && ImGui::BeginTable(
            "scenario_table", 4,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Instance");
        ImGui::TableSetupColumn("Scenario");
        ImGui::TableSetupColumn("Targets");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (const auto& scenario : snapshot->scenarios) {
            ImGui::PushID(
                static_cast<int>(scenario.scenario_instance_id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text(
                "%lld",
                static_cast<long long>(
                    scenario.scenario_instance_id));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                label_for(*snapshot, scenario.template_name).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", scenario.target_count);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!connected);
            if (ImGui::SmallButton("REMOVE"))
                client_.remove_scenario(
                    scenario.scenario_instance_id);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("active_targets", ImVec2(0.0f, lower_height), true);
    ImGui::SeparatorText("ACTIVE TARGETS");
    if (snapshot && ImGui::BeginTable(
            "target_table", 9,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ID");
        ImGui::TableSetupColumn("Scenario");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Range km");
        ImGui::TableSetupColumn("East km");
        ImGui::TableSetupColumn("North km");
        ImGui::TableSetupColumn("Up km");
        ImGui::TableSetupColumn("Speed m/s");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (const auto& target : snapshot->targets) {
            ImGui::PushID(target.target_id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%d", target.target_id);
            ImGui::TableNextColumn();
            ImGui::Text(
                "%lld",
                static_cast<long long>(
                    target.scenario_instance_id));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(
                target_type_name(target.target_type));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", target.range_m / 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", target.position.x_east_m / 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", target.position.y_north_m / 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", target.position.z_up_m / 1000.0);
            ImGui::TableNextColumn();
            ImGui::Text(
                "%.0f", std::hypot(
                    target.velocity.x_east_m,
                    target.velocity.y_north_m));
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!connected);
            if (ImGui::SmallButton("REMOVE"))
                client_.remove_target(target.target_id);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::BeginChild("events", ImVec2(-190.0f, 0.0f), true);
    ImGui::SeparatorText("CONTROL EVENTS");
    for (const auto& event : event_log_)
        ImGui::TextUnformatted(event.c_str());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("clear", ImVec2(0.0f, 0.0f), true);
    ImGui::BeginDisabled(!connected || !snapshot ||
                         snapshot->targets.empty());
    ImGui::PushStyleColor(
        ImGuiCol_Button, ImVec4(0.55f, 0.10f, 0.08f, 1.0f));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.16f, 0.12f, 1.0f));
    if (ImGui::Button("CLEAR ALL", ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("Confirm clear");
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal(
            "Confirm clear", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "Dispose every active target and remove all scenario instances?");
        if (ImGui::Button("CLEAR ALL TARGETS")) {
            client_.clear_all();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("CANCEL"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    ImGui::End();
}

int TargetControlUi::run() {
    if (!init())
        return 1;

    while (!glfwWindowShouldClose(window_)) {
#if defined(__APPLE__)
        @autoreleasepool {
#endif
        glfwPollEvents();
        if (stop_requested_ && stop_requested_()) {
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
            continue;
        }
        update_content_scale();
#if defined(__APPLE__)
        if (!metal_.begin_frame()) {
            glfwWaitEventsTimeout(0.05);
            continue;
        }
        metal_.new_frame();
#else
        ImGui_ImplOpenGL3_NewFrame();
#endif
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        render();
        ImGui::Render();
#if defined(__APPLE__)
        metal_.render();
#else
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.04f, 0.04f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
#endif
#if defined(__APPLE__)
        }
#endif
    }
    return 0;
}

} // namespace target_control
