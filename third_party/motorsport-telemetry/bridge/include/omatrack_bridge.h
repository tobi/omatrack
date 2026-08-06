#ifndef OMATRACK_BRIDGE_H
#define OMATRACK_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void* omatrack_open(const char* path);
void omatrack_close(void* handle);
const char* omatrack_last_error(void);
const char* omatrack_format(void* handle);
int omatrack_media_time_offset_ns(void* handle, int64_t* out);
size_t omatrack_channel_count(void* handle);
const char* omatrack_channel_name(void* handle, size_t index);
const char* omatrack_channel_unit(void* handle, size_t index);
uint32_t omatrack_channel_type_code(void* handle, size_t index);
uint64_t omatrack_channel_duration_ns(void* handle, size_t index);
uint64_t omatrack_channel_sample_count(void* handle, size_t index);
size_t omatrack_channel_chunk_count(void* handle, size_t index);
uint64_t omatrack_chunk_period_ns(void* handle, size_t index, size_t chunk);
uint64_t omatrack_chunk_time_base_ns(void* handle, size_t index, size_t chunk);
uint64_t omatrack_chunk_sample_count(void* handle, size_t index, size_t chunk);
size_t omatrack_decode_range(void* handle, size_t index, size_t chunk, uint64_t start,
                       size_t count, double* out);
size_t omatrack_channel_decode_all(void* handle, size_t index, double* out,
                             size_t capacity);
int omatrack_sample_at(void* handle, size_t index, uint64_t time_ns, bool linear,
                 double* out);
uint64_t omatrack_sample_time_ns(void* handle, size_t index, size_t chunk,
                           uint64_t local);

#ifdef __cplusplus
}
#endif

#endif
