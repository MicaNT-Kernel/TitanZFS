#pragma once

#include "titanzfs/types.hpp"
#include "titanzfs/fletcher4.hpp"
#include "titanzfs/compression.hpp"
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <iostream>

namespace titanzfs {

struct ZfsFile {
    std::string name;
    std::vector<uint8_t> data;
    bool isDirectory{false};
    uint64_t birthTxg{1};
    uint64_t modificationTxg{1};
};

// ============================================================================
// Clean-Room On-Wire Stream Serialization Protocol (zfs send / zfs recv)
// ============================================================================

#pragma pack(push, 1)
struct ZfsStreamHeader {
    uint64_t magic{0x5A46535354524D31ULL}; // "ZFSSTRM1" (Clean-room ZFS stream signature)
    uint64_t fileCount{0};
    uint64_t streamBirthTxg{0};
    Checksum256 streamChecksum{};
};

struct ZfsStreamFileRecord {
    uint32_t pathLength{0};
    uint32_t isDirectory{0};
    uint64_t uncompressedSize{0};
    uint64_t payloadSize{0};
    uint64_t compType{0};
    Checksum256 payloadChecksum{};
};
#pragma pack(pop)

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

    // O(1) Instantaneous CoW Snapshot
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

    Status destroySnapshot(const std::string& snapName) {
        auto it = snapshots_.find(snapName);
        if (it == snapshots_.end()) return Status::ObjectNameNotFound;
        snapshots_.erase(it);
        snapshotTxgs_.erase(snapName);
        return Status::Success;
    }

    [[nodiscard]] const std::map<std::string, std::map<std::string, ZfsFile>>& getSnapshots() const noexcept {
        return snapshots_;
    }

    [[nodiscard]] const std::map<std::string, ZfsFile>& getFiles() const noexcept {
        return files_;
    }

    // ------------------------------------------------------------------------
    // ZFS Send: Serialize Snapshot to Compressed, Checksummed Binary Stream
    // ------------------------------------------------------------------------
    Status sendSnapshot(const std::string& snapName, std::vector<uint8_t>& outStream) const {
        const std::map<std::string, ZfsFile>* pFiles = nullptr;
        uint64_t birthTxg = 1;

        if (snapName.empty() || snapName == "$ACTIVE$") {
            pFiles = &files_;
        } else {
            auto snapIt = snapshots_.find(snapName);
            if (snapIt == snapshots_.end()) return Status::ObjectNameNotFound;
            pFiles = &snapIt->second;
            auto txgIt = snapshotTxgs_.find(snapName);
            if (txgIt != snapshotTxgs_.end()) birthTxg = txgIt->second;
        }

        const auto& snapFiles = *pFiles;
        outStream.clear();

        ZfsStreamHeader hdr{};
        hdr.fileCount = snapFiles.size();
        auto txgIt = snapshotTxgs_.find(snapName);
        hdr.streamBirthTxg = (txgIt != snapshotTxgs_.end()) ? txgIt->second : 1;

        // Reserve space for header at start of stream
        const size_t hdrOffset = 0;
        outStream.resize(sizeof(ZfsStreamHeader));

        for (const auto& [path, f] : snapFiles) {
            std::vector<uint8_t> payload;
            uint64_t compType = BP_COMP_OFF;

            if (!f.isDirectory && !f.data.empty()) {
                std::span<const uint8_t> dataSpan(f.data.data(), f.data.size());
                CompressionEngine::CompressBlock(dataSpan, payload, compType);
            }

            Checksum256 payloadCs{};
            if (!payload.empty()) {
                payloadCs = Fletcher4::Compute(payload);
            }

            ZfsStreamFileRecord rec{
                .pathLength = static_cast<uint32_t>(path.size()),
                .isDirectory = f.isDirectory ? 1u : 0u,
                .uncompressedSize = f.data.size(),
                .payloadSize = payload.size(),
                .compType = compType,
                .payloadChecksum = payloadCs
            };

            // Append record
            const size_t recOff = outStream.size();
            outStream.resize(recOff + sizeof(rec));
            std::memcpy(outStream.data() + recOff, &rec, sizeof(rec));

            // Append path string
            const size_t pathOff = outStream.size();
            outStream.resize(pathOff + path.size());
            std::memcpy(outStream.data() + pathOff, path.data(), path.size());

            // Append payload bytes
            if (!payload.empty()) {
                const size_t dataOff = outStream.size();
                outStream.resize(dataOff + payload.size());
                std::memcpy(outStream.data() + dataOff, payload.data(), payload.size());
            }
        }

        // Compute end-to-end Fletcher-4 checksum of entire stream body
        std::span<const uint8_t> bodySpan(outStream.data() + sizeof(ZfsStreamHeader), outStream.size() - sizeof(ZfsStreamHeader));
        hdr.streamChecksum = Fletcher4::Compute(bodySpan);
        std::memcpy(outStream.data() + hdrOffset, &hdr, sizeof(ZfsStreamHeader));

        return Status::Success;
    }

    // ------------------------------------------------------------------------
    // ZFS Recv: Ingest & Deserialize Stream with Bit-Flip Verification
    // ------------------------------------------------------------------------
    Status receiveStream(std::span<const uint8_t> inStream) {
        if (inStream.size() < sizeof(ZfsStreamHeader)) return Status::BufferTooSmall;

        ZfsStreamHeader hdr{};
        std::memcpy(&hdr, inStream.data(), sizeof(ZfsStreamHeader));

        if (hdr.magic != 0x5A46535354524D31ULL) return Status::InvalidParameter;

        // Verify end-to-end stream checksum
        std::span<const uint8_t> bodySpan(inStream.data() + sizeof(ZfsStreamHeader), inStream.size() - sizeof(ZfsStreamHeader));
        if (!Fletcher4::Verify(bodySpan, hdr.streamChecksum)) {
            return Status::DataChecksumError;
        }

        size_t cursor = sizeof(ZfsStreamHeader);
        for (uint64_t i = 0; i < hdr.fileCount; ++i) {
            if (cursor + sizeof(ZfsStreamFileRecord) > inStream.size()) {
                return Status::BufferTooSmall;
            }

            ZfsStreamFileRecord rec{};
            std::memcpy(&rec, inStream.data() + cursor, sizeof(rec));
            cursor += sizeof(rec);

            if (cursor + rec.pathLength + rec.payloadSize > inStream.size()) {
                return Status::BufferTooSmall;
            }

            std::string path(reinterpret_cast<const char*>(inStream.data() + cursor), rec.pathLength);
            cursor += rec.pathLength;

            std::vector<uint8_t> fileData;
            if (rec.payloadSize > 0) {
                std::span<const uint8_t> payloadSpan(inStream.data() + cursor, rec.payloadSize);
                if (!Fletcher4::Verify(payloadSpan, rec.payloadChecksum)) {
                    return Status::DataChecksumError;
                }

                fileData.resize(rec.uncompressedSize);
                Status decompSt = CompressionEngine::DecompressBlock(payloadSpan, fileData, rec.compType);
                if (decompSt != Status::Success) return decompSt;

                cursor += rec.payloadSize;
            }

            files_[path] = ZfsFile{
                .name = path,
                .data = std::move(fileData),
                .isDirectory = (rec.isDirectory != 0),
                .birthTxg = hdr.streamBirthTxg,
                .modificationTxg = hdr.streamBirthTxg
            };
        }

        return Status::Success;
    }

    Status receiveSnapshotStream(const std::string& snapName, uint64_t snapTxg, std::span<const uint8_t> inStream) {
        if (inStream.empty()) {
            snapshots_[snapName] = {};
            snapshotTxgs_[snapName] = snapTxg;
            return Status::Success;
        }
        ZfsDataset tmp("tmp");
        Status st = tmp.receiveStream(inStream);
        if (st != Status::Success) return st;
        snapshots_[snapName] = tmp.getFiles();
        snapshotTxgs_[snapName] = snapTxg;
        return Status::Success;
    }

private:
    std::string name_;
    std::map<std::string, ZfsFile> files_;
    std::map<std::string, std::map<std::string, ZfsFile>> snapshots_;
    std::map<std::string, uint64_t> snapshotTxgs_;
};

} // namespace titanzfs
