
#ifndef GPUTRIGGER_GPUTRIGGER_H
#pragma once
#include <stdint.h>
#include <sanitizer.h>
#include <sanitizer_result.h>
//#include <atomic>
#include "gpu-patch.h"
#include "template_call.h"
#include "stdbool.h"
#include <vector_types.h>
#include <stdio.h>
#include <stdio.h>




void sanitizer_buffer_config(int gpu_patch_record_num, int buffer_pool_size);

static void sanitizer_load_callback(CUcontext context, CUmodule module, const void *cubin, size_t cubin_size);
static void sanitizer_unload_callback (const void *module, const void *cubin,size_t cubin_size);

static void sanitizer_buffer_init(CUcontext context);
static Sanitizer_StreamHandle sanitizer_priority_stream_get(CUcontext context);

static void
sanitizer_kernel_launch_callback(uint64_t correlation_id, CUcontext context, Sanitizer_StreamHandle priority_stream,
                                 CUfunction function, dim3 grid_size, dim3 block_size, bool kernel_sampling);
static Sanitizer_StreamHandle sanitizer_kernel_stream_get(CUcontext context);
static void buffer_analyze(int32_t persistent_id, uint64_t correlation_id, uint32_t cubin_id, uint32_t mod_id,
                           uint32_t gpu_patch_type, size_t record_size, gpu_patch_buffer_t *gpu_patch_buffer_host,
                           gpu_patch_buffer_t *gpu_patch_buffer_device, Sanitizer_StreamHandle priority_stream);
static void sanitizer_kernel_analyze(int32_t persistent_id, uint64_t correlation_id, uint32_t cubin_id, uint32_t mod_id,
                                     Sanitizer_StreamHandle priority_stream, Sanitizer_StreamHandle kernel_stream,
                                     bool analysis_end);

void sanitizer_process_signal();
static void
sanitizer_kernel_launch_sync(int32_t persistent_id, uint64_t correlation_id, CUcontext context, CUmodule module,
                             CUfunction function, Sanitizer_StreamHandle priority_stream,
                             Sanitizer_StreamHandle kernel_stream, dim3 grid_size, dim3 block_size);
static void sanitizer_subscribe_callback(void *userdata, Sanitizer_CallbackDomain domain, Sanitizer_CallbackId cbid,
                                         const void *cbdata);

int sanitizer_callbacks_subscribe();


#define GPUTRIGGER_GPUTRIGGER_H

#endif //GPUTRIGGER_GPUTRIGGER_H
