/* C bridge between the guest file system (port/src/runtime_file.c) and the vendored ZArchive
 * library (third_party/zarchive, MIT-0, by Exzap). Based on @R-V-L's bridge in R-V-L/gow3_pc
 * (SpartanPort .zar build); TLG adds a reader pool and the packer.
 *
 * ZArchiveReader serialises ReadFromFile on one mutex (its block cache is shared state). The game
 * streams from several threads at once, so the archive keeps a few readers on the same file and a
 * read takes the first idle one. Lookups and listings only touch the immutable file tree. */
#include "../../src/runtime_zarchive.h"

#include <zarchive/zarchivereader.h>
#include <zarchive/zarchivewriter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr size_t kReaders = 4;

struct Slot {
    std::unique_ptr<ZArchiveReader> reader;
    std::mutex mutex;
};

thread_local std::string open_error;

std::string_view relative_path(const char* path) {
    std::string_view view = path ? path : "";
    while (!view.empty() && (view.front() == '/' || view.front() == '\\'))
        view.remove_prefix(1);
    return view;
}

fs::path native_path(const char* path) {
    return fs::u8path(path); // launcher paths are UTF-8
}
} // namespace

struct ZArchive {
    Slot slots[kReaders];
    uint64_t size{};
    std::atomic<uint32_t> next{0};
    ZArchiveReader* tree() const { return slots[0].reader.get(); }
};

extern "C" {

ZArchive* zarchive_open(const char* path) {
    open_error.clear();
    if (!path || !*path) {
        open_error = "empty path";
        return nullptr;
    }
    std::error_code ec;
    const fs::path file = native_path(path);
    const uint64_t size = fs::file_size(file, ec);
    if (ec) {
        open_error = "cannot read the archive: " + ec.message();
        return nullptr;
    }
    auto archive = std::make_unique<ZArchive>();
    archive->size = size;
    for (Slot& slot : archive->slots) {
        try {
            slot.reader.reset(ZArchiveReader::OpenFromFile(file));
        } catch (const std::exception& error) {
            open_error = std::string("not a ZArchive: ") + error.what();
            return nullptr;
        }
        if (!slot.reader) {
            open_error = "not a ZArchive (bad footer or tables)";
            return nullptr;
        }
    }
    return archive.release();
}

void zarchive_close(ZArchive* archive) {
    delete archive;
}

uint64_t zarchive_size(const ZArchive* archive) {
    return archive ? archive->size : 0;
}

const char* zarchive_error(void) {
    return open_error.c_str();
}

int64_t zarchive_find(ZArchive* archive, const char* path) {
    if (!archive)
        return ZARCHIVE_NONE;
    const ZArchiveNodeHandle node = archive->tree()->LookUp(relative_path(path), true, true);
    return node == ZARCHIVE_INVALID_NODE ? ZARCHIVE_NONE : static_cast<int64_t>(node);
}

int zarchive_is_file(ZArchive* archive, int64_t node) {
    return archive && node >= 0 && archive->tree()->IsFile(static_cast<ZArchiveNodeHandle>(node));
}

uint64_t zarchive_entry_size(ZArchive* archive, int64_t node) {
    if (!zarchive_is_file(archive, node))
        return 0;
    return archive->tree()->GetFileSize(static_cast<ZArchiveNodeHandle>(node));
}

int zarchive_child(ZArchive* archive, int64_t node, uint32_t index, char* name, size_t size,
                   int* is_file) {
    if (!archive || node < 0)
        return 0;
    ZArchiveReader::DirEntry entry{};
    if (!archive->tree()->GetDirEntry(static_cast<ZArchiveNodeHandle>(node), index, entry))
        return 0;
    if (name && size) {
        const size_t length = std::min(entry.name.size(), size - 1);
        std::memcpy(name, entry.name.data(), length);
        name[length] = '\0';
    }
    if (is_file)
        *is_file = entry.isFile ? 1 : 0;
    return 1;
}

uint64_t zarchive_read(ZArchive* archive, int64_t node, uint64_t offset, void* buffer,
                       uint64_t length) {
    if (!archive || node < 0 || !buffer || !length)
        return 0;
    const uint32_t start = archive->next.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < kReaders; ++i) {
        Slot& slot = archive->slots[(start + i) % kReaders];
        std::unique_lock lock(slot.mutex, std::try_to_lock);
        if (lock.owns_lock())
            return slot.reader->ReadFromFile(static_cast<ZArchiveNodeHandle>(node), offset,
                                             length, buffer);
    }
    Slot& slot = archive->slots[start % kReaders];
    std::lock_guard lock(slot.mutex);
    return slot.reader->ReadFromFile(static_cast<ZArchiveNodeHandle>(node), offset, length, buffer);
}

namespace {
struct PackOutput {
    std::FILE* file{};
    bool failed{};
    uint64_t written{};
};

void pack_new_file(int32_t, void*) {} // the writer never splits the output

void pack_write(const void* data, size_t length, void* ctx) {
    auto* out = static_cast<PackOutput*>(ctx);
    if (out->failed)
        return;
    if (std::fwrite(data, 1, length, out->file) != length)
        out->failed = true;
    out->written += length;
}
} // namespace

int zarchive_pack(const char* folder, const char* out_path) {
    std::error_code ec;
    const fs::path root = native_path(folder);
    const fs::path target = native_path(out_path);
    if (!fs::is_directory(root, ec)) {
        std::fprintf(stderr, "Pack: %s is not a folder\n", folder);
        return 1;
    }
    // Collect first: the total gives the progress, and the archive itself must not be packed.
    struct Entry {
        fs::path path;
        std::string name;
        bool is_file;
        uint64_t size;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
        const fs::path& path = it->path();
        if (fs::equivalent(path, target, ec)) {
            ec.clear();
            continue;
        }
        ec.clear();
        const std::u8string u8 = fs::relative(path, root).generic_u8string();
        std::string name(u8.begin(), u8.end());
        if (it->is_directory()) {
            entries.push_back({path, std::move(name), false, 0});
        } else if (it->is_regular_file()) {
            const uint64_t size = it->file_size();
            total += size;
            entries.push_back({path, std::move(name), true, size});
        }
    }
    if (ec) {
        std::fprintf(stderr, "Pack: cannot list %s: %s\n", folder, ec.message().c_str());
        return 1;
    }
    PackOutput out;
#ifdef _WIN32
    out.file = _wfopen(target.c_str(), L"wb");
#else
    out.file = std::fopen(target.c_str(), "wb");
#endif
    if (!out.file) {
        std::fprintf(stderr, "Pack: cannot create %s\n", out_path);
        return 1;
    }
    std::printf("Pack: %zu entries, %.2f GiB to compress into %s\n", entries.size(),
                total / 1073741824.0, out_path);
    std::fflush(stdout);
    std::vector<unsigned char> buffer(4u << 20);
    uint64_t done = 0;
    int last_percent = -1;
    bool failed = false;
    {
        ZArchiveWriter writer(pack_new_file, pack_write, &out);
        for (const Entry& entry : entries) {
            if (!entry.is_file) {
                writer.MakeDir(entry.name.c_str(), true);
                continue;
            }
            if (!writer.StartNewFile(entry.name.c_str())) {
                std::fprintf(stderr, "Pack: cannot add %s\n", entry.name.c_str());
                failed = true;
                break;
            }
#ifdef _WIN32
            std::FILE* in = _wfopen(entry.path.c_str(), L"rb");
#else
            std::FILE* in = std::fopen(entry.path.c_str(), "rb");
#endif
            if (!in) {
                std::fprintf(stderr, "Pack: cannot read %s\n", entry.name.c_str());
                failed = true;
                break;
            }
            size_t n;
            while ((n = std::fread(buffer.data(), 1, buffer.size(), in)) > 0) {
                writer.AppendData(buffer.data(), n);
                done += n;
                const int percent = total ? static_cast<int>(done * 100 / total) : 100;
                if (percent != last_percent) {
                    last_percent = percent;
                    std::printf("Pack: %d%%\n", percent);
                    std::fflush(stdout);
                }
            }
            const bool read_error = std::ferror(in);
            std::fclose(in);
            if (read_error || out.failed) {
                std::fprintf(stderr, "Pack: %s while packing %s\n",
                             read_error ? "read error" : "write error (disk full?)",
                             entry.name.c_str());
                failed = true;
                break;
            }
        }
        if (!failed)
            writer.Finalize();
    }
    const bool close_failed = std::fclose(out.file) != 0;
    if (failed || out.failed || close_failed) {
        if (!failed)
            std::fprintf(stderr, "Pack: write error (disk full?)\n");
        fs::remove(target, ec);
        return 1;
    }
    std::printf("Pack: done, %.2f GiB -> %.2f GiB\n", total / 1073741824.0,
                out.written / 1073741824.0);
    return 0;
}

} // extern "C"
