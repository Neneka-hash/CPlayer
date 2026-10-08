/*
 * tag_cache.h - Persistent on-disk tag metadata cache.
 *
 * Metadata is loaded lazily (see playlist.h), so scrolling a 100k-entry
 * playlist would otherwise parse 100k audio headers. This cache keeps the
 * parsed results on disk keyed by (path hash, file size), so a repeat visit
 * -- or a restart -- costs one small record read instead of file parsing.
 *
 * Thread-safety: tag_cache_lookup/store are called from the metadata worker
 * thread and from the UI thread (synchronous fills), so all entry points are
 * guarded by an internal critical section.
 */
#ifndef TAG_CACHE_H
#define TAG_CACHE_H

#include <stdint.h>
#include "tag_reader.h"

/* Open (or create) the cache file and load its index. Idempotent. */
void tag_cache_open(void);

/* Flush and close the cache. */
void tag_cache_close(void);

/* Look up metadata for a file. On a hit, `out` is filled with heap strings
 * the caller owns (release with tag_info_free). Returns 1 on hit. */
int  tag_cache_lookup(uint64_t path_hash, uint64_t file_size, TagInfo *out);

/* Append a freshly parsed record. */
void tag_cache_store(uint64_t path_hash, uint64_t file_size, const TagInfo *ti);

#endif /* TAG_CACHE_H */
