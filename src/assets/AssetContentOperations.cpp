#include "assets/AssetContentOperations.h"

#include "assets/AssetMetadata.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace Iridium {

    namespace {

        constexpr std::string_view kFolderMarker = ".iridium-folder";

        bool validName(std::string_view name) {
            constexpr std::string_view invalid = "<>:\"/\\|?*";
            return !name.empty() && name != "." && name != ".." &&
                name.find_first_of(invalid) == std::string_view::npos &&
                name.back() != ' ' && name.back() != '.';
        }

        bool inside(const std::filesystem::path& root,
            const std::filesystem::path& candidate) {
            std::error_code error;
            const std::filesystem::path canonicalRoot =
                std::filesystem::weakly_canonical(root, error);
            if (error) return false;
            const std::filesystem::path canonicalParent =
                std::filesystem::weakly_canonical(
                    candidate.parent_path(), error);
            if (error) return false;
            const std::filesystem::path relative =
                (canonicalParent / candidate.filename())
                    .lexically_normal()
                    .lexically_relative(canonicalRoot);
            return relative != "." && !relative.empty() &&
                !relative.generic_string().starts_with("..") &&
                !relative.is_absolute();
        }

        std::filesystem::path resolve(
            const AssetRoot& root,
            const std::filesystem::path& relative,
            bool allowRoot = false) {
            if (relative.is_absolute()) {
                throw std::runtime_error(
                    "Project content paths must be relative to the asset root.");
            }
            if (allowRoot && relative.empty()) {
                return root.path.lexically_normal();
            }
            const std::filesystem::path result =
                (root.path / relative).lexically_normal();
            const std::filesystem::path normalizedRoot =
                root.path.lexically_normal();
            if ((!allowRoot && !inside(root.path, result)) ||
                (allowRoot && result != normalizedRoot &&
                    !inside(root.path, result))) {
                throw std::runtime_error(
                    "Project content path escapes the registered asset root.");
            }
            return result;
        }

        void writeFolderMarker(
            const std::filesystem::path& directory) {
            const std::filesystem::path marker =
                directory / kFolderMarker;
            if (std::filesystem::exists(marker)) return;
            std::ofstream output(marker,
                std::ios::binary | std::ios::out);
            if (!output) {
                throw std::runtime_error(
                    "Could not create the source-controlled folder marker.");
            }
            output << "{\n  \"schemaVersion\": 1\n}\n";
            if (!output) {
                throw std::runtime_error(
                    "Could not write the source-controlled folder marker.");
            }
        }

        void copyFileCancellable(
            const std::filesystem::path& source,
            const std::filesystem::path& destination,
            std::stop_token stopToken) {
            std::ifstream input(
                source, std::ios::binary);
            if (!input) {
                throw std::runtime_error(
                    "Could not read import source dependency: " +
                    source.generic_string());
            }
            std::filesystem::create_directories(
                destination.parent_path());
            std::ofstream output(
                destination,
                std::ios::binary |
                    std::ios::trunc);
            if (!output) {
                throw std::runtime_error(
                    "Could not create staged project asset: " +
                    destination.generic_string());
            }
            std::vector<char> buffer(
                1024 * 1024);
            while (input) {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "Asset import cancelled.");
                }
                input.read(
                    buffer.data(),
                    static_cast<std::streamsize>(
                        buffer.size()));
                const std::streamsize count =
                    input.gcount();
                if (count > 0) {
                    output.write(
                        buffer.data(), count);
                    if (!output) {
                        throw std::runtime_error(
                            "Could not write staged project asset.");
                    }
                }
            }
            if (!input.eof()) {
                throw std::runtime_error(
                    "Could not finish reading import source dependency.");
            }
        }

        bool externalUri(std::string_view uri) {
            return !uri.empty() &&
                !uri.starts_with("data:") &&
                uri.find("://") == std::string_view::npos;
        }

        std::vector<std::string*> gltfUris(
            nlohmann::json& document) {
            std::vector<std::string*> result;
            for (const char* collection :
                { "buffers", "images" }) {
                const auto found =
                    document.find(collection);
                if (found == document.end() ||
                    !found->is_array()) {
                    continue;
                }
                for (auto& record : *found) {
                    if (!record.is_object()) continue;
                    const auto uri = record.find("uri");
                    if (uri != record.end() &&
                        uri->is_string()) {
                        result.push_back(
                            &uri->get_ref<std::string&>());
                    }
                }
            }
            return result;
        }

        nlohmann::json readGltf(
            const std::filesystem::path& path,
            std::stop_token stopToken = {}) {
            std::ifstream input(
                path,
                std::ios::binary |
                    std::ios::ate);
            if (!input) {
                throw std::runtime_error(
                    "Could not read glTF source.");
            }
            const std::streamsize size =
                input.tellg();
            if (size < 0) {
                throw std::runtime_error(
                    "Could not size glTF source.");
            }
            input.seekg(0, std::ios::beg);
            std::string text(
                static_cast<size_t>(size),
                '\0');
            constexpr size_t chunkSize =
                4ull * 1024ull * 1024ull;
            size_t offset = 0;
            while (offset < text.size()) {
                if (stopToken.stop_requested()) {
                    throw std::runtime_error(
                        "Asset import cancelled.");
                }
                const size_t count =
                    std::min(
                        chunkSize,
                        text.size() - offset);
                if (!input.read(
                        text.data() + offset,
                        static_cast<
                            std::streamsize>(
                                count))) {
                    throw std::runtime_error(
                        "Could not read glTF source.");
                }
                offset += count;
            }
            const nlohmann::json::
                parser_callback_t callback =
                    [stopToken](
                        int,
                        nlohmann::json::
                            parse_event_t,
                        nlohmann::json&) {
                        if (stopToken
                                .stop_requested()) {
                            throw std::
                                runtime_error(
                                    "Asset import cancelled.");
                        }
                        return true;
                    };
            nlohmann::json document =
                nlohmann::json::parse(
                    text.begin(), text.end(),
                    callback);
            if (!document.is_object()) {
                throw std::runtime_error(
                    "glTF source root must be an object.");
            }
            return document;
        }

        void writeJsonAtomic(
            const std::filesystem::path& path,
            const nlohmann::json& document) {
            std::filesystem::path temporary = path;
            temporary += ".move.tmp";
            std::filesystem::path backup = path;
            backup += ".move.backup";
            if (std::filesystem::exists(temporary) ||
                std::filesystem::exists(backup)) {
                throw std::runtime_error(
                    "A stale glTF move transaction file must be removed before retrying.");
            }
            {
                std::ofstream output(temporary,
                    std::ios::binary | std::ios::trunc);
                if (!output) {
                    throw std::runtime_error(
                        "Could not create temporary glTF move output.");
                }
                output << document.dump(2);
                if (!output) {
                    throw std::runtime_error(
                        "Could not write temporary glTF move output.");
                }
            }
            std::error_code error;
            std::filesystem::rename(path, backup, error);
            if (error) {
                std::filesystem::remove(temporary);
                throw std::runtime_error(
                    "Could not stage moved glTF source replacement: " +
                    error.message());
            }
            std::filesystem::rename(temporary, path, error);
            if (error) {
                std::error_code ignored;
                std::filesystem::rename(
                    backup, path, ignored);
                std::filesystem::remove(
                    temporary, ignored);
                throw std::runtime_error(
                    "Could not replace moved glTF source: " +
                    error.message());
            }
            std::filesystem::remove(backup, error);
        }

        void rewriteMovedGltfUris(
            const std::filesystem::path& oldPath,
            const std::filesystem::path& newPath,
            const std::filesystem::path& assetRoot) {
            if (newPath.extension() != ".gltf" ||
                oldPath.parent_path() ==
                    newPath.parent_path()) {
                return;
            }
            nlohmann::json document = readGltf(newPath);
            bool changed = false;
            for (std::string* uri : gltfUris(document)) {
                if (!externalUri(*uri)) continue;
                const std::filesystem::path dependency =
                    (oldPath.parent_path() /
                        std::filesystem::path(*uri))
                        .lexically_normal();
                if (!inside(assetRoot, dependency)) {
                    throw std::runtime_error(
                        "glTF move would reference a dependency outside the project asset root.");
                }
                std::error_code error;
                const std::filesystem::path relative =
                    std::filesystem::relative(
                        dependency,
                        newPath.parent_path(),
                        error);
                if (error || relative.empty()) {
                    throw std::runtime_error(
                        "Could not preserve a relative glTF dependency during the move.");
                }
                *uri = relative.generic_string();
                changed = true;
            }
            if (changed) {
                writeJsonAtomic(newPath, document);
            }
        }

        void validateFolderMoveDependencies(
            const std::filesystem::path& directory) {
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator iterator(
                    directory,
                    std::filesystem::directory_options::
                        skip_permission_denied,
                    error), end;
                !error && iterator != end;
                iterator.increment(error)) {
                if (!iterator->is_regular_file(error) ||
                    iterator->path().extension() != ".gltf") {
                    continue;
                }
                nlohmann::json document =
                    readGltf(iterator->path());
                for (std::string* uri :
                    gltfUris(document)) {
                    if (!externalUri(*uri)) continue;
                    const std::filesystem::path dependency =
                        (iterator->path().parent_path() /
                            std::filesystem::path(*uri))
                            .lexically_normal();
                    if (!inside(directory, dependency)) {
                        throw std::runtime_error(
                            "Folder rename is unsafe because a contained glTF references a file outside that folder. Move the whole dependency set or move the asset instead.");
                    }
                }
            }
            if (error) {
                throw std::runtime_error(
                    "Could not inspect folder dependencies before rename: " +
                    error.message());
            }
        }

        void movePair(
            const std::filesystem::path& source,
            const std::filesystem::path& sidecar,
            const std::filesystem::path& destination,
            const std::filesystem::path& destinationSidecar,
            const std::filesystem::path& assetRoot) {
            if (!std::filesystem::is_regular_file(source) ||
                !std::filesystem::is_regular_file(sidecar)) {
                throw std::runtime_error(
                    "Asset source and metadata sidecar must both exist.");
            }
            if (std::filesystem::exists(destination) ||
                std::filesystem::exists(destinationSidecar)) {
                throw std::runtime_error(
                    "The destination already contains an asset with that name.");
            }
            std::filesystem::create_directories(
                destination.parent_path());
            std::filesystem::rename(source, destination);
            try {
                std::filesystem::rename(
                    sidecar, destinationSidecar);
                rewriteMovedGltfUris(
                    source, destination, assetRoot);
            }
            catch (...) {
                std::error_code ignored;
                if (std::filesystem::exists(destinationSidecar)) {
                    std::filesystem::rename(
                        destinationSidecar, sidecar, ignored);
                }
                if (std::filesystem::exists(destination)) {
                    std::filesystem::rename(
                        destination, source, ignored);
                }
                throw;
            }
        }

        AssetContentMutationResult failure(
            const std::exception& exception) {
            return {
                .diagnostic = exception.what(),
            };
        }

    } // namespace

    AssetContentOperations::AssetContentOperations(
        std::span<const AssetRoot> roots)
        : roots_(roots.begin(), roots.end()) {}

    const AssetRoot* AssetContentOperations::root(
        std::string_view id) const noexcept {
        const auto found = std::ranges::find_if(
            roots_, [id](const AssetRoot& candidate) {
                return candidate.id == id;
            });
        return found == roots_.end() ? nullptr : &*found;
    }

    AssetContentMutationResult
        AssetContentOperations::importAsset(
            std::string_view rootId,
            const std::filesystem::path&
                destinationDirectory,
            const std::filesystem::path&
                requestedSource,
            std::stop_token stopToken) const {
        std::filesystem::path staging;
        try {
            const AssetRoot* assetRoot =
                root(rootId);
            if (!assetRoot) {
                throw std::runtime_error(
                    "Import target uses an unknown project asset root.");
            }
            const std::filesystem::path source =
                std::filesystem::absolute(
                    requestedSource)
                    .lexically_normal();
            if (!std::filesystem::
                    is_regular_file(source)) {
                throw std::runtime_error(
                    "Import source is not a regular file.");
            }
            if (inside(
                    assetRoot->path,
                    source)) {
                return {
                    .path = source,
                    .previousPath = source,
                };
            }

            const std::filesystem::path
                destinationParent =
                    resolve(
                        *assetRoot,
                        destinationDirectory,
                        true);
            if (!std::filesystem::is_directory(
                    destinationParent)) {
                throw std::runtime_error(
                    "Import destination is not a project folder.");
            }
            const std::string packageName =
                source.stem().string();
            if (!validName(packageName)) {
                throw std::runtime_error(
                    "Import source filename cannot form a project package folder.");
            }
            const std::filesystem::path package =
                destinationParent /
                packageName;
            staging =
                destinationParent /
                ("." + packageName +
                    ".iridium-importing");
            if (std::filesystem::exists(package) ||
                std::filesystem::exists(staging)) {
                throw std::runtime_error(
                    "The import destination already contains a package named '" +
                    packageName + "'.");
            }
            std::filesystem::create_directory(
                staging);
            copyFileCancellable(
                source,
                staging / source.filename(),
                stopToken);

            if (source.extension() == ".gltf") {
                nlohmann::json document =
                    readGltf(
                        source,
                        stopToken);
                std::set<std::filesystem::path>
                    copied;
                for (std::string* uri :
                    gltfUris(document)) {
                    if (!externalUri(*uri)) {
                        continue;
                    }
                    const std::filesystem::path
                        relative =
                            std::filesystem::path(
                                *uri)
                                .lexically_normal();
                    if (relative.empty() ||
                        relative.is_absolute() ||
                        relative == ".." ||
                        relative.generic_string()
                            .starts_with("../")) {
                        throw std::runtime_error(
                            "glTF import dependency escapes its source package: " +
                            *uri);
                    }
                    const std::filesystem::path
                        dependency =
                            (source.parent_path() /
                                relative)
                                .lexically_normal();
                    if (!inside(
                            source.parent_path(),
                            dependency) ||
                        !std::filesystem::
                            is_regular_file(
                                dependency)) {
                        throw std::runtime_error(
                            "glTF import dependency is missing or outside its source package: " +
                            dependency.generic_string());
                    }
                    if (!copied.insert(
                            relative).second) {
                        continue;
                    }
                    copyFileCancellable(
                        dependency,
                        staging / relative,
                        stopToken);
                }
            }
            if (stopToken.stop_requested()) {
                throw std::runtime_error(
                    "Asset import cancelled.");
            }
            std::filesystem::rename(
                staging, package);
            staging.clear();
            return {
                .path =
                    package /
                    source.filename(),
                .previousPath = source,
            };
        }
        catch (const std::exception&
            exception) {
            if (!staging.empty()) {
                std::error_code ignored;
                std::filesystem::remove_all(
                    staging, ignored);
            }
            return failure(exception);
        }
    }

    AssetContentMutationResult
        AssetContentOperations::createFolder(
            std::string_view rootId,
            const std::filesystem::path& parentDirectory,
            std::string_view name) const {
        try {
            const AssetRoot* assetRoot = root(rootId);
            if (!assetRoot || !validName(name)) {
                throw std::runtime_error(
                    "Folder name or project asset root is invalid.");
            }
            const std::filesystem::path path = resolve(
                *assetRoot, parentDirectory /
                    std::filesystem::path(name));
            if (!std::filesystem::create_directory(path)) {
                throw std::runtime_error(
                    "The folder already exists or could not be created.");
            }
            writeFolderMarker(path);
            return { .path = path };
        }
        catch (const std::exception& exception) {
            return failure(exception);
        }
    }

    AssetContentMutationResult
        AssetContentOperations::renameFolder(
            std::string_view rootId,
            const std::filesystem::path& directory,
            std::string_view name) const {
        try {
            const AssetRoot* assetRoot = root(rootId);
            if (!assetRoot || !validName(name) ||
                directory.empty()) {
                throw std::runtime_error(
                    "Folder rename target or name is invalid.");
            }
            const std::filesystem::path source =
                resolve(*assetRoot, directory);
            const std::filesystem::path destination =
                source.parent_path() /
                std::filesystem::path(name);
            if (std::filesystem::exists(destination)) {
                throw std::runtime_error(
                    "A folder with that name already exists.");
            }
            validateFolderMoveDependencies(source);
            std::filesystem::rename(source, destination);
            return {
                .path = destination,
                .previousPath = source,
            };
        }
        catch (const std::exception& exception) {
            return failure(exception);
        }
    }

    AssetContentMutationResult
        AssetContentOperations::deleteFolder(
            std::string_view rootId,
            const std::filesystem::path& directory) const {
        try {
            const AssetRoot* assetRoot = root(rootId);
            if (!assetRoot || directory.empty()) {
                throw std::runtime_error(
                    "Project asset root folders cannot be deleted.");
            }
            const std::filesystem::path path =
                resolve(*assetRoot, directory);
            if (!std::filesystem::is_directory(path)) {
                throw std::runtime_error(
                    "Folder no longer exists.");
            }
            std::filesystem::remove_all(path);
            return { .previousPath = path };
        }
        catch (const std::exception& exception) {
            return failure(exception);
        }
    }

    AssetContentMutationResult
        AssetContentOperations::moveAsset(
            const AssetCatalogRecord& rootRecord,
            const std::filesystem::path&
                destinationDirectory) const {
        try {
            if (rootRecord.parentGuid) {
                throw std::runtime_error(
                    "Imported subassets move with their source asset.");
            }
            const AssetRoot* assetRoot =
                root(rootRecord.assetRoot);
            if (!assetRoot) {
                throw std::runtime_error(
                    "Asset uses an unknown project root.");
            }
            const std::filesystem::path source =
                resolve(*assetRoot,
                    rootRecord.sourcePath);
            const std::filesystem::path destinationFolder =
                resolve(*assetRoot,
                    destinationDirectory, true);
            if (!std::filesystem::is_directory(
                    destinationFolder)) {
                throw std::runtime_error(
                    "Asset destination is not a project folder.");
            }
            const std::filesystem::path destination =
                destinationFolder /
                source.filename();
            if (source == destination) {
                return {
                    .path = destination,
                    .previousPath = source,
                };
            }
            movePair(source,
                assetMetadataSidecarPath(source),
                destination,
                assetMetadataSidecarPath(destination),
                assetRoot->path);
            return {
                .path = destination,
                .previousPath = source,
            };
        }
        catch (const std::exception& exception) {
            return failure(exception);
        }
    }

    AssetContentMutationResult
        AssetContentOperations::renameAsset(
            const AssetCatalogRecord& rootRecord,
            std::string_view name) const {
        try {
            if (rootRecord.parentGuid ||
                !validName(name)) {
                throw std::runtime_error(
                    "Only source assets can be renamed.");
            }
            const AssetRoot* assetRoot =
                root(rootRecord.assetRoot);
            if (!assetRoot) {
                throw std::runtime_error(
                    "Asset uses an unknown project root.");
            }
            const std::filesystem::path source =
                resolve(*assetRoot,
                    rootRecord.sourcePath);
            std::filesystem::path filename(name);
            if (filename.extension().empty()) {
                filename += source.extension();
            }
            if (filename.extension() !=
                source.extension() ||
                filename.filename() != filename) {
                throw std::runtime_error(
                    "Renaming preserves the source asset extension.");
            }
            const std::filesystem::path destination =
                source.parent_path() / filename;
            movePair(source,
                assetMetadataSidecarPath(source),
                destination,
                assetMetadataSidecarPath(destination),
                assetRoot->path);
            return {
                .path = destination,
                .previousPath = source,
            };
        }
        catch (const std::exception& exception) {
            return failure(exception);
        }
    }

    AssetContentMutationResult
        AssetContentOperations::deleteAsset(
            const AssetCatalogRecord& rootRecord) const {
        try {
            if (rootRecord.parentGuid) {
                throw std::runtime_error(
                    "Imported subassets are deleted with their source asset.");
            }
            const AssetRoot* assetRoot =
                root(rootRecord.assetRoot);
            if (!assetRoot) {
                throw std::runtime_error(
                    "Asset uses an unknown project root.");
            }
            const std::filesystem::path source =
                resolve(*assetRoot,
                    rootRecord.sourcePath);
            const std::filesystem::path sidecar =
                assetMetadataSidecarPath(source);
            if (!std::filesystem::remove(source)) {
                throw std::runtime_error(
                    "Asset source no longer exists.");
            }
            std::error_code error;
            if (!std::filesystem::remove(sidecar, error) ||
                error) {
                throw std::runtime_error(
                    "Asset source was deleted, but its sidecar could not be removed. Refresh will report the recoverable orphan.");
            }
            return { .previousPath = source };
        }
        catch (const std::exception& exception) {
            return failure(exception);
        }
    }

} // namespace Iridium
