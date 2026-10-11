// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>
#include <filesystem>
namespace Common::FS {
enum class FileAccessMode { Read, ReadWrite };
enum class FileType { BinaryFile };
class IOFile {
public:
    IOFile(const std::filesystem::path& path, FileAccessMode mode, FileType) {
        file = std::fopen(path.string().c_str(), mode == FileAccessMode::Read ? "rb" : "r+b");
    }
    ~IOFile() {
        if (file)
            std::fclose(file);
    }
    bool IsOpen() const {
        return file != nullptr;
    }
    template <class T>
    std::size_t Read(T& value) const {
        return file ? std::fread(&value, sizeof(T), 1, file) : 0;
    }
    template <class T>
    std::size_t Write(const T& value) const {
        return file ? std::fwrite(&value, sizeof(T), 1, file) : 0;
    }

private:
    std::FILE* file{};
};
} // namespace Common::FS
