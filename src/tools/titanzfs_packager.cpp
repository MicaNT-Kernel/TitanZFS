// ============================================================================
// TitanZFS Standalone Single-File Installer Packager
// Packs target executables and assets into the PE overlay of titanzfs_setup.exe
// ============================================================================

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include "titanzfs/compression.hpp"

namespace fs = std::filesystem;

#pragma pack(push, 1)
struct PayloadFooter {
    uint64_t tocOffset{0};
    uint64_t payloadStart{0};
    char     magic[16]{"TITANZFS_BUNDLE"};
};
#pragma pack(pop)
static_assert(sizeof(PayloadFooter) == 32, "PayloadFooter must be 32 bytes");

struct PackEntry {
    std::string name;
    std::vector<uint8_t> data;
    uint64_t dataOffset{0};
    uint64_t compSize{0};
    uint64_t uncompSize{0};
    uint32_t isCompressed{0};
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: titanzfs_packager <target_setup_exe> <file1> [file2 ...]\n";
        return 1;
    }

    fs::path targetExePath(argv[1]);
    if (!fs::exists(targetExePath)) {
        std::cerr << "Target setup executable not found: " << targetExePath << "\n";
        return 1;
    }

    // 1. Check if the target executable already has an embedded payload footer
    uint64_t fileSize = fs::file_size(targetExePath);
    if (fileSize >= sizeof(PayloadFooter)) {
        std::ifstream in(targetExePath, std::ios::binary);
        if (in.is_open()) {
            in.seekg(fileSize - sizeof(PayloadFooter), std::ios::beg);
            PayloadFooter footer{};
            in.read(reinterpret_cast<char*>(&footer), sizeof(footer));
            if (std::memcmp(footer.magic, "TITANZFS_BUNDLE", 15) == 0 &&
                footer.payloadStart > 0 && footer.payloadStart < fileSize) {
                in.close();
                std::cout << "[Packager] Found previous payload at offset " << footer.payloadStart 
                          << ". Truncating executable before repacking...\n";
                std::error_code ec;
                fs::resize_file(targetExePath, footer.payloadStart, ec);
                fileSize = fs::file_size(targetExePath);
            }
        }
    }

    uint64_t payloadStartOffset = fileSize;
    std::cout << "[Packager] Packaging payload into " << targetExePath.filename() 
              << " starting at offset " << payloadStartOffset << "...\n";

    // 2. Read and compress each input file
    std::vector<PackEntry> entries;
    for (int i = 2; i < argc; ++i) {
        fs::path p(argv[i]);
        if (!fs::exists(p)) {
            std::cerr << "[Packager Warning] Skipping missing file: " << p << "\n";
            continue;
        }

        std::ifstream file(p, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[Packager Error] Cannot open: " << p << "\n";
            continue;
        }

        file.seekg(0, std::ios::end);
        size_t srcSize = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> raw(srcSize);
        file.read(reinterpret_cast<char*>(raw.data()), srcSize);

        // Normalize destination relative path name
        std::string name = p.filename().string();
        if (name == "titanzfs_hero.jpg") {
            name = "assets/titanzfs_hero.jpg";
        } else if (name.find("winfsp") != std::string::npos && name.ends_with(".msi")) {
            name = "redist/winfsp.msi";
        }

        PackEntry ent;
        ent.name = name;
        ent.uncompSize = srcSize;

        // Attempt LZ4 compression
        std::vector<uint8_t> comp;
        size_t cSize = titanzfs::CompressionEngine::CompressLZ4(raw, comp);
        if (cSize > 0 && cSize < srcSize) {
            ent.data = std::move(comp);
            ent.compSize = ent.data.size();
            ent.isCompressed = 1;
            std::cout << "  + Packed: " << name << " (" << srcSize / 1024 << " KB -> " 
                      << ent.compSize / 1024 << " KB LZ4)\n";
        } else {
            ent.data = std::move(raw);
            ent.compSize = srcSize;
            ent.isCompressed = 0;
            std::cout << "  + Packed: " << name << " (" << srcSize / 1024 << " KB uncompressed)\n";
        }

        entries.push_back(std::move(ent));
    }

    if (entries.empty()) {
        std::cerr << "[Packager Error] No files were packaged.\n";
        return 1;
    }

    // 3. Open target executable for appending
    std::ofstream out(targetExePath, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        std::cerr << "[Packager Error] Failed to open target executable for writing.\n";
        return 1;
    }

    // Write file payloads
    uint64_t curOffset = payloadStartOffset;
    for (auto& ent : entries) {
        ent.dataOffset = curOffset;
        out.write(reinterpret_cast<const char*>(ent.data.data()), ent.data.size());
        curOffset += ent.data.size();
    }

    // 4. Write Table of Contents (TOC)
    uint64_t tocOffset = curOffset;
    uint32_t entryCount = static_cast<uint32_t>(entries.size());
    out.write(reinterpret_cast<const char*>(&entryCount), sizeof(entryCount));

    for (const auto& ent : entries) {
        uint32_t nameLen = static_cast<uint32_t>(ent.name.size());
        out.write(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        out.write(ent.name.data(), nameLen);
        out.write(reinterpret_cast<const char*>(&ent.dataOffset), sizeof(ent.dataOffset));
        out.write(reinterpret_cast<const char*>(&ent.compSize), sizeof(ent.compSize));
        out.write(reinterpret_cast<const char*>(&ent.uncompSize), sizeof(ent.uncompSize));
        out.write(reinterpret_cast<const char*>(&ent.isCompressed), sizeof(ent.isCompressed));
    }

    // 5. Write 32-byte Footer
    PayloadFooter footer{};
    footer.tocOffset = tocOffset;
    footer.payloadStart = payloadStartOffset;
    std::memcpy(footer.magic, "TITANZFS_BUNDLE", 16);
    out.write(reinterpret_cast<const char*>(&footer), sizeof(footer));
    out.close();

    uint64_t finalSize = fs::file_size(targetExePath);
    std::cout << "[Packager Success] Complete! Standalone installer size: " 
              << finalSize / 1024 << " KB (Payload: " 
              << (finalSize - payloadStartOffset) / 1024 << " KB)\n";

    return 0;
}
