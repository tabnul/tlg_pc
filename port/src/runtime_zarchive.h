/* Read-only ZArchive (.zar) game images for the guest file system (issue #72).
 *
 * ZArchive (https://github.com/Exzap/ZArchive, MIT-0, by Exzap) stores the game folder at its
 * root in 64 KiB zstd blocks with random access. The reader is C++ and lives in the GPU library
 * (port/gpu/shim/zarchive_bridge.cpp); this small C API lets the runtime mount a .zar as /app0.
 * Design and API follow @R-V-L's ZArchive build of SpartanPort (R-V-L/gow3_pc, linux-v3.0.0-zar1).
 *
 * Nodes are opaque handles; ZARCHIVE_NONE means "not found". Every function is thread-safe: the
 * file tree is immutable, and reads go through a small pool of readers so streaming threads do
 * not queue behind one decompression lock. */
#ifndef TLG_RUNTIME_ZARCHIVE_H
#define TLG_RUNTIME_ZARCHIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZArchive ZArchive;

#define ZARCHIVE_NONE (-1)

/* Opens a .zar; NULL when missing or not a ZArchive (zarchive_error() says why). */
ZArchive *zarchive_open(const char *path);
void zarchive_close(ZArchive *archive);
/* Bytes of the .zar file itself. */
uint64_t zarchive_size(const ZArchive *archive);
/* Why the last open in this thread failed, or "". */
const char *zarchive_error(void);

/* Node of a path inside the archive ("sce_sys/param.sfo", "" = root); case-insensitive. */
int64_t zarchive_find(ZArchive *archive, const char *path);
int zarchive_is_file(ZArchive *archive, int64_t node);
uint64_t zarchive_entry_size(ZArchive *archive, int64_t node);
/* Child `index` of a directory node: name (NUL-terminated, truncated) and kind; 0 past the end. */
int zarchive_child(ZArchive *archive, int64_t node, uint32_t index, char *name, size_t size,
                   int *is_file);
/* Up to length bytes at offset; bytes read (short only at the end of the file). */
uint64_t zarchive_read(ZArchive *archive, int64_t node, uint64_t offset, void *buffer,
                       uint64_t length);

/* Counters since the last call (then reset): bytes handed to the game and time in those reads
 * (disk + unpack + cache), bytes read from disk and time, bytes unpacked and time. */
typedef struct ZArchiveStats {
    uint64_t served_bytes, read_ns, disk_bytes, disk_ns, unpack_bytes, unpack_ns;
} ZArchiveStats;
void zarchive_take_stats(ZArchiveStats *out);

/* Packs folder (its contents at the archive root) into out_path. Progress lines go to stdout.
 * 0 on success; on failure the partial output is removed and the reason printed to stderr. */
int zarchive_pack(const char *folder, const char *out_path);

#ifdef __cplusplus
}
#endif

#endif
