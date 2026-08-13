#pragma once

#include "assets/AssetGuid.h"
#include "assets/runtime/AssetRuntimePublisher.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    enum class EditorAssetViewerKind : uint8_t {
        Model,
        Material,
    };

    enum class EditorAssetPresentationSource : uint8_t {
        Asset,
        ParentAsset,
    };

    struct EditorAssetViewerRegistration {
        std::string assetType;
        std::string viewerId;
        EditorAssetViewerKind kind = EditorAssetViewerKind::Model;
        EditorAssetPresentationSource presentationSource =
            EditorAssetPresentationSource::Asset;

        auto operator<=>(const EditorAssetViewerRegistration&) const = default;
    };

    class EditorAssetViewerRegistry {
    public:
        void registerViewer(EditorAssetViewerRegistration registration);
        void freeze() noexcept { frozen_ = true; }

        [[nodiscard]] bool frozen() const noexcept { return frozen_; }
        [[nodiscard]] const EditorAssetViewerRegistration* find(
            std::string_view assetType) const noexcept;
        [[nodiscard]] std::vector<EditorAssetViewerRegistration>
            registrations() const;

    private:
        std::map<std::string, EditorAssetViewerRegistration,
            std::less<>> registrations_;
        bool frozen_ = false;
    };

    void registerCoreAssetViewers(EditorAssetViewerRegistry& registry);

    struct EditorAssetOpenRequest {
        AssetGuid assetGuid;
        std::optional<AssetGuid> parentAssetGuid;
        std::string assetType;
        std::string displayName;
    };

    struct EditorAssetDocument {
        AssetGuid assetGuid;
        AssetGuid presentationAssetGuid;
        std::string assetType;
        std::string viewerId;
        std::string displayName;
        EditorAssetViewerKind kind = EditorAssetViewerKind::Model;
        std::optional<RuntimeAssetState> runtimeState;
        std::string runtimeDiagnostic;
        uint64_t activationSerial = 0;

        auto operator<=>(const EditorAssetDocument&) const = default;
    };

    struct EditorAssetOpenResult {
        bool succeeded = false;
        bool reused = false;
        std::string diagnostic;

        [[nodiscard]] explicit operator bool() const noexcept {
            return succeeded;
        }
    };

    class EditorAssetDocumentService {
    public:
        using RuntimePinCallback =
            std::function<void(AssetGuid, bool)>;

        explicit EditorAssetDocumentService(
            const EditorAssetViewerRegistry* registry = nullptr);
        ~EditorAssetDocumentService();

        EditorAssetDocumentService(const EditorAssetDocumentService&) = delete;
        EditorAssetDocumentService& operator=(
            const EditorAssetDocumentService&) = delete;

        void setRegistry(const EditorAssetViewerRegistry* registry) noexcept;
        void setRuntimePinCallback(RuntimePinCallback callback);

        [[nodiscard]] EditorAssetOpenResult open(
            const EditorAssetOpenRequest& request);
        [[nodiscard]] bool activate(AssetGuid assetGuid) noexcept;
        [[nodiscard]] bool close(AssetGuid assetGuid);
        void closeAll();

        void updateRuntimeState(
            AssetGuid presentationAssetGuid,
            std::optional<RuntimeAssetState> state,
            std::string_view diagnostic = {});

        [[nodiscard]] const EditorAssetDocument* active() const noexcept;
        [[nodiscard]] const EditorAssetDocument* find(
            AssetGuid assetGuid) const noexcept;
        [[nodiscard]] std::span<const EditorAssetDocument>
            documents() const noexcept { return documents_; }
        [[nodiscard]] size_t pinReferenceCount(
            AssetGuid presentationAssetGuid) const noexcept;

    private:
        [[nodiscard]] EditorAssetDocument* findMutable(
            AssetGuid assetGuid) noexcept;
        void acquirePin(AssetGuid assetGuid);
        void releasePin(AssetGuid assetGuid);
        void chooseMostRecentActive() noexcept;

        const EditorAssetViewerRegistry* registry_ = nullptr;
        RuntimePinCallback pinCallback_;
        std::vector<EditorAssetDocument> documents_;
        std::map<AssetGuid, size_t> pinReferences_;
        std::optional<AssetGuid> activeAssetGuid_;
        uint64_t activationSerial_ = 0;
    };

} // namespace Iridium
