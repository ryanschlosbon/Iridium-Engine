#include "editor/EditorAssetDocumentService.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Iridium {

    void EditorAssetViewerRegistry::registerViewer(
        EditorAssetViewerRegistration registration) {
        if (frozen_) {
            throw std::logic_error(
                "Asset viewer registry is frozen");
        }
        if (registration.assetType.empty() ||
            registration.viewerId.empty()) {
            throw std::invalid_argument(
                "Asset viewer registration requires stable asset and viewer IDs");
        }
        const auto [position, inserted] = registrations_.emplace(
            registration.assetType, std::move(registration));
        if (!inserted) {
            throw std::logic_error(
                "Duplicate asset viewer registration for " + position->first);
        }
    }

    const EditorAssetViewerRegistration* EditorAssetViewerRegistry::find(
        std::string_view assetType) const noexcept {
        const auto found = registrations_.find(assetType);
        return found == registrations_.end() ? nullptr : &found->second;
    }

    std::vector<EditorAssetViewerRegistration>
        EditorAssetViewerRegistry::registrations() const {
        std::vector<EditorAssetViewerRegistration> result;
        result.reserve(registrations_.size());
        for (const auto& [assetType, registration] : registrations_) {
            (void)assetType;
            result.push_back(registration);
        }
        return result;
    }

    void registerCoreAssetViewers(EditorAssetViewerRegistry& registry) {
        registry.registerViewer({
            .assetType = "iridium.model",
            .viewerId = "iridium.viewer.model",
            .kind = EditorAssetViewerKind::Model,
            .presentationSource = EditorAssetPresentationSource::Asset,
        });
        registry.registerViewer({
            .assetType = "iridium.material",
            .viewerId = "iridium.viewer.material",
            .kind = EditorAssetViewerKind::Material,
            .presentationSource = EditorAssetPresentationSource::ParentAsset,
        });
    }

    EditorAssetDocumentService::EditorAssetDocumentService(
        const EditorAssetViewerRegistry* registry)
        : registry_(registry) {}

    EditorAssetDocumentService::~EditorAssetDocumentService() {
        try {
            closeAll();
        }
        catch (...) {
            // Destruction cannot surface an external runtime-pin callback failure.
        }
    }

    void EditorAssetDocumentService::setRegistry(
        const EditorAssetViewerRegistry* registry) noexcept {
        registry_ = registry;
    }

    void EditorAssetDocumentService::setRuntimePinCallback(
        RuntimePinCallback callback) {
        if (!documents_.empty()) {
            throw std::logic_error(
                "Cannot replace asset viewer pin callback while documents are open");
        }
        pinCallback_ = std::move(callback);
    }

    EditorAssetOpenResult EditorAssetDocumentService::open(
        const EditorAssetOpenRequest& request) {
        if (!registry_) {
            return { .diagnostic = "Asset viewer registry is unavailable" };
        }
        if (request.assetGuid.isNil()) {
            return { .diagnostic = "Cannot open an asset with a nil GUID" };
        }
        const EditorAssetViewerRegistration* registration =
            registry_->find(request.assetType);
        if (!registration) {
            return {
                .diagnostic = "No viewer is registered for asset type " +
                    request.assetType,
            };
        }
        if (EditorAssetDocument* existing = findMutable(request.assetGuid)) {
            existing->activationSerial = ++activationSerial_;
            activeAssetGuid_ = existing->assetGuid;
            return { .succeeded = true, .reused = true };
        }

        AssetGuid presentationAssetGuid = request.assetGuid;
        if (registration->presentationSource ==
            EditorAssetPresentationSource::ParentAsset) {
            if (!request.parentAssetGuid || request.parentAssetGuid->isNil()) {
                return {
                    .diagnostic = "This asset viewer requires a stable parent model GUID",
                };
            }
            presentationAssetGuid = *request.parentAssetGuid;
        }

        acquirePin(presentationAssetGuid);
        try {
            documents_.push_back({
                .assetGuid = request.assetGuid,
                .presentationAssetGuid = presentationAssetGuid,
                .assetType = request.assetType,
                .viewerId = registration->viewerId,
                .displayName = request.displayName.empty()
                    ? request.assetGuid.toString()
                    : request.displayName,
                .kind = registration->kind,
                .activationSerial = ++activationSerial_,
            });
        }
        catch (...) {
            releasePin(presentationAssetGuid);
            throw;
        }
        activeAssetGuid_ = request.assetGuid;
        return { .succeeded = true };
    }

    bool EditorAssetDocumentService::activate(AssetGuid assetGuid) noexcept {
        auto found = std::ranges::find_if(documents_,
            [assetGuid](const EditorAssetDocument& document) {
                return document.assetGuid == assetGuid;
            });
        if (found == documents_.end()) return false;
        found->activationSerial = ++activationSerial_;
        activeAssetGuid_ = assetGuid;
        return true;
    }

    bool EditorAssetDocumentService::close(AssetGuid assetGuid) {
        const auto found = std::ranges::find_if(documents_,
            [assetGuid](const EditorAssetDocument& document) {
                return document.assetGuid == assetGuid;
            });
        if (found == documents_.end()) return false;
        const AssetGuid presentationAssetGuid = found->presentationAssetGuid;
        const bool wasActive = activeAssetGuid_ == assetGuid;
        documents_.erase(found);
        releasePin(presentationAssetGuid);
        if (wasActive) chooseMostRecentActive();
        return true;
    }

    void EditorAssetDocumentService::closeAll() {
        while (!documents_.empty()) {
            (void)close(documents_.back().assetGuid);
        }
        activeAssetGuid_.reset();
    }

    void EditorAssetDocumentService::updateRuntimeState(
        AssetGuid presentationAssetGuid,
        std::optional<RuntimeAssetState> state,
        std::string_view diagnostic) {
        for (EditorAssetDocument& document : documents_) {
            if (document.presentationAssetGuid != presentationAssetGuid) continue;
            document.runtimeState = state;
            document.runtimeDiagnostic = diagnostic;
        }
    }

    const EditorAssetDocument* EditorAssetDocumentService::active() const noexcept {
        return activeAssetGuid_ ? find(*activeAssetGuid_) : nullptr;
    }

    const EditorAssetDocument* EditorAssetDocumentService::find(
        AssetGuid assetGuid) const noexcept {
        const auto found = std::ranges::find_if(documents_,
            [assetGuid](const EditorAssetDocument& document) {
                return document.assetGuid == assetGuid;
            });
        return found == documents_.end() ? nullptr : &*found;
    }

    EditorAssetDocument* EditorAssetDocumentService::findMutable(
        AssetGuid assetGuid) noexcept {
        const auto found = std::ranges::find_if(documents_,
            [assetGuid](const EditorAssetDocument& document) {
                return document.assetGuid == assetGuid;
            });
        return found == documents_.end() ? nullptr : &*found;
    }

    size_t EditorAssetDocumentService::pinReferenceCount(
        AssetGuid presentationAssetGuid) const noexcept {
        const auto found = pinReferences_.find(presentationAssetGuid);
        return found == pinReferences_.end() ? 0 : found->second;
    }

    void EditorAssetDocumentService::acquirePin(AssetGuid assetGuid) {
        auto [found, inserted] = pinReferences_.try_emplace(assetGuid, 0);
        if (inserted && pinCallback_) {
            try {
                pinCallback_(assetGuid, true);
            }
            catch (...) {
                pinReferences_.erase(found);
                throw;
            }
        }
        ++found->second;
    }

    void EditorAssetDocumentService::releasePin(AssetGuid assetGuid) {
        const auto found = pinReferences_.find(assetGuid);
        if (found == pinReferences_.end()) {
            throw std::logic_error("Asset viewer pin reference underflow");
        }
        if (--found->second != 0) return;
        pinReferences_.erase(found);
        if (pinCallback_) pinCallback_(assetGuid, false);
    }

    void EditorAssetDocumentService::chooseMostRecentActive() noexcept {
        if (documents_.empty()) {
            activeAssetGuid_.reset();
            return;
        }
        const auto found = std::ranges::max_element(documents_, {},
            &EditorAssetDocument::activationSerial);
        activeAssetGuid_ = found->assetGuid;
    }

} // namespace Iridium
