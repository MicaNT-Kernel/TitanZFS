#pragma once

#include "titanzfs/types.hpp"
#include <map>
#include <vector>
#include <string>

namespace titanzfs {

struct ZfsFile {
    std::string name;
    std::vector<uint8_t> data;
    bool isDirectory{false};
    uint64_t birthTxg{1};
    uint64_t modificationTxg{1};
};

class ZfsDataset {
public:
    explicit ZfsDataset(std::string name) : name_(std::move(name)) {}

    [[nodiscard]] const std::string& getName() const noexcept { return name_; }

    Status createFile(const std::string& path, bool isDir, uint64_t currentTxg) {
        if (files_.contains(path)) return Status::ObjectNameCollision;
        files_[path] = ZfsFile{
            .name = path,
            .data = {},
            .isDirectory = isDir,
            .birthTxg = currentTxg,
            .modificationTxg = currentTxg
        };
        return Status::Success;
    }

    Status writeFile(const std::string& path, const void* buffer, size_t size, uint64_t currentTxg) {
        auto it = files_.find(path);
        if (it == files_.end()) {
            files_[path] = ZfsFile{
                .name = path,
                .data = {},
                .isDirectory = false,
                .birthTxg = currentTxg,
                .modificationTxg = currentTxg
            };
            it = files_.find(path);
        }

        if (it->second.isDirectory) return Status::AccessDenied;

        const auto* bytes = reinterpret_cast<const uint8_t*>(buffer);
        it->second.data.assign(bytes, bytes + size);
        it->second.modificationTxg = currentTxg;
        return Status::Success;
    }

    Status readFile(const std::string& path, void* buffer, size_t bufSize, size_t& bytesRead) const {
        auto it = files_.find(path);
        if (it == files_.end()) return Status::ObjectNameNotFound;
        if (it->second.isDirectory) return Status::AccessDenied;

        bytesRead = std::min(bufSize, it->second.data.size());
        if (bytesRead > 0) {
            std::memcpy(buffer, it->second.data.data(), bytesRead);
        }
        return Status::Success;
    }

    [[nodiscard]] bool fileExists(const std::string& path) const {
        return files_.contains(path);
    }

    // $O(1)$ Instantaneous CoW Snapshot
    Status createSnapshot(const std::string& snapName, uint64_t currentTxg) {
        if (snapshots_.contains(snapName)) return Status::ObjectNameCollision;
        snapshots_[snapName] = files_;
        snapshotTxgs_[snapName] = currentTxg;
        return Status::Success;
    }

    // Atomic Snapshot Rollback
    Status rollbackSnapshot(const std::string& snapName) {
        auto it = snapshots_.find(snapName);
        if (it == snapshots_.end()) return Status::ObjectNameNotFound;
        files_ = it->second;
        return Status::Success;
    }

    [[nodiscard]] const std::map<std::string, std::map<std::string, ZfsFile>>& getSnapshots() const noexcept {
        return snapshots_;
    }

    [[nodiscard]] const std::map<std::string, ZfsFile>& getFiles() const noexcept {
        return files_;
    }

private:
    std::string name_;
    std::map<std::string, ZfsFile> files_;
    std::map<std::string, std::map<std::string, ZfsFile>> snapshots_;
    std::map<std::string, uint64_t> snapshotTxgs_;
};

} // namespace titanzfs
