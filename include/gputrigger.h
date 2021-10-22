
#ifndef GPUTRIGGER_GPUTRIGGER_H
#pragma once
#include <sanitizer.h>
#include <sanitizer_result.h>
#include <stdint.h>
//#include <atomic>
#include "cubin-hash-map.h"
#include "gpu-patch.h"
#include "stdbool.h"
#include <stdio.h>
#include <vector_types.h>

#ifdef __cplusplus
#define EXTERNC extern "C"
#else
#define EXTERNC
#endif

// @findhao: when you are going to change the bellowing functions, you have to change both of it in cplusplus and else.
#ifdef __cplusplus
extern "C" {
static void sanitizer_load_callback(CUcontext context, CUmodule module,
                                    const void *cubin, size_t cubin_size);
}
extern "C" {
static void sanitizer_unload_callback(const void *module, const void *cubin,
                                      size_t cubin_size);
}
extern "C" {
static void sanitizer_buffer_init(CUcontext context);
}
extern "C" {
static Sanitizer_StreamHandle sanitizer_priority_stream_get(CUcontext context);
}
extern "C" {
static void
sanitizer_kernel_launch_callback(uint64_t correlation_id, CUcontext context,
                                 Sanitizer_StreamHandle priority_stream,
                                 CUfunction function, dim3 grid_size,
                                 dim3 block_size, bool kernel_sampling);
}
extern "C" {
static Sanitizer_StreamHandle sanitizer_kernel_stream_get(CUcontext context);
}
extern "C" {
static void buffer_analyze(int32_t persistent_id, uint64_t correlation_id,
                           uint32_t cubin_id, uint32_t mod_id,
                           uint32_t gpu_patch_type, size_t record_size,
                           gpu_patch_buffer_t *gpu_patch_buffer_host,
                           gpu_patch_buffer_t *gpu_patch_buffer_device,
                           Sanitizer_StreamHandle priority_stream);
}
extern "C" {
static void sanitizer_kernel_analyze(int32_t persistent_id,
                                     uint64_t correlation_id, uint32_t cubin_id,
                                     uint32_t mod_id,
                                     Sanitizer_StreamHandle priority_stream,
                                     Sanitizer_StreamHandle kernel_stream,
                                     bool analysis_end);
}
extern "C" {
static void sanitizer_kernel_launch_sync(int32_t persistent_id,
                                         uint64_t correlation_id,
                                         CUcontext context, CUmodule module,
                                         CUfunction function,
                                         Sanitizer_StreamHandle priority_stream,
                                         Sanitizer_StreamHandle kernel_stream,
                                         dim3 grid_size, dim3 block_size);
}
extern "C" {
static void sanitizer_subscribe_callback(void *userdata,
                                         Sanitizer_CallbackDomain domain,
                                         Sanitizer_CallbackId cbid,
                                         const void *cbdata);
}
extern "C" {
static void output_dir_config(char *dir_name, char *suffix);
}

#else

static void sanitizer_load_callback(CUcontext context, CUmodule module,
                                    const void *cubin, size_t cubin_size);
static void sanitizer_unload_callback(const void *module, const void *cubin,
                                      size_t cubin_size);
static void sanitizer_buffer_init(CUcontext context);
static Sanitizer_StreamHandle sanitizer_priority_stream_get(CUcontext context);
static void
sanitizer_kernel_launch_callback(uint64_t correlation_id, CUcontext context,
                                 Sanitizer_StreamHandle priority_stream,
                                 CUfunction function, dim3 grid_size,
                                 dim3 block_size, bool kernel_sampling);
static Sanitizer_StreamHandle sanitizer_kernel_stream_get(CUcontext context);
static void buffer_analyze(int32_t persistent_id, uint64_t correlation_id,
                           uint32_t cubin_id, uint32_t mod_id,
                           uint32_t gpu_patch_type, size_t record_size,
                           gpu_patch_buffer_t *gpu_patch_buffer_host,
                           gpu_patch_buffer_t *gpu_patch_buffer_device,
                           Sanitizer_StreamHandle priority_stream);
static void sanitizer_kernel_analyze(int32_t persistent_id,
                                     uint64_t correlation_id, uint32_t cubin_id,
                                     uint32_t mod_id,
                                     Sanitizer_StreamHandle priority_stream,
                                     Sanitizer_StreamHandle kernel_stream,
                                     bool analysis_end);
static void sanitizer_kernel_launch_sync(int32_t persistent_id,
                                         uint64_t correlation_id,
                                         CUcontext context, CUmodule module,
                                         CUfunction function,
                                         Sanitizer_StreamHandle priority_stream,
                                         Sanitizer_StreamHandle kernel_stream,
                                         dim3 grid_size, dim3 block_size);
static void sanitizer_subscribe_callback(void *userdata,
                                         Sanitizer_CallbackDomain domain,
                                         Sanitizer_CallbackId cbid,
                                         const void *cbdata);
static void output_dir_config(char *dir_name, char *suffix);

#endif

EXTERNC void sanitizer_buffer_config(int gpu_patch_record_num,
                                     int buffer_pool_size);

EXTERNC void sanitizer_process_signal();

EXTERNC int sanitizer_callbacks_subscribe();

EXTERNC size_t sanitizer_gpu_patch_record_num_get();

EXTERNC size_t sanitizer_gpu_analysis_record_num_get();

EXTERNC void sanitizer_process_init();

EXTERNC int sanitizer_buffer_pool_size_get();

EXTERNC void sanitizer_stop_flag_set();

EXTERNC void sanitizer_stop_flag_unset();

EXTERNC void sanitizer_value_pattern_analysis_enable();
EXTERNC void sanitizer_callbacks_unsubscribe();
EXTERNC void sanitizer_device_flush();
EXTERNC void sanitizer_device_flush_now();
EXTERNC void sanitizer_device_shutdown();
EXTERNC void sanitizer_buffer_config(int gpu_patch_record_num,
                                     int buffer_pool_size);

#define SANITIZER_API_DEBUG 1
#if SANITIZER_API_DEBUG
#define PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define PRINT(...)
#endif

#define PRINT_ERR(...) fprintf(stderr, __VA_ARGS__)

#define GPUTRIGGER_GPUTRIGGER_H

#endif // GPUTRIGGER_GPUTRIGGER_H
