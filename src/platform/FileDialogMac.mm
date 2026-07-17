#include "platform/FileDialog.h"

#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>

#include <string>

namespace Iridium {
    namespace {
        GLFWwindow* dialogOwner = nullptr;

        NSArray<NSString*>* allowedTypes(std::span<const FileDialogFilter> filters) {
            NSMutableArray<NSString*>* types = [NSMutableArray array];
            for (const FileDialogFilter& filter : filters) {
                std::string patterns(filter.pattern);
                size_t start = 0;
                while (start <= patterns.size()) {
                    const size_t end = patterns.find(';', start);
                    const std::string pattern = patterns.substr(start, end - start);
                    if (pattern.size() > 2 && pattern.starts_with("*.")) {
                        [types addObject:[NSString stringWithUTF8String:pattern.c_str() + 2]];
                    }
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
            return types.count == 0 ? nil : types;
        }

        void focusOwner() {
            if (dialogOwner) {
                [glfwGetCocoaWindow(dialogOwner) makeKeyWindow];
            }
        }
    }

    void setFileDialogOwner(GLFWwindow* window) noexcept {
        dialogOwner = window;
    }

    std::optional<std::filesystem::path> openFileDialog(
        std::span<const FileDialogFilter> filters,
        const std::filesystem::path& initialDirectory) {
        @autoreleasepool {
            focusOwner();
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = YES;
            panel.canChooseDirectories = NO;
            panel.allowsMultipleSelection = NO;
            panel.allowedFileTypes = allowedTypes(filters);
            if (!initialDirectory.empty()) {
                panel.directoryURL = [NSURL fileURLWithPath:
                    [NSString stringWithUTF8String:initialDirectory.string().c_str()]];
            }

            if ([panel runModal] != NSModalResponseOK || panel.URL == nil) return std::nullopt;
            return std::filesystem::path(panel.URL.fileSystemRepresentation);
        }
    }

    std::optional<std::filesystem::path> saveFileDialog(
        std::span<const FileDialogFilter> filters,
        const std::filesystem::path& suggestedPath,
        std::string_view defaultExtension) {
        @autoreleasepool {
            focusOwner();
            NSSavePanel* panel = [NSSavePanel savePanel];
            panel.allowedFileTypes = allowedTypes(filters);
            if (!suggestedPath.empty()) {
                const std::filesystem::path directory = suggestedPath.parent_path();
                if (!directory.empty()) {
                    panel.directoryURL = [NSURL fileURLWithPath:
                        [NSString stringWithUTF8String:directory.string().c_str()]];
                }
                panel.nameFieldStringValue =
                    [NSString stringWithUTF8String:suggestedPath.filename().string().c_str()];
            }
            else if (!defaultExtension.empty()) {
                panel.nameFieldStringValue = [NSString stringWithFormat:@"untitled.%s",
                    std::string(defaultExtension).c_str()];
            }

            if ([panel runModal] != NSModalResponseOK || panel.URL == nil) return std::nullopt;
            return std::filesystem::path(panel.URL.fileSystemRepresentation);
        }
    }

} // namespace Iridium
