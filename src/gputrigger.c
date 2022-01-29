// -*-Mode: C++;-*- // technically C99

// * BeginRiceCopyright *****************************************************
//
// --------------------------------------------------------------------------
// Part of HPCToolkit (hpctoolkit.org)
//
// Information about sources of support for research and development of
// HPCToolkit is at 'hpctoolkit.org' and in 'README.Acknowledgments'.
// --------------------------------------------------------------------------
//
// Copyright ((c)) 2002-2019, Rice University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//
// * Neither the name of Rice University (RICE) nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// This software is provided by RICE and contributors "as is" and any
// express or implied warranties, including, but not limited to, the
// implied warranties of merchantability and fitness for a particular
// purpose are disclaimed. In no event shall RICE or contributors be
// liable for any direct, indirect, incidental, special, exemplary, or
// consequential damages (including, but not limited to, procurement of
// substitute goods or services; loss of use, data, or profits; or
// business interruption) however caused and on any theory of liability,
// whether in contract, strict liability, or tort (including negligence
// or otherwise) arising in any way out of the use of this software, even
// if advised of the possibility of such damage.
//
// ******************************************************* EndRiceCopyright *

//***************************************************************************
//
// File:
//   sanitizer-api.c
//
// Purpose:
//   implementation of wrapper around NVIDIA's Sanitizer API
//
//***************************************************************************

#include "gputrigger.h"

#include "sanitizer-buffer-channel-set.h"
#include "sanitizer-buffer-channel.h"
#include "sanitizer-buffer.h"
#include "sanitizer-context-map.h"
//#include "cuda-api.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include <pthread.h>

#ifndef HPCRUN_STATIC_LINK
#include <dlfcn.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "cubin-id-map.h"
#include "cubin-symbols.h"

static __thread gpu_cct_record_t *gpu_cct_records = NULL;
static __thread bool sanitizer_stop_flag = false;
static __thread uint32_t sanitizer_thread_id_self = (1 << 30);
static __thread int32_t sanitizer_thread_id_local = 0;
static __thread CUcontext sanitizer_thread_context = NULL;

static atomic_uint sanitizer_thread_id = ATOMIC_VAR_INIT(0);
static atomic_uint sanitizer_process_thread_counter = ATOMIC_VAR_INIT(0);
static atomic_bool sanitizer_process_awake_flag = ATOMIC_VAR_INIT(0);
static atomic_bool sanitizer_process_stop_flag = ATOMIC_VAR_INIT(0);
static atomic_int sanitizer_persistant_id = ATOMIC_VAR_INIT(0x08000000);
static atomic_int sanitizer_host_op_id = ATOMIC_VAR_INIT(0x20000000);

// Host buffers are per-thread
static __thread gpu_patch_buffer_t *sanitizer_gpu_patch_buffer_host = NULL;
static __thread gpu_patch_buffer_t *sanitizer_gpu_patch_buffer_addr_read_host =
    NULL;
static __thread gpu_patch_buffer_t *sanitizer_gpu_patch_buffer_addr_write_host =
    NULL;
static __thread gpu_patch_aux_address_dict_t
    *sanitizer_gpu_patch_aux_addr_dict_host = NULL;
// Reset and device buffers are per-context
static __thread gpu_patch_buffer_t *sanitizer_gpu_patch_buffer_reset = NULL;
static __thread gpu_patch_buffer_t *sanitizer_gpu_patch_buffer_addr_read_reset =
    NULL;
static __thread gpu_patch_buffer_t
    *sanitizer_gpu_patch_buffer_addr_write_reset = NULL;
static __thread gpu_patch_aux_address_dict_t
    *sanitizer_gpu_patch_aux_addr_dict_reset = NULL;
static __thread gpu_patch_buffer_t *sanitizer_gpu_patch_buffer_device = NULL;
static __thread gpu_patch_buffer_t
    *sanitizer_gpu_patch_buffer_addr_read_device = NULL;
static __thread gpu_patch_buffer_t
    *sanitizer_gpu_patch_buffer_addr_write_device = NULL;
static __thread gpu_patch_aux_address_dict_t
    *sanitizer_gpu_patch_aux_addr_dict_device = NULL;

// only subscribed by the main thread
static Sanitizer_SubscriberHandle sanitizer_subscriber_handle;

typedef void (*sanitizer_error_callback_t)(const char *type, const char *fn,
                                           const char *error_string);

static void sanitizer_error_callback_dummy(const char *type, const char *fn,
                                           const char *error_string);

static sanitizer_error_callback_t sanitizer_error_callback =
    sanitizer_error_callback_dummy;

// Patch info (GPU)
static size_t sanitizer_gpu_patch_record_num = 0;
static size_t sanitizer_gpu_patch_record_size = 0;
static uint32_t sanitizer_gpu_patch_type = GPU_PATCH_TYPE_DEFAULT;
// Configurable variables
static int sanitizer_buffer_pool_size = 0;
static int sanitizer_pc_views = 0;
static int sanitizer_mem_views = 0;

// Analysis info (GPU)
static int sanitizer_gpu_analysis_record_num = 0;
static size_t sanitizer_gpu_analysis_record_size = 0;
static uint32_t sanitizer_gpu_analysis_blocks = 0;
static uint32_t sanitizer_gpu_analysis_type = GPU_PATCH_TYPE_ADDRESS_ANALYSIS;
static bool sanitizer_read_trace_ignore = false;
static bool sanitizer_data_flow_hash = false;
// default type
static redshow_analysis_type_t GPUPUNK_ANALYSIS_MODE = REDSHOW_ANALYSIS_MEMORY_PAGE;
// CPU async
static bool sanitizer_analysis_async = false;
typedef struct
{
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} sanitizer_thread_t;
// Single background process thread, can be extended
static sanitizer_thread_t sanitizer_thread;

//#define DYN_FN_NAME(f) f ## _fn
#define SANITIZER_FN_NAME(f) f

#define SANITIZER_FN(fn, args) \
  static SanitizerResult(*SANITIZER_FN_NAME(fn)) args

#define GPUTRIGGER_SANITIZER_CALL(fn, args)              \
  {                                                      \
    SanitizerResult status = SANITIZER_FN_NAME(fn) args; \
    if (status != SANITIZER_SUCCESS) {                   \
      sanitizer_error_report(status, #fn);               \
    }                                                    \
  }

#define GPUTRIGGER_SANITIZER_CALL_NO_CHECK(fn, args) \
  {                                                  \
    SANITIZER_FN_NAME(fn)                            \
    args;                                            \
  }



static const int DEFAULT_GPU_PATCH_RECORD_NUM = 16 * 1024;
static const int DEFAULT_BUFFER_POOL_SIZE = 500;
static const int DEFAULT_DEVICE_BUFFER_SIZE = 1024 * 1024 * 8;

//----------------------------------------------------------
// sanitizer function pointers for late binding
//----------------------------------------------------------

static void sanitizer_error_callback_dummy  // __attribute__((unused))
    (const char *type, const char *fn, const char *error_string) {
  PRINT("Sanitizer-> %s: function %s failed with error %s\n", type, fn,
        error_string);
  exit(-1);
}

static void sanitizer_error_report(SanitizerResult error, const char *fn) {
  const char *error_string;
  SANITIZER_FN_NAME(sanitizerGetResultString)
  (error, &error_string);
  sanitizer_error_callback("Sanitizer result error", fn, error_string);
}

void sanitizer_buffer_config(int gpu_patch_record_num, int buffer_pool_size) {
  sanitizer_gpu_patch_record_num = gpu_patch_record_num;
  sanitizer_gpu_analysis_record_num =
      gpu_patch_record_num * GPU_PATCH_WARP_SIZE * 4;
  sanitizer_buffer_pool_size = buffer_pool_size;
}

static void sanitizer_load_callback(CUcontext context, CUmodule module,
                                    const void *cubin, size_t cubin_size) {
  //    check patch file existence and permission
  const char *env_PATCH_PATH = getenv("GPUPATCH_PATH");
      PRINT("The GPUPATCH_PATH is %s\n", env_PATCH_PATH);
  // Create file name
  char env_FATBIN_PATCH[PATH_MAX];
  size_t used = 0;
  //    @todo fix path
  used += sprintf(&env_FATBIN_PATCH[0], "%s", env_PATCH_PATH);
  if (env_PATCH_PATH) {
    if (access(env_FATBIN_PATCH, R_OK) != 0) {
      PRINT_ERR("ERROR: Can not access GPUPATCH_PATH\n");
      exit(-1);
    }
    //@FindHao: todo, add more modes
    used += sprintf(&env_FATBIN_PATCH[used], "%s", "/lib/gpu-patch.fatbin");
    if (access(env_FATBIN_PATCH, R_OK) != 0) {
      PRINT_ERR("ERROR: Can not access FATBIN_PATCH %s\n", env_FATBIN_PATCH);
      exit(-1);
    }
  } else {
    PRINT_ERR("ERROR: no GPUPATCH_PATH env specified.");
    exit(-1);
  }

  hpctoolkit_cumod_st_t *cumod = (hpctoolkit_cumod_st_t *)module;
  uint32_t cubin_id = cumod->cubin_id;
  uint32_t mod_id = cumod->mod_id;

  // Compute hash for cubin and store it into a map
  cubin_hash_map_entry_t *cubin_hash_entry = cubin_hash_map_lookup(cubin_id);
  unsigned char *hash;
  unsigned int hash_len;
  if (cubin_hash_entry == NULL) {
    cubin_hash_map_insert(cubin_id, cubin, cubin_size);
    cubin_hash_entry = cubin_hash_map_lookup(cubin_id);
  }
  hash = cubin_hash_map_entry_hash_get(cubin_hash_entry, &hash_len);

  // Create file name
  char file_name[PATH_MAX];
  size_t i;
  used = 0;
  //    @todo fix path
  used += sprintf(&file_name[used], "%s", "./");
  used += sprintf(&file_name[used], "%s", "/cubins/");
  mkdir(file_name, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  for (i = 0; i < hash_len; ++i) {
    used += sprintf(&file_name[used], "%02x", hash[i]);
  }
  used += sprintf(&file_name[used], "%s", ".cubin");
  PRINT("Sanitizer-> cubin_id %d hash %s\n", cubin_id, file_name);
  // @FindHao todo: check the gvprof's code here.
  uint32_t hpctoolkit_module_id = 0;
  PRINT("Sanitizer-> <cubin_id %d, mod_id %d> -> hpctoolkit_module_id %d\n",
        cubin_id, mod_id, hpctoolkit_module_id);
  // Compute elf vector
  Elf_SymbolVector *elf_vector = computeCubinFunctionOffsets(cubin, cubin_size);

  // Register cubin module
  cubin_id_map_insert(cubin_id, hpctoolkit_module_id, elf_vector);

  // Query cubin function offsets
  uint64_t *addrs =
      (uint64_t *)calloc(1, sizeof(uint64_t) * elf_vector->nsymbols);
  for (i = 0; i < elf_vector->nsymbols; ++i) {
    addrs[i] = 0;
    if (elf_vector->symbols[i] != 0) {
      uint64_t pc;
      uint64_t size;
      // do not check error
      GPUTRIGGER_SANITIZER_CALL_NO_CHECK(
          sanitizerGetFunctionPcAndSize,
          (module, elf_vector->names[i], &pc, &size));
      addrs[i] = pc;
    }
  }
  REDSHOW_FN(redshow_cubin_cache_register, (cubin_id, mod_id, elf_vector->nsymbols, addrs,
                                         file_name));
  PRINT("Sanitizer-> Context %p Patch CUBIN: \n", context);
  // @FindHao todo: add different patch mode
  // Instrument user code!
  GPUTRIGGER_SANITIZER_CALL(sanitizerAddPatchesFromFile,
                            (env_FATBIN_PATCH, context));

  GPUTRIGGER_SANITIZER_CALL(sanitizerPatchInstructions,
                            (SANITIZER_INSTRUCTION_GLOBAL_MEMORY_ACCESS, module,
                             "sanitizer_global_memory_access_callback"));
  GPUTRIGGER_SANITIZER_CALL(sanitizerPatchInstructions,
                            (SANITIZER_INSTRUCTION_SHARED_MEMORY_ACCESS, module,
                             "sanitizer_shared_memory_access_callback"));
  GPUTRIGGER_SANITIZER_CALL(sanitizerPatchInstructions,
                            (SANITIZER_INSTRUCTION_LOCAL_MEMORY_ACCESS, module,
                             "sanitizer_local_memory_access_callback"));
  GPUTRIGGER_SANITIZER_CALL(sanitizerPatchInstructions,
                            (SANITIZER_INSTRUCTION_BLOCK_ENTER, module,
                             "sanitizer_block_enter_callback"));
  GPUTRIGGER_SANITIZER_CALL(sanitizerPatchInstructions,
                            (SANITIZER_INSTRUCTION_BLOCK_EXIT, module,
                             "sanitizer_block_exit_callback"));
  // GPUTRIGGER_SANITIZER_CALL(
  //     sanitizerPatchInstructions,
  //     (SANITIZER_INSTRUCTION_CALL, module, "sanitizer_instr_call_callback"));
  // GPUTRIGGER_SANITIZER_CALL(
  //     sanitizerPatchInstructions,
  //     (SANITIZER_INSTRUCTION_RET, module, "sanitizer_instr_ret_callback"));
  GPUTRIGGER_SANITIZER_CALL(sanitizerPatchModule, (module));
  sanitizer_buffer_init(context);
}

static void sanitizer_unload_callback(const void *module, const void *cubin,
                                      size_t cubin_size) {
  //    hpctoolkit_cumod_st_t *cumod = (hpctoolkit_cumod_st_t *) module;
  //    cuda_unload_callback(cumod->cubin_id);
  // We cannot unregister cubins
  // redshow_cubin_unregister(cumod->cubin_id, cumod->mod_id);
}

static void sanitizer_buffer_init(CUcontext context) {
  if (sanitizer_gpu_patch_buffer_device != NULL) {
    // All entries have been initialized
    return;
  }

  // Get cached entry
  sanitizer_context_map_entry_t *entry = sanitizer_context_map_init(context);
  Sanitizer_StreamHandle priority_stream =
      sanitizer_priority_stream_get(context);

  sanitizer_gpu_patch_buffer_device =
      sanitizer_context_map_entry_buffer_device_get(entry);
  sanitizer_gpu_patch_buffer_addr_read_device =
      sanitizer_context_map_entry_buffer_addr_read_device_get(entry);
  sanitizer_gpu_patch_buffer_addr_write_device =
      sanitizer_context_map_entry_buffer_addr_write_device_get(entry);
  sanitizer_gpu_patch_aux_addr_dict_device =
      sanitizer_context_map_entry_aux_addr_dict_device_get(entry);

  sanitizer_gpu_patch_buffer_reset =
      sanitizer_context_map_entry_buffer_reset_get(entry);
  sanitizer_gpu_patch_buffer_addr_read_reset =
      sanitizer_context_map_entry_buffer_addr_read_reset_get(entry);
  sanitizer_gpu_patch_buffer_addr_write_reset =
      sanitizer_context_map_entry_buffer_addr_write_reset_get(entry);
  sanitizer_gpu_patch_aux_addr_dict_reset =
      sanitizer_context_map_entry_aux_addr_dict_reset_get(entry);

  if (sanitizer_gpu_patch_buffer_device == NULL) {
    // Allocated buffer
    void *gpu_patch_records = NULL;
    // gpu_patch_buffer
    GPUTRIGGER_SANITIZER_CALL(sanitizerAlloc,
                              (context,
                               (void **)(&(sanitizer_gpu_patch_buffer_device)),
                               sizeof(gpu_patch_buffer_t)));
    GPUTRIGGER_SANITIZER_CALL(sanitizerMemset,
                              (sanitizer_gpu_patch_buffer_device, 0,
                               sizeof(gpu_patch_buffer_t), priority_stream));

    PRINT("Sanitizer-> Allocate gpu_patch_buffer %p, size %zu\n",
          sanitizer_gpu_patch_buffer_device, sizeof(gpu_patch_buffer_t));

    // gpu_patch_buffer_t->records
    GPUTRIGGER_SANITIZER_CALL(
        sanitizerAlloc,
        (context, &gpu_patch_records,
         sanitizer_gpu_patch_record_num * sanitizer_gpu_patch_record_size));
    GPUTRIGGER_SANITIZER_CALL(
        sanitizerMemset,
        (gpu_patch_records, 0,
         sanitizer_gpu_patch_record_num * sanitizer_gpu_patch_record_size,
         priority_stream));
    PRINT("Sanitizer-> Allocate gpu_patch_records %p, size %zu\n",
          gpu_patch_records,
          sanitizer_gpu_patch_record_num * sanitizer_gpu_patch_record_size);

    // Allocate reset record
    sanitizer_gpu_patch_buffer_reset =
        (gpu_patch_buffer_t *)calloc(1, sizeof(gpu_patch_buffer_t));
    sanitizer_gpu_patch_buffer_reset->full = 0;
    sanitizer_gpu_patch_buffer_reset->analysis = 0;
    sanitizer_gpu_patch_buffer_reset->head_index = 0;
    sanitizer_gpu_patch_buffer_reset->tail_index = 0;
    sanitizer_gpu_patch_buffer_reset->size = sanitizer_gpu_patch_record_num;
    sanitizer_gpu_patch_buffer_reset->num_threads = 0;
    sanitizer_gpu_patch_buffer_reset->type = sanitizer_gpu_patch_type;
    sanitizer_gpu_patch_buffer_reset->flags = GPU_PATCH_NONE;
    sanitizer_gpu_patch_buffer_reset->aux = NULL;
    sanitizer_gpu_patch_buffer_reset->records = gpu_patch_records;

    if (sanitizer_gpu_analysis_blocks != 0) {
      sanitizer_gpu_patch_buffer_reset->flags |= GPU_PATCH_ANALYSIS;
    }

    if (sanitizer_read_trace_ignore) {
      void *gpu_patch_aux = NULL;
      // Use a dict to filter read trace
      GPUTRIGGER_SANITIZER_CALL(sanitizerAlloc,
                                (context, (void **)(&(gpu_patch_aux)),
                                 sizeof(gpu_patch_aux_address_dict_t)));
      GPUTRIGGER_SANITIZER_CALL(sanitizerMemset,
                                (gpu_patch_aux, 0,
                                 sizeof(gpu_patch_aux_address_dict_t),
                                 priority_stream));

      PRINT("Sanitizer-> Allocate gpu_patch_aux %p, size %zu\n", gpu_patch_aux,
            sizeof(gpu_patch_aux_address_dict_t));

      // Update map
      sanitizer_gpu_patch_buffer_reset->aux = gpu_patch_aux;
      sanitizer_context_map_aux_addr_dict_device_update(
          context, (gpu_patch_aux_address_dict_t *)gpu_patch_aux);
    }

    GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyHostToDeviceAsync,
                              (sanitizer_gpu_patch_buffer_device,
                               sanitizer_gpu_patch_buffer_reset,
                               sizeof(gpu_patch_buffer_t), priority_stream));
    // Update map
    sanitizer_context_map_buffer_device_update(
        context, sanitizer_gpu_patch_buffer_device);
    sanitizer_context_map_buffer_reset_update(context,
                                              sanitizer_gpu_patch_buffer_reset);

    if (sanitizer_gpu_analysis_blocks != 0) {
      // Read
      GPUTRIGGER_SANITIZER_CALL(
          sanitizerAlloc,
          (context, (void **)(&(sanitizer_gpu_patch_buffer_addr_read_device)),
           sizeof(gpu_patch_buffer_t)));
      GPUTRIGGER_SANITIZER_CALL(sanitizerMemset,
                                (sanitizer_gpu_patch_buffer_addr_read_device, 0,
                                 sizeof(gpu_patch_buffer_t), priority_stream));
      PRINT(
          "Sanitizer-> Allocate sanitizer_gpu_patch_buffer_addr_read_device "
          "%p, size %zu\n",
          sanitizer_gpu_patch_buffer_addr_read_device,
          sizeof(gpu_patch_buffer_t));

      // gpu_patch_buffer_t->records
      GPUTRIGGER_SANITIZER_CALL(sanitizerAlloc,
                                (context, &gpu_patch_records,
                                 sanitizer_gpu_analysis_record_num *
                                     sanitizer_gpu_analysis_record_size));
      GPUTRIGGER_SANITIZER_CALL(sanitizerMemset,
                                (gpu_patch_records, 0,
                                 sanitizer_gpu_analysis_record_num *
                                     sanitizer_gpu_analysis_record_size,
                                 priority_stream));
      PRINT("Sanitizer-> Allocate gpu_patch_records %p, size %zu\n",
            gpu_patch_records,
            sanitizer_gpu_analysis_record_num *
                sanitizer_gpu_analysis_record_size);

      sanitizer_gpu_patch_buffer_addr_read_reset =
          (gpu_patch_buffer_t *)calloc(1, sizeof(gpu_patch_buffer_t));
      sanitizer_gpu_patch_buffer_addr_read_reset->full = 0;
      sanitizer_gpu_patch_buffer_addr_read_reset->analysis = 0;
      sanitizer_gpu_patch_buffer_addr_read_reset->head_index = 0;
      sanitizer_gpu_patch_buffer_addr_read_reset->tail_index = 0;
      sanitizer_gpu_patch_buffer_addr_read_reset->size =
          sanitizer_gpu_analysis_record_num;
      sanitizer_gpu_patch_buffer_addr_read_reset->num_threads =
          GPU_PATCH_ANALYSIS_THREADS;
      sanitizer_gpu_patch_buffer_addr_read_reset->type =
          GPU_PATCH_TYPE_ADDRESS_ANALYSIS;
      sanitizer_gpu_patch_buffer_addr_read_reset->flags =
          GPU_PATCH_READ | GPU_PATCH_ANALYSIS;
      sanitizer_gpu_patch_buffer_addr_read_reset->aux = NULL;
      sanitizer_gpu_patch_buffer_addr_read_reset->records = gpu_patch_records;

      GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyHostToDeviceAsync,
                                (sanitizer_gpu_patch_buffer_addr_read_device,
                                 sanitizer_gpu_patch_buffer_addr_read_reset,
                                 sizeof(gpu_patch_buffer_t), priority_stream));

      // Write
      GPUTRIGGER_SANITIZER_CALL(
          sanitizerAlloc,
          (context, (void **)(&(sanitizer_gpu_patch_buffer_addr_write_device)),
           sizeof(gpu_patch_buffer_t)));
      GPUTRIGGER_SANITIZER_CALL(
          sanitizerMemset, (sanitizer_gpu_patch_buffer_addr_write_device, 0,
                            sizeof(gpu_patch_buffer_t), priority_stream));
      PRINT(
          "Sanitizer-> Allocate sanitizer_gpu_patch_buffer_addr_write_device "
          "%p, size %zu\n",
          sanitizer_gpu_patch_buffer_addr_write_device,
          sizeof(gpu_patch_buffer_t));

      // gpu_patch_buffer_t->records
      GPUTRIGGER_SANITIZER_CALL(sanitizerAlloc,
                                (context, &gpu_patch_records,
                                 sanitizer_gpu_analysis_record_num *
                                     sanitizer_gpu_analysis_record_size));
      GPUTRIGGER_SANITIZER_CALL(sanitizerMemset,
                                (gpu_patch_records, 0,
                                 sanitizer_gpu_analysis_record_num *
                                     sanitizer_gpu_analysis_record_size,
                                 priority_stream));
      PRINT("Sanitizer-> Allocate gpu_patch_records %p, size %zu\n",
            gpu_patch_records,
            sanitizer_gpu_analysis_record_num *
                sanitizer_gpu_analysis_record_size);

      sanitizer_gpu_patch_buffer_addr_write_reset =
          (gpu_patch_buffer_t *)calloc(1, sizeof(gpu_patch_buffer_t));
      sanitizer_gpu_patch_buffer_addr_write_reset->full = 0;
      sanitizer_gpu_patch_buffer_addr_write_reset->analysis = 0;
      sanitizer_gpu_patch_buffer_addr_write_reset->head_index = 0;
      sanitizer_gpu_patch_buffer_addr_write_reset->tail_index = 0;
      sanitizer_gpu_patch_buffer_addr_write_reset->size =
          sanitizer_gpu_analysis_record_num;
      sanitizer_gpu_patch_buffer_addr_write_reset->num_threads =
          GPU_PATCH_ANALYSIS_THREADS;
      sanitizer_gpu_patch_buffer_addr_write_reset->type =
          GPU_PATCH_TYPE_ADDRESS_ANALYSIS;
      sanitizer_gpu_patch_buffer_addr_write_reset->flags =
          GPU_PATCH_WRITE | GPU_PATCH_ANALYSIS;
      sanitizer_gpu_patch_buffer_addr_write_reset->aux = NULL;
      sanitizer_gpu_patch_buffer_addr_write_reset->records = gpu_patch_records;

      GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyHostToDeviceAsync,
                                (sanitizer_gpu_patch_buffer_addr_write_device,
                                 sanitizer_gpu_patch_buffer_addr_write_reset,
                                 sizeof(gpu_patch_buffer_t), priority_stream));

      // Update map
      sanitizer_context_map_buffer_addr_read_device_update(
          context, sanitizer_gpu_patch_buffer_addr_read_device);
      sanitizer_context_map_buffer_addr_write_device_update(
          context, sanitizer_gpu_patch_buffer_addr_write_device);
      sanitizer_context_map_buffer_addr_read_reset_update(
          context, sanitizer_gpu_patch_buffer_addr_read_reset);
      sanitizer_context_map_buffer_addr_write_reset_update(
          context, sanitizer_gpu_patch_buffer_addr_write_reset);
    }

    // Ensure data copy is done
    GPUTRIGGER_SANITIZER_CALL(sanitizerStreamSynchronize, (priority_stream));
  }
}

static Sanitizer_StreamHandle sanitizer_priority_stream_get(CUcontext context) {
  sanitizer_context_map_entry_t *entry = sanitizer_context_map_init(context);

  Sanitizer_StreamHandle priority_stream_handle =
      sanitizer_context_map_entry_priority_stream_handle_get(entry);

  if (priority_stream_handle == NULL) {
    // First time
    // Update priority stream
    CUstream priority_stream =
        sanitizer_context_map_entry_priority_stream_get(entry);
    GPUTRIGGER_SANITIZER_CALL(
        sanitizerGetStreamHandle,
        (context, priority_stream, &priority_stream_handle));

    sanitizer_context_map_priority_stream_handle_update(context,
                                                        priority_stream_handle);
  }

  return priority_stream_handle;
}

static void
sanitizer_kernel_launch_callback(uint64_t correlation_id, CUcontext context,
                                 Sanitizer_StreamHandle priority_stream,
                                 CUfunction function, dim3 grid_size,
                                 dim3 block_size, bool kernel_sampling) {
  int grid_dim = grid_size.x * grid_size.y * grid_size.z;
  int block_dim = block_size.x * block_size.y * block_size.z;
  //    @todo sampling frequency
  int block_sampling_frequency = 1;
  int block_sampling_offset =
      kernel_sampling ? rand() % grid_dim % block_sampling_frequency : 0;

  PRINT("Sanitizer-> kernel sampling %d\n", kernel_sampling);
  PRINT("Sanitizer-> sampling offset %d\n", block_sampling_offset);
  PRINT("Sanitizer-> sampling frequency %d\n", block_sampling_frequency);

  // Get cached entries, already locked
  sanitizer_buffer_init(context);

  // reset buffer
  sanitizer_gpu_patch_buffer_reset->num_threads = grid_dim * block_dim;
  sanitizer_gpu_patch_buffer_reset->block_sampling_frequency =
      block_sampling_frequency;
  sanitizer_gpu_patch_buffer_reset->block_sampling_offset =
      block_sampling_offset;

  GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyHostToDeviceAsync,
                            (sanitizer_gpu_patch_buffer_device,
                             sanitizer_gpu_patch_buffer_reset,
                             sizeof(gpu_patch_buffer_t), priority_stream));

  if (sanitizer_gpu_analysis_blocks != 0) {
    GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyHostToDeviceAsync,
                              (sanitizer_gpu_patch_buffer_addr_read_device,
                               sanitizer_gpu_patch_buffer_addr_read_reset,
                               sizeof(gpu_patch_buffer_t), priority_stream));
    GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyHostToDeviceAsync,
                              (sanitizer_gpu_patch_buffer_addr_write_device,
                               sanitizer_gpu_patch_buffer_addr_write_reset,
                               sizeof(gpu_patch_buffer_t), priority_stream));
  }

  if (sanitizer_read_trace_ignore) {
    if (sanitizer_gpu_patch_aux_addr_dict_host == NULL) {
      sanitizer_gpu_patch_aux_addr_dict_host =
          (gpu_patch_aux_address_dict_t *)calloc(
              1, sizeof(gpu_patch_aux_address_dict_t));
    }
    memset(sanitizer_gpu_patch_aux_addr_dict_host->hit, 0,
           sizeof(uint32_t) * GPU_PATCH_ADDRESS_DICT_SIZE);

    // Get memory ranges from redshow
    uint64_t limit = GPU_PATCH_ADDRESS_DICT_SIZE;
    REDSHOW_FN(redshow_memory_ranges_get, (correlation_id, limit,
                              sanitizer_gpu_patch_aux_addr_dict_host->start_end,
                              &sanitizer_gpu_patch_aux_addr_dict_host->size));
    // Copy
    GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyHostToDeviceAsync,
                              (sanitizer_gpu_patch_buffer_reset->aux,
                               sanitizer_gpu_patch_aux_addr_dict_host,
                               sizeof(gpu_patch_aux_address_dict_t),
                               priority_stream));
  }

  GPUTRIGGER_SANITIZER_CALL(sanitizerSetCallbackData,
                            (function, sanitizer_gpu_patch_buffer_device));

  GPUTRIGGER_SANITIZER_CALL(sanitizerStreamSynchronize, (priority_stream));
}

static Sanitizer_StreamHandle sanitizer_kernel_stream_get(CUcontext context) {
  sanitizer_context_map_entry_t *entry = sanitizer_context_map_init(context);

  Sanitizer_StreamHandle kernel_stream_handle =
      sanitizer_context_map_entry_kernel_stream_handle_get(entry);

  if (kernel_stream_handle == NULL) {
    // First time
    // Update kernel stream
    CUstream kernel_stream =
        sanitizer_context_map_entry_kernel_stream_get(entry);
    GPUTRIGGER_SANITIZER_CALL(sanitizerGetStreamHandle,
                              (context, kernel_stream, &kernel_stream_handle));

    sanitizer_context_map_kernel_stream_handle_update(context,
                                                      kernel_stream_handle);
  }

  return kernel_stream_handle;
}

static void buffer_analyze(int32_t persistent_id, uint64_t correlation_id,
                           uint32_t cubin_id, uint32_t mod_id,
                           uint32_t gpu_patch_type, size_t record_size,
                           gpu_patch_buffer_t *gpu_patch_buffer_host,
                           gpu_patch_buffer_t *gpu_patch_buffer_device,
                           Sanitizer_StreamHandle priority_stream) {
  sanitizer_buffer_t *sanitizer_buffer = sanitizer_buffer_channel_produce(
      sanitizer_thread_id_local, cubin_id, mod_id, persistent_id,
      correlation_id, gpu_patch_type, sanitizer_gpu_patch_record_num,
      sanitizer_analysis_async);
  gpu_patch_buffer_t *gpu_patch_buffer =
      sanitizer_buffer_entry_gpu_patch_buffer_get(sanitizer_buffer);

  // If sync mode and not enough buffer, empty current buffer
  if (gpu_patch_buffer == NULL && !sanitizer_analysis_async) {
    sanitizer_buffer_channel_t *channel =
        sanitizer_buffer_channel_get(gpu_patch_type);
    sanitizer_buffer_channel_consume(channel);
    // Get it again
    sanitizer_buffer = sanitizer_buffer_channel_produce(
        sanitizer_thread_id_local, cubin_id, mod_id, persistent_id,
        correlation_id, gpu_patch_type, sanitizer_gpu_patch_record_num,
        sanitizer_analysis_async);
    gpu_patch_buffer =
        sanitizer_buffer_entry_gpu_patch_buffer_get(sanitizer_buffer);
  }
  // Move host buffer to a cache
  memcpy(gpu_patch_buffer, gpu_patch_buffer_host,
         offsetof(gpu_patch_buffer_t, records));

  // Copy all records to the cache
  size_t num_records = gpu_patch_buffer_host->head_index;
  void *gpu_patch_record_device = gpu_patch_buffer_host->records;
  if (num_records != 0) {
    GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyDeviceToHost,
                              (gpu_patch_buffer->records,
                               gpu_patch_record_device,
                               record_size * num_records, priority_stream));
    PRINT("Sanitizer-> copy num_records %zu\n", num_records);
  }

  // Tell kernel to continue
  // Do not need to sync stream.
  // The function will return once the pageable buffer has been copied to the
  // staging memory. for DMA transfer to device memory, but the DMA to final
  // destination may not have completed. Only copy the first field because other
  // fields are being updated by the GPU.
  gpu_patch_buffer_host->full = 0;
  GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyHostToDeviceAsync,
                            (gpu_patch_buffer_device, gpu_patch_buffer_host,
                             sizeof(gpu_patch_buffer_host->full),
                             priority_stream));

  sanitizer_buffer_channel_push(sanitizer_buffer, gpu_patch_type);
}

// only works when does data flow analysis
static void sanitizer_kernel_analyze(int32_t persistent_id,
                                     uint64_t correlation_id, uint32_t cubin_id,
                                     uint32_t mod_id,
                                     Sanitizer_StreamHandle priority_stream,
                                     Sanitizer_StreamHandle kernel_stream,
                                     bool analysis_end) {
  if (analysis_end) {
    GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyDeviceToHost,
                              (sanitizer_gpu_patch_buffer_addr_read_host,
                               sanitizer_gpu_patch_buffer_addr_read_device,
                               sizeof(gpu_patch_buffer_t), priority_stream));

    GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyDeviceToHost,
                              (sanitizer_gpu_patch_buffer_addr_write_host,
                               sanitizer_gpu_patch_buffer_addr_write_device,
                               sizeof(gpu_patch_buffer_t), priority_stream));

    while (sanitizer_gpu_patch_buffer_addr_read_host->num_threads != 0) {
      if (sanitizer_gpu_patch_buffer_addr_read_host->full != 0) {
        PRINT("Sanitizer-> read analysis address\n");
        buffer_analyze(
            persistent_id, correlation_id, cubin_id, mod_id,
            GPU_PATCH_TYPE_ADDRESS_ANALYSIS, sanitizer_gpu_analysis_record_size,
            sanitizer_gpu_patch_buffer_addr_read_host,
            sanitizer_gpu_patch_buffer_addr_read_device, priority_stream);
      }

      if (sanitizer_gpu_patch_buffer_addr_write_host->full != 0) {
        PRINT("Sanitizer-> write analysis address\n");
        buffer_analyze(
            persistent_id, correlation_id, cubin_id, mod_id,
            GPU_PATCH_TYPE_ADDRESS_ANALYSIS, sanitizer_gpu_analysis_record_size,
            sanitizer_gpu_patch_buffer_addr_write_host,
            sanitizer_gpu_patch_buffer_addr_write_device, priority_stream);
      }

      GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyDeviceToHost,
                                (sanitizer_gpu_patch_buffer_addr_read_host,
                                 sanitizer_gpu_patch_buffer_addr_read_device,
                                 sizeof(gpu_patch_buffer_t), priority_stream));

      GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyDeviceToHost,
                                (sanitizer_gpu_patch_buffer_addr_write_host,
                                 sanitizer_gpu_patch_buffer_addr_write_device,
                                 sizeof(gpu_patch_buffer_t), priority_stream));
    }

    // To ensure analysis is done
    GPUTRIGGER_SANITIZER_CALL(sanitizerStreamSynchronize, (kernel_stream));

    // Last analysis
    PRINT("Sanitizer-> read analysis address\n");
    buffer_analyze(
        persistent_id, correlation_id, cubin_id, mod_id,
        GPU_PATCH_TYPE_ADDRESS_ANALYSIS, sanitizer_gpu_analysis_record_size,
        sanitizer_gpu_patch_buffer_addr_read_host,
        sanitizer_gpu_patch_buffer_addr_read_device, priority_stream);

    PRINT("Sanitizer-> write analysis address\n");
    buffer_analyze(
        persistent_id, correlation_id, cubin_id, mod_id,
        GPU_PATCH_TYPE_ADDRESS_ANALYSIS, sanitizer_gpu_analysis_record_size,
        sanitizer_gpu_patch_buffer_addr_write_host,
        sanitizer_gpu_patch_buffer_addr_write_device, priority_stream);

    // Do not enter later code
    PRINT("Sanitizer-> analysis gpu done\n");

    return;
  }

  GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyDeviceToHost,
                            (sanitizer_gpu_patch_buffer_addr_read_host,
                             sanitizer_gpu_patch_buffer_addr_read_device,
                             sizeof(gpu_patch_buffer_t), priority_stream));

  if (sanitizer_gpu_patch_buffer_addr_read_host->full != 0) {
    PRINT("Sanitizer-> read analysis address\n");
    buffer_analyze(
        persistent_id, correlation_id, cubin_id, mod_id,
        GPU_PATCH_TYPE_ADDRESS_ANALYSIS, sanitizer_gpu_analysis_record_size,
        sanitizer_gpu_patch_buffer_addr_read_host,
        sanitizer_gpu_patch_buffer_addr_read_device, priority_stream);

    PRINT("Sanitizer-> analysis gpu in process\n");
  }

  GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyDeviceToHost,
                            (sanitizer_gpu_patch_buffer_addr_write_host,
                             sanitizer_gpu_patch_buffer_addr_write_device,
                             sizeof(gpu_patch_buffer_t), priority_stream));

  if (sanitizer_gpu_patch_buffer_addr_write_host->full != 0) {
    PRINT("Sanitizer-> write analysis address\n");
    buffer_analyze(
        persistent_id, correlation_id, cubin_id, mod_id,
        GPU_PATCH_TYPE_ADDRESS_ANALYSIS, sanitizer_gpu_analysis_record_size,
        sanitizer_gpu_patch_buffer_addr_write_host,
        sanitizer_gpu_patch_buffer_addr_write_device, priority_stream);

    PRINT("Sanitizer-> analysis gpu in process\n");
  }
}

//******************************************************************************
// asynchronous process thread
//******************************************************************************

void sanitizer_process_signal() {
  pthread_cond_t *cond = &(sanitizer_thread.cond);
  pthread_mutex_t *mutex = &(sanitizer_thread.mutex);

  pthread_mutex_lock(mutex);

  atomic_store(&sanitizer_process_awake_flag, true);

  pthread_cond_signal(cond);

  pthread_mutex_unlock(mutex);
}

static void sanitizer_process_await() {
  pthread_cond_t *cond = &(sanitizer_thread.cond);
  pthread_mutex_t *mutex = &(sanitizer_thread.mutex);

  pthread_mutex_lock(mutex);

  while (!atomic_load(&sanitizer_process_awake_flag)) {
    pthread_cond_wait(cond, mutex);
  }

  atomic_store(&sanitizer_process_awake_flag, false);

  pthread_mutex_unlock(mutex);
}

static void *sanitizer_process_thread(void *arg) {
  pthread_cond_t *cond = &(sanitizer_thread.cond);
  pthread_mutex_t *mutex = &(sanitizer_thread.mutex);
  while (!atomic_load(&sanitizer_process_stop_flag)) {
    REDSHOW_FN(redshow_analysis_begin, ());
    sanitizer_buffer_channel_set_consume();
    REDSHOW_FN(redshow_analysis_end, ());
    sanitizer_process_await();
  }
  // Last records
  sanitizer_buffer_channel_set_consume();

  // Create thread data
  //    thread_data_t *td = NULL;
  //    int id = sanitizer_thread_id_self;
  //    hpcrun_threadMgr_non_compact_data_get(id, NULL, &td);
  //    hpcrun_set_thread_data(td);

  atomic_fetch_add(&sanitizer_process_thread_counter, -1);

  pthread_mutex_destroy(mutex);
  pthread_cond_destroy(cond);

  return NULL;
}

size_t sanitizer_gpu_patch_record_num_get() {
  return sanitizer_gpu_patch_record_num;
}

size_t sanitizer_gpu_analysis_record_num_get() {
  return sanitizer_gpu_analysis_record_num;
}

int sanitizer_buffer_pool_size_get() { return sanitizer_buffer_pool_size; }

void sanitizer_stop_flag_set() { sanitizer_stop_flag = true; }

void sanitizer_stop_flag_unset() { sanitizer_stop_flag = false; }

static void sanitizer_kernel_launch_sync(int32_t persistent_id,
                                         uint64_t correlation_id,
                                         CUcontext context, CUmodule module,
                                         CUfunction function,
                                         Sanitizer_StreamHandle priority_stream,
                                         Sanitizer_StreamHandle kernel_stream,
                                         dim3 grid_size, dim3 block_size) {
  // Look up module id
  hpctoolkit_cumod_st_t *cumod = (hpctoolkit_cumod_st_t *)module;
  uint32_t cubin_id = cumod->cubin_id;
  uint32_t mod_id = cumod->mod_id;

  // TODO(Keren): correlate metrics with api_node

  //    int block_sampling_frequency = sanitizer_block_sampling_frequency_get();
  int block_sampling_frequency = 0;
  int grid_dim = grid_size.x * grid_size.y * grid_size.z;
  int block_dim = block_size.x * block_size.y * block_size.z;
  uint64_t num_threads = grid_dim * block_dim;
  size_t num_left_threads = 0;

  // If block sampling is set
  if (block_sampling_frequency != 0) {
    // Uniform sampling
    int sampling_offset =
        sanitizer_gpu_patch_buffer_reset->block_sampling_offset;
    int mod_blocks = grid_dim % block_sampling_frequency;
    int sampling_blocks = 0;
    if (mod_blocks == 0) {
      sampling_blocks = (grid_dim - 1) / block_sampling_frequency + 1;
    } else {
      sampling_blocks = (grid_dim - 1) / block_sampling_frequency +
                        (sampling_offset >= mod_blocks ? 0 : 1);
    }
    num_left_threads = num_threads - sampling_blocks * block_dim;
  }

  // Init a buffer on host
  if (sanitizer_gpu_patch_buffer_host == NULL) {
    sanitizer_gpu_patch_buffer_host =
        (gpu_patch_buffer_t *)calloc(1, sizeof(gpu_patch_buffer_t));

    if (sanitizer_gpu_analysis_blocks != 0) {
      sanitizer_gpu_patch_buffer_addr_read_host =
          (gpu_patch_buffer_t *)calloc(1, sizeof(gpu_patch_buffer_t));
      sanitizer_gpu_patch_buffer_addr_write_host =
          (gpu_patch_buffer_t *)calloc(1, sizeof(gpu_patch_buffer_t));
    }
  }

  // Reserve for debugging correctness
  PRINT("head_index %u, tail_index %u, num_left_threads %lu\n",
        sanitizer_gpu_patch_buffer_host->head_index,
        sanitizer_gpu_patch_buffer_host->tail_index, num_threads);

  while (true) {
    // Copy buffer
    GPUTRIGGER_SANITIZER_CALL(sanitizerMemcpyDeviceToHost,
                              (sanitizer_gpu_patch_buffer_host,
                               sanitizer_gpu_patch_buffer_device,
                               sizeof(gpu_patch_buffer_t), priority_stream));

    size_t num_records = sanitizer_gpu_patch_buffer_host->head_index;

    // Reserve for debugging correctness
    // PRINT("head_index %u, tail_index %u, num_left_threads %u expected %zu\n",
    //       sanitizer_gpu_patch_buffer_host->head_index,
    //       sanitizer_gpu_patch_buffer_host->tail_index,
    //       sanitizer_gpu_patch_buffer_host->num_threads, num_left_threads);

    if (sanitizer_gpu_analysis_blocks != 0) {
      sanitizer_kernel_analyze(persistent_id, correlation_id, cubin_id, mod_id,
                               priority_stream, kernel_stream, false);
    }

    // Wait until the buffer is full or the kernel is finished
    if (!(sanitizer_gpu_patch_buffer_host->num_threads == num_left_threads ||
          sanitizer_gpu_patch_buffer_host->full)) {
      continue;
    }

    // Reserve for debugging correctness
    PRINT("num_records %zu\n", num_records);

    if (sanitizer_gpu_analysis_blocks == 0) {
      buffer_analyze(persistent_id, correlation_id, cubin_id, mod_id,
                     sanitizer_gpu_patch_type, sanitizer_gpu_patch_record_size,
                     sanitizer_gpu_patch_buffer_host,
                     sanitizer_gpu_patch_buffer_device, priority_stream);

      PRINT("Sanitizer-> analysis cpu in process\n");
    }

    // Awake background thread
    if (sanitizer_analysis_async) {
      // If multiple application threads are created, it might miss a signal,
      // but we finally still process all the records
      sanitizer_process_signal();
    }

    // Finish all the threads
    if (sanitizer_gpu_patch_buffer_host->num_threads == num_left_threads) {
      break;
    }
  }

  if (sanitizer_gpu_analysis_blocks != 0) {
    // Kernel is done
    sanitizer_kernel_analyze(persistent_id, correlation_id, cubin_id, mod_id,
                             priority_stream, kernel_stream, true);
  }

  // To ensure previous copies are done
  GPUTRIGGER_SANITIZER_CALL(sanitizerStreamSynchronize, (priority_stream));

  if (!sanitizer_analysis_async) {
    // Empty current buffer
    sanitizer_buffer_channel_t *channel =
        sanitizer_buffer_channel_get(sanitizer_gpu_patch_type);
    sanitizer_buffer_channel_consume(channel);

    if (sanitizer_gpu_analysis_blocks != 0) {
      channel = sanitizer_buffer_channel_get(sanitizer_gpu_analysis_type);
      sanitizer_buffer_channel_consume(channel);
    }
  }
}

void sanitizer_callbacks_unsubscribe() {
  //    sanitizer_correlation_callback = 0;
  GPUTRIGGER_SANITIZER_CALL(sanitizerUnsubscribe,
                            (sanitizer_subscriber_handle));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerSubscribe,
      (&sanitizer_subscriber_handle, sanitizer_subscribe_callback, NULL));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (0, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_LAUNCH));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (0, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_UVM));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (0, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_RESOURCE));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (0, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_MEMCPY));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (0, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_MEMSET));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (0, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_DRIVER_API));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (0, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_RUNTIME_API));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (0, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_SYNCHRONIZE));
}
volatile int GPUPUNK_DEBUG = 0;

static void sanitizer_subscribe_callback(void *userdata,
                                         Sanitizer_CallbackDomain domain,
                                         Sanitizer_CallbackId cbid,
                                         const void *cbdata) {
  if (cuda_api_internal()) {
    return;
  }
  if (!sanitizer_stop_flag) {
    sanitizer_thread_id_local = atomic_fetch_add(&sanitizer_thread_id, 1);
    sanitizer_stop_flag = true;
  }

  // pid_t pid = getpid();
  // PRINT("sanitizer_subscribe_callback PID: %d\n", pid);

  if (domain == SANITIZER_CB_DOMAIN_DRIVER_API) {
    Sanitizer_CallbackData *cb = (Sanitizer_CallbackData *)cbdata;
    if (cb->callbackSite == SANITIZER_API_ENTER) {
      // Reserve for debug
      //            PRINT("Sanitizer-> Thread %u enter context %p function
      //            %s\n", sanitizer_thread_id_local, cb->context,
      //            cb->functionName);
      sanitizer_context_map_context_lock(cb->context,
                                         sanitizer_thread_id_local);
      sanitizer_thread_context = cb->context;
    } else {
      // Reserve for debug
      //            PRINT("Sanitizer-> Thread %u exit context %p function %s\n",
      //            sanitizer_thread_id_local, cb->context, cb->functionName);
      // Caution, do not use cb->context. When cuCtxGetCurrent is used,
      // cb->context != sanitizer_thread_context
      sanitizer_context_map_context_unlock(sanitizer_thread_context,
                                           sanitizer_thread_id_local);
      sanitizer_thread_context = NULL;
    }
    return;
  }

  // XXX(keren): assume single cpu thread per stream
  if (domain == SANITIZER_CB_DOMAIN_RESOURCE) {
    switch (cbid) {
      case SANITIZER_CBID_RESOURCE_MODULE_LOADED: {
        // single thread
        Sanitizer_ResourceModuleData *md = (Sanitizer_ResourceModuleData *)cbdata;
        sanitizer_load_callback(md->context, md->module, md->pCubin,
                                md->cubinSize);
        break;
      }
      case SANITIZER_CBID_RESOURCE_MODULE_UNLOAD_STARTING: {
        // single thread
        Sanitizer_ResourceModuleData *md = (Sanitizer_ResourceModuleData *)cbdata;
        sanitizer_unload_callback(md->module, md->pCubin, md->cubinSize);
        break;
      }
      case SANITIZER_CBID_RESOURCE_STREAM_CREATED: {
        // single thread
        PRINT("Sanitizer-> Stream create starting\n");
        break;
      }
      case SANITIZER_CBID_RESOURCE_STREAM_DESTROY_STARTING: {
        // single thread
        // TODO
        PRINT("Sanitizer-> Stream destroy starting\n");
        break;
      }
      case SANITIZER_CBID_RESOURCE_CONTEXT_CREATION_FINISHED: {
        break;
      }
      case SANITIZER_CBID_RESOURCE_CONTEXT_DESTROY_STARTING: {
        // single thread
        // TODO
        PRINT("Sanitizer-> Context destroy starting\n");
        break;
      }
      case SANITIZER_CBID_RESOURCE_HOST_MEMORY_ALLOC: {
        Sanitizer_ResourceMemoryData *md = (Sanitizer_ResourceMemoryData *)cbdata;
        PRINT("Sanitizer-> Allocate memory address %p, size %zu\n",
              (void *)md->address, md->size);
        break;
      }
      case SANITIZER_CBID_RESOURCE_HOST_MEMORY_FREE: {
        Sanitizer_ResourceMemoryData *md = (Sanitizer_ResourceMemoryData *)cbdata;
        PRINT("Sanitizer-> Free memory address %p, size %zu\n",
              (void *)md->address, md->size);
        break;
      }
      case SANITIZER_CBID_RESOURCE_DEVICE_MEMORY_ALLOC: {
        Sanitizer_ResourceMemoryData *md = (Sanitizer_ResourceMemoryData *)cbdata;
        int32_t memory_id = atomic_fetch_add(&sanitizer_persistant_id, 1);
        uint64_t host_op_id = atomic_fetch_add(&sanitizer_host_op_id, 1);
        REDSHOW_FN(redshow_memory_register, (memory_id, host_op_id, md->address,
                                md->address + md->size));
        PRINT("Sanitizer-> Allocate memory address %p, size %zu, op %lu, id %d\n",
              (void *)md->address, md->size, host_op_id, memory_id);
        break;
      }
      case SANITIZER_CBID_RESOURCE_DEVICE_MEMORY_FREE: {
        Sanitizer_ResourceMemoryData *md = (Sanitizer_ResourceMemoryData *)cbdata;
        uint64_t host_op_id = atomic_fetch_add(&sanitizer_host_op_id, 1);
        REDSHOW_FN(redshow_memory_unregister, (host_op_id, md->address,
                                  md->address + md->size));
        PRINT("Sanitizer-> Free memory address %p, size %zu, op %lu\n",
              (void *)md->address, md->size, host_op_id);
        break;
      }
      default: {
        break;
      }
    }
  } else if (domain == SANITIZER_CB_DOMAIN_LAUNCH) {
    Sanitizer_LaunchData *ld = (Sanitizer_LaunchData *)cbdata;
    static __thread dim3 grid_size = {0, 0, 0};
    static __thread dim3 block_size = {0, 0, 0};
    static __thread Sanitizer_StreamHandle priority_stream = NULL;
    static __thread Sanitizer_StreamHandle kernel_stream = NULL;
    static __thread bool kernel_sampling = true;
    static __thread uint64_t correlation_id = 0;
    static __thread int32_t persistent_id = 0;
    if (cbid == SANITIZER_CBID_LAUNCH_BEGIN) {
      // Use function list to filter functions
      // Get a place holder cct node
      correlation_id = atomic_fetch_add(&sanitizer_host_op_id, 1);
      // Look up persisitent id
      persistent_id = atomic_fetch_add(&sanitizer_persistant_id, 1);
      //            if (kernel_sampling)
      //                kernel_sampling = true;
      //                @todo sampling and op map init
      grid_size.x = ld->gridDim_x;
      grid_size.y = ld->gridDim_y;
      grid_size.z = ld->gridDim_z;
      block_size.x = ld->blockDim_x;
      block_size.y = ld->blockDim_y;
      block_size.z = ld->blockDim_z;
      PRINT(
          "Sanitizer-> Launch kernel %s <%d, %d, %d>:<%d, %d, %d>, op %lu, "
          "id %d, mod_id %u\n",
          ld->functionName, ld->gridDim_x, ld->gridDim_y, ld->gridDim_z,
          ld->blockDim_x, ld->blockDim_y, ld->blockDim_z, correlation_id,
          persistent_id, ((hpctoolkit_cumod_st_t *)ld->module)->mod_id);
      // thread-safe
      // Create a high priority stream for the context at the first time
      // TODO(Keren): change stream->hstream
      REDSHOW_FN(redshow_kernel_begin, (sanitizer_thread_id_local, persistent_id,
                           correlation_id));
      priority_stream = sanitizer_priority_stream_get(ld->context);
      sanitizer_kernel_launch_callback(correlation_id, ld->context,
                                       priority_stream, ld->function, grid_size,
                                       block_size, kernel_sampling);
      sanitizer_device_flush_now();
    } else if (cbid == SANITIZER_CBID_LAUNCH_AFTER_SYSCALL_SETUP) {
      //            @FindHao todo: fix this in the future
      //            if (sanitizer_gpu_analysis_blocks != 0 && kernel_sampling) {
      //                sanitizer_kernel_launch(ld->context);
      //            }
    } else if (cbid == SANITIZER_CBID_LAUNCH_END) {
      //            if (kernel_sampling) {
      PRINT("Sanitizer-> Sync kernel %s\n", ld->functionName);

      kernel_stream = sanitizer_kernel_stream_get(ld->context);

      sanitizer_kernel_launch_sync(persistent_id, correlation_id, ld->context,
                                   ld->module, ld->function, priority_stream,
                                   kernel_stream, grid_size, block_size);
      //            }

      // NOTICE: Need to synchronize this stream even when this kernel is not
      // sampled. TO prevent data is incorrectly copied in the next round
      GPUTRIGGER_SANITIZER_CALL(sanitizerStreamSynchronize, (ld->hStream));

      REDSHOW_FN(redshow_kernel_end, (sanitizer_thread_id_local, persistent_id,
                         correlation_id));

      //            kernel_sampling = true;

      PRINT("Sanitizer-> kernel %s done\n", ld->functionName);
    }
  } else if (domain == SANITIZER_CB_DOMAIN_MEMCPY) {
    Sanitizer_MemcpyData *md = (Sanitizer_MemcpyData *)cbdata;

    bool src_host = false;
    bool dst_host = false;

    if (md->direction == SANITIZER_MEMCPY_DIRECTION_HOST_TO_DEVICE) {
      src_host = true;
    } else if (md->direction == SANITIZER_MEMCPY_DIRECTION_HOST_TO_HOST) {
      src_host = true;
      dst_host = true;
    } else if (md->direction == SANITIZER_MEMCPY_DIRECTION_DEVICE_TO_HOST) {
      dst_host = true;
    }

    uint64_t correlation_id = atomic_fetch_add(&sanitizer_host_op_id, 1);
    int32_t persistent_id = atomic_fetch_add(&sanitizer_persistant_id, 1);
    PRINT(
        "Sanitizer-> Memcpy async %d direction %d from %p to %p, op %lu, id "
        "%d\n",
        md->isAsync, md->direction, (void *)md->srcAddress,
        (void *)md->dstAddress, correlation_id, persistent_id);

    // Avoid memcpy to symbol without allocation
    // Let redshow update shadow memory
    REDSHOW_FN(redshow_memcpy_register, (persistent_id, correlation_id, src_host,
                            md->srcAddress, dst_host, md->dstAddress, md->size));
  } else if (domain == SANITIZER_CB_DOMAIN_MEMSET) {
    Sanitizer_MemsetData *md = (Sanitizer_MemsetData *)cbdata;
    uint64_t correlation_id = atomic_fetch_add(&sanitizer_host_op_id, 1);
    int32_t persistent_id = atomic_fetch_add(&sanitizer_persistant_id, 1);
    REDSHOW_FN(redshow_memset_register, (persistent_id, correlation_id, md->address,
                            md->value, md->width));
  } else if (domain == SANITIZER_CB_DOMAIN_SYNCHRONIZE) {
    // TODO(Keren): sync data
    switch (cbid) {
      case SANITIZER_CBID_SYNCHRONIZE_STREAM_SYNCHRONIZED: {
        // @findhao: comment for api call in drcctprof
        // sanitizer_device_flush();
        // sanitizer_device_shutdown();
        sanitizer_device_flush_now();
        break;
      }
      default:
        break;
    }
  }
}

static void output_dir_config(char *dir_name, char *suffix) {
  size_t used = 0;
  used += sprintf(&dir_name[used], "%s", "./");
  used += sprintf(&dir_name[used], "%s", suffix);
  mkdir(dir_name, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
}

void sanitizer_value_pattern_analysis_enable() {
  REDSHOW_FN(redshow_analysis_enable, (REDSHOW_ANALYSIS_VALUE_PATTERN));
  char dir_name[PATH_MAX];
  output_dir_config(dir_name, "/value_pattern/");
  REDSHOW_FN(redshow_output_dir_config, (REDSHOW_ANALYSIS_VALUE_PATTERN, dir_name));
  sanitizer_gpu_patch_record_size = sizeof(gpu_patch_record_t);
}

void sanitizer_memory_page_analysis_enable() {
  REDSHOW_FN(redshow_analysis_enable, (REDSHOW_ANALYSIS_MEMORY_PAGE));
  char dir_name[PATH_MAX];
  output_dir_config(dir_name, "/memory_page/");
  REDSHOW_FN(redshow_output_dir_config, (REDSHOW_ANALYSIS_MEMORY_PAGE, dir_name));
  sanitizer_gpu_patch_record_size = sizeof(gpu_patch_record_t);
}

void sanitizer_device_flush() {
  if (sanitizer_stop_flag) {
    sanitizer_stop_flag_unset();
    if (sanitizer_analysis_async) {
      // Spin wait
      sanitizer_buffer_channel_flush(sanitizer_gpu_patch_type);
      if (sanitizer_gpu_analysis_blocks != 0) {
        sanitizer_buffer_channel_flush(sanitizer_gpu_analysis_type);
      }
      sanitizer_process_signal();
      while (sanitizer_buffer_channel_finish(sanitizer_gpu_patch_type) ==
             false) {
      }
      while (sanitizer_buffer_channel_finish(sanitizer_gpu_analysis_type) ==
             false) {
      }
    }
    // Attribute performance metrics to CCTs
    REDSHOW_FN(redshow_flush_thread, (sanitizer_thread_id_local));
  }
}

void sanitizer_device_flush_now() {
  if (GPUPUNK_ANALYSIS_MODE == REDSHOW_ANALYSIS_MEMORY_PAGE) {
    REDSHOW_FN(redshow_flush_now, (sanitizer_thread_id_local));
  }
}

void sanitizer_device_shutdown() {
  sanitizer_callbacks_unsubscribe();

  if (sanitizer_analysis_async) {
    atomic_store(&sanitizer_process_stop_flag, true);
    // Spin wait
    sanitizer_buffer_channel_flush(sanitizer_gpu_patch_type);
    sanitizer_process_signal();
    while (sanitizer_buffer_channel_finish(sanitizer_gpu_analysis_type) ==
           false) {
    }
  }

  // Attribute performance metrics to CCTs
  REDSHOW_FN(redshow_flush, ());

  while (atomic_load(&sanitizer_process_thread_counter))
    ;
}

void sanitizer_process_init() {
  // XXX(Keren): value flow analysis must be sync
  if (sanitizer_analysis_async) {
    pthread_t *thread = &(sanitizer_thread.thread);
    pthread_mutex_t *mutex = &(sanitizer_thread.mutex);
    pthread_cond_t *cond = &(sanitizer_thread.cond);
    atomic_fetch_add(&sanitizer_process_thread_counter, 1);

    pthread_mutex_init(mutex, NULL);
    pthread_cond_init(cond, NULL);
    pthread_create(thread, NULL, sanitizer_process_thread, NULL);
  }
}
// @findhao: comment for debug
// __attribute__((constructor)) int sanitizer_callbacks_subscribe() {
int sanitizer_callbacks_subscribe() {
  pid_t pid = getpid();
  PRINT("PID: %d\n", pid);

  // Get mode control from the env variable.
  const char *GPUPUNK_ANALYSIS_MODE_raw = getenv("GPUPUNK_ANALYSIS_MODE");
  if (GPUPUNK_ANALYSIS_MODE_raw) {
    char *tmp;
    GPUPUNK_ANALYSIS_MODE = strtol(GPUPUNK_ANALYSIS_MODE_raw, &tmp, 10);
  }
  switch (GPUPUNK_ANALYSIS_MODE) {
    case REDSHOW_ANALYSIS_VALUE_PATTERN:
      sanitizer_value_pattern_analysis_enable();
      break;
    case REDSHOW_ANALYSIS_MEMORY_PAGE:
    default:
      sanitizer_memory_page_analysis_enable();
      break;
  }
  PRINT("GPUTRIGGER -> Working on %d mode.\n", GPUPUNK_ANALYSIS_MODE);

  sanitizer_buffer_config(DEFAULT_GPU_PATCH_RECORD_NUM,
                          DEFAULT_BUFFER_POOL_SIZE);

  GPUTRIGGER_SANITIZER_CALL(
      sanitizerSubscribe,
      (&sanitizer_subscriber_handle, sanitizer_subscribe_callback, NULL));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (1, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_LAUNCH));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (1, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_UVM));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (1, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_RESOURCE));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (1, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_MEMCPY));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (1, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_MEMSET));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (1, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_DRIVER_API));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (1, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_RUNTIME_API));
  GPUTRIGGER_SANITIZER_CALL(
      sanitizerEnableDomain,
      (1, sanitizer_subscriber_handle, SANITIZER_CB_DOMAIN_SYNCHRONIZE));

  return 0;
}
// void __attribute__((weak))
// monitor_at_main(void) {
//   // __attribute__((constructor)) test() {
//   if (GPUPUNK_DEBUG == 0) {
//     const char *GPUPUNK_DEBUG_raw = getenv("GPUPUNK_DEBUG");
//     if (GPUPUNK_DEBUG_raw) {
//       char *tmp;
//       // set to 1
//       GPUPUNK_DEBUG = strtol(GPUPUNK_DEBUG_raw, &tmp, 10);
//     }
//     while (GPUPUNK_DEBUG) {
//     }
//   }
// }

void __attribute__((weak))
monitor_init_library(void) {
  PRINT("gputrigger-> start\n");
  if (cuda_bind()) {
    PRINT_ERR("gputrigger-> unable to bind to NVIDIA CUDA library%s\n", dlerror());
  }
  sanitizer_callbacks_subscribe();
}

// void *__attribute__((weak))
// monitor_init_process(int *argc, char **argv, void *data) {
//   int i;

//   PRINT("(default callback) parent = %d, argc = %d, argv = %p\n",
//         (int)getppid(), (argc != NULL) ? *argc : 0, argv);
//   return (data);
// }
void monitor_fini_process(int how, void *data) {
  sanitizer_device_flush();
  sanitizer_device_shutdown();
}

void monitor_fini_thread(void *data) {
  sanitizer_device_flush();
}
__attribute__((destructor)) void notify_exit() {
  PRINT("gputrigger-> exit\n");
}

__attribute__((constructor)) void notify_init() {
  const char *with_libmonitor_raw = getenv("W_LIBMONITOR");
  if (with_libmonitor_raw) {
    char *tmp;
    long long with_libmonitor = strtol(with_libmonitor_raw, &tmp, 10);
    if (with_libmonitor == 0){
      PRINT("gputrigger-> start\n");
      if (cuda_bind()) {
        PRINT_ERR("gputrigger-> unable to bind to NVIDIA CUDA library%s\n", dlerror());
      }
      sanitizer_callbacks_subscribe();
    }
  }
}