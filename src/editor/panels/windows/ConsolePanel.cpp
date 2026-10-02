#include "editor/panels/windows/ConsolePanel.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace {

    const char* severityName(
        Iridium::EngineLogSeverity severity) {
        switch (severity) {
        case Iridium::EngineLogSeverity::Info:
            return "Info";
        case Iridium::EngineLogSeverity::Warning:
            return "Warning";
        case Iridium::EngineLogSeverity::Error:
            return "Error";
        }
        return "Unknown";
    }

    ImVec4 severityColor(
        Iridium::EngineLogSeverity severity) {
        switch (severity) {
        case Iridium::EngineLogSeverity::Warning:
            return ImVec4(
                1.0f, 0.76f, 0.24f, 1.0f);
        case Iridium::EngineLogSeverity::Error:
            return ImVec4(
                1.0f, 0.34f, 0.30f, 1.0f);
        case Iridium::EngineLogSeverity::Info:
            return ImGui::GetStyleColorVec4(
                ImGuiCol_Text);
        }
        return ImGui::GetStyleColorVec4(
            ImGuiCol_Text);
    }

    std::string timestamp(
        const Iridium::EngineLogEntry& entry) {
        const std::time_t value =
            std::chrono::system_clock::to_time_t(
                entry.timestamp);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &value);
#else
        localtime_r(&value, &local);
#endif
        char text[16]{};
        std::snprintf(
            text, sizeof(text),
            "%02d:%02d:%02d",
            local.tm_hour,
            local.tm_min,
            local.tm_sec);
        return text;
    }

    bool containsInsensitive(
        std::string_view text,
        std::string_view query) {
        if (query.empty()) return true;
        const auto found = std::search(
            text.begin(), text.end(),
            query.begin(), query.end(),
            [](char lhs, char rhs) {
                return std::tolower(
                    static_cast<unsigned char>(lhs)) ==
                    std::tolower(
                        static_cast<unsigned char>(rhs));
            });
        return found != text.end();
    }

} // namespace

ConsolePanel::ConsolePanel(
    bool* isOpen,
    Iridium::EngineLog* log)
    : isOpen_(isOpen), log_(log) {}

void ConsolePanel::rebuildVisible() {
    visible_.clear();
    const std::string_view query(
        filter_.data());
    for (size_t index = 0;
        index < entries_.size();
        ++index) {
        const auto& entry =
            entries_[index];
        const bool severityVisible =
            (entry.severity ==
                    Iridium::EngineLogSeverity::Info &&
                showInfo_) ||
            (entry.severity ==
                    Iridium::EngineLogSeverity::Warning &&
                showWarnings_) ||
            (entry.severity ==
                    Iridium::EngineLogSeverity::Error &&
                showErrors_);
        if (!severityVisible) continue;
        if (!containsInsensitive(
                entry.category, query) &&
            !containsInsensitive(
                entry.message, query)) {
            continue;
        }
        visible_.push_back(index);
    }
    filterDirty_ = false;
}

void ConsolePanel::OnImGuiRender(
    Registry& registry,
    Iridium::AssetManager* assetManager) {
    (void)registry;
    (void)assetManager;
    if (!isOpen_ || !*isOpen_) return;
    ImGui::SetNextWindowSize(
        ImVec2(760.0f, 240.0f),
        ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(
            "Console", isOpen_)) {
        ImGui::End();
        return;
    }
    if (!log_) {
        ImGui::TextDisabled(
            "Engine log is unavailable.");
        ImGui::End();
        return;
    }

    const uint64_t revision =
        log_->revision();
    const bool receivedEntries =
        revision != cachedRevision_;
    if (receivedEntries) {
        entries_ = log_->snapshot();
        cachedRevision_ = revision;
        filterDirty_ = true;
    }

    if (ImGui::Button("Clear")) {
        log_->clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        std::string text;
        for (size_t index : visible_) {
            const auto& entry =
                entries_[index];
            text += '[' + timestamp(entry) +
                "] [" +
                severityName(entry.severity) +
                "] [" + entry.category +
                "] " + entry.message + '\n';
        }
        ImGui::SetClipboardText(
            text.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(
            "Info", &showInfo_)) {
        filterDirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(
            "Warnings", &showWarnings_)) {
        filterDirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(
            "Errors", &showErrors_)) {
        filterDirty_ = true;
    }
    ImGui::SameLine();
    ImGui::Checkbox(
        "Auto-scroll", &autoScroll_);

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint(
            "##console-filter",
            "Filter messages or categories",
            filter_.data(),
            filter_.size())) {
        filterDirty_ = true;
    }
    if (filterDirty_) {
        rebuildVisible();
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable(
            "console-entries", 4, flags,
            ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(
            0, 1);
        ImGui::TableSetupColumn(
            "Time",
            ImGuiTableColumnFlags_WidthFixed,
            72.0f);
        ImGui::TableSetupColumn(
            "Level",
            ImGuiTableColumnFlags_WidthFixed,
            68.0f);
        ImGui::TableSetupColumn(
            "Category",
            ImGuiTableColumnFlags_WidthFixed,
            130.0f);
        ImGui::TableSetupColumn(
            "Message",
            ImGuiTableColumnFlags_WidthStretch,
            520.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(
            static_cast<int>(
                visible_.size()));
        while (clipper.Step()) {
            for (int visibleIndex =
                    clipper.DisplayStart;
                visibleIndex <
                    clipper.DisplayEnd;
                ++visibleIndex) {
                const auto& entry =
                    entries_[visible_[
                        static_cast<size_t>(
                            visibleIndex)]];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const std::string time =
                    timestamp(entry);
                ImGui::TextUnformatted(
                    time.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(
                    severityColor(
                        entry.severity),
                    "%s",
                    severityName(
                        entry.severity));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(
                    entry.category.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(
                    entry.message.c_str());
                if (ImGui::IsItemHovered(
                        ImGuiHoveredFlags_DelayShort)) {
                    ImGui::SetTooltip(
                        "%s",
                        entry.message.c_str());
                }
            }
        }
        if (receivedEntries &&
            autoScroll_) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
