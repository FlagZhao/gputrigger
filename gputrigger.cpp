
#include "gputrigger.h"
#include "gpu-patch.h"
#include "map.h"
#include <filesystem>
#include <atomic>
#define SANITIZER_API_DEBUG 1
#if SANITIZER_API_DEBUG
#define PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define PRINT(...)
#endif

#include <sys/types.h>
#include <unistd.h>
#include <sys/syscall.h>

#define GPUPUNK_SANITIZER_CALL(fn, args...) wrapped_call(fn, #fn, ##args)
constexpr size_t RECORD_SIZE = 1024;
static __thread gpu_cct_record_t * gpu_cct_records= nullptr;
static __thread gpu_buffer_t *d_buffer= nullptr;
static __thread gpu_buffer_t h_buffer;
using std::map;
using redshow::Map;
using std::tuple;
static Map<tuple<Sanitizer_StreamHandle, CUcontext>, uint64_t> context_hashmap;
static std::atomic_int64_t global_context_id = 0;



template<typename T, typename ...Args>
void wrapped_call(T *f, const char *fn, Args... args) {
    SanitizerResult status = f(std::forward<Args>(args)...);
    if (status != SANITIZER_SUCCESS) {
        const char *error_string;
        sanitizerGetResultString(status, &error_string);
        PRINT("Sanitizer result error: function %s failed with error %s\\n", fn, error_string);
    }
}

struct LaunchData {
    std::string functionName;
    MemoryAccessTracker *pTracker;
};

struct CallbackTracker {
    std::map<Sanitizer_StreamHandle, std::vector<LaunchData>> memoryTrackers;
};

static void
sanitizer_buffer_init(CUcontext context, Sanitizer_StreamHandle stream) {
    sanitizerAlloc(context, (void **)&gpu_cct_records, sizeof(gpu_cct_record_t)*RECORD_SIZE);
    sanitizerMemset(gpu_cct_records, 0, sizeof(gpu_cct_record_t) * RECORD_SIZE, stream);


    h_buffer.gpu_cct_records = gpu_cct_records;
    h_buffer.gpu_mem_access_buffer = nullptr;

    sanitizerAlloc(context, (void **) &d_buffer, sizeof(*d_buffer));
    sanitizerMemcpyHostToDeviceAsync(d_buffer, &h_buffer, sizeof(*d_buffer), stream);



}

void sanitizer_load_callback(CUcontext context, CUmodule module, const void *cubin, size_t cubin_size) {
    using std::filesystem::exists;
    using std::cout;
    using std::endl;
//    check patch file
    const char *env_FATBIN_PATCH = std::getenv("GPUPUNK_PATCH");
    if ((env_FATBIN_PATCH and not exists(env_FATBIN_PATCH)) or (not env_FATBIN_PATCH)) {
        cout << "GPUPUNK_PATCH does not exist";
        exit(-1);
    }
    cout << env_FATBIN_PATCH;
    PRINT("Patch CUBIN: \n");
    // Instrument user code!
    GPUPUNK_SANITIZER_CALL(sanitizerAddPatchesFromFile, env_FATBIN_PATCH, context);
//    GPUPUNK_SANITIZER_CALL(sanitizerPatchInstructions, SANITIZER_INSTRUCTION_GLOBAL_MEMORY_ACCESS, module,
//                           "sanitizer_global_memory_access_callback");
//    GPUPUNK_SANITIZER_CALL(sanitizerPatchInstructions, SANITIZER_INSTRUCTION_SHARED_MEMORY_ACCESS, module,
//                           "sanitizer_shared_memory_access_callback");
//    GPUPUNK_SANITIZER_CALL(sanitizerPatchInstructions, SANITIZER_INSTRUCTION_LOCAL_MEMORY_ACCESS, module,
//                           "sanitizer_local_memory_access_callback");
//@todo there are bugs
//    GPUPUNK_SANITIZER_CALL(sanitizerPatchInstructions, SANITIZER_INSTRUCTION_BLOCK_ENTER, module,
//                           "sanitizer_block_enter_callback");
//    GPUPUNK_SANITIZER_CALL(sanitizerPatchInstructions, SANITIZER_INSTRUCTION_BLOCK_EXIT, module,
//                           "sanitizer_block_exit_callback");
    GPUPUNK_SANITIZER_CALL(sanitizerPatchInstructions, SANITIZER_INSTRUCTION_CALL, module,
                           "sanitizer_instr_call_callback");
    GPUPUNK_SANITIZER_CALL(sanitizerPatchInstructions, SANITIZER_INSTRUCTION_RET, module,
                           "sanitizer_instr_ret_callback");
    GPUPUNK_SANITIZER_CALL(sanitizerPatchModule, module);


}

void gpupunk_memory_register_trigger(Sanitizer_ResourceMemoryData *md) {}

void gpupunk_memory_unregister_trigger(Sanitizer_ResourceMemoryData *md) {}

void gpupunk_kernel_begin(Sanitizer_LaunchData *md, gpu_buffer_t *buffer) {\
    using std::make_tuple;
    sanitizerSetCallbackData(md->function, d_buffer);
    context_hashmap[make_tuple(md->hStream, md->context)] = global_context_id++;
    PRINT("launch begin: %s\n", md->functionName);
}

void gpupunk_kernel_end(Sanitizer_LaunchData *md) {
    PRINT("launch end: %s\n", md->functionName);
}

void funA() {

}

void StreamSynchronized(
        gpu_buffer_t *buffer,
        Sanitizer_SynchronizeData* syncdata) {
    using std::cout, std::endl;
    sanitizerMemcpyDeviceToHost(&h_buffer, buffer, sizeof(h_buffer), syncdata->hStream);

    gpu_cct_record_t h_gpu_cct_records[RECORD_SIZE];
    sanitizerMemcpyDeviceToHost(h_gpu_cct_records, gpu_cct_records, sizeof(gpu_cct_record_t)*RECORD_SIZE, syncdata->hStream);
    cout<<"========"<<endl;
    for(int i =0; i < h_buffer.current_cct_index; i++){
        auto cct_record = h_gpu_cct_records[i];
        cout<<cct_record.pc<<'\t'<<cct_record.target_pc<<"\t"<<cct_record.sanitizer_flag<<"\t"<<cct_record.flag<<endl;
    }
    cout<<"========"<<endl;

//    MemoryAccessTracker hTracker = {0};
//
//    std::vector<LaunchData> &deviceTrackers = pCallbackTracker->memoryTrackers[stream];
//
//    for (auto &tracker : deviceTrackers) {
//        std::cout << "Kernel Launch: " << tracker.functionName << std::endl;
//
//        sanitizerMemcpyDeviceToHost(&hTracker, tracker.pTracker, sizeof(*tracker.pTracker), stream);
//
//        uint32_t numEntries = std::min(hTracker.currentEntry, hTracker.maxEntry);
//
//        std::cout << "  Memory accesses: " << numEntries << std::endl;
//
//        std::vector<MemoryAccess> accesses(numEntries);
//        sanitizerMemcpyDeviceToHost(accesses.data(), hTracker.accesses, sizeof(MemoryAccess) * numEntries, stream);
//
//        for (uint32_t i = 0; i < numEntries; ++i) {
//            MemoryAccess &access = accesses[i];
//
//            std::cout << "  [" << i << "] " << GetMemoryRWString(access.flags)
//                      << " access of " << GetMemoryTypeString(access.type)
//                      << " memory by thread (" << access.threadId.x
//                      << "," << access.threadId.y
//                      << "," << access.threadId.z
//                      << ") at address 0x" << std::hex << access.address << std::dec
//                      << " (size is " << access.accessSize << " bytes)" << std::endl;
//        }
//
//        sanitizerFree(context, hTracker.accesses);
//        sanitizerFree(context, tracker.pTracker);
//    }
//
//    deviceTrackers.clear();
}

void ContextSynchronized(CallbackTracker *pCallbackTracker, CUcontext context) {
//    for (auto &streamTracker : pCallbackTracker->memoryTrackers) {
//        StreamSynchronized(pCallbackTracker, context, streamTracker.first);
//    }
}


void MemoryTrackerCallback(
        void *userdata,
        Sanitizer_CallbackDomain domain,
        Sanitizer_CallbackId cbid,
        const void *cbdata) {
    using std::cout;
    using std::endl;

    auto *buffer = (gpu_buffer_t *) userdata;

    cout << "tracker\t" << domain << endl;

    static __thread CUcontext sanitizer_thread_context = nullptr;
    const Sanitizer_CallbackData *cbInfo = (Sanitizer_CallbackData *) cbdata;

    if (domain == SANITIZER_CB_DOMAIN_RESOURCE) {
        cout << "SANITIZER_CB_DOMAIN_RESOURCE resource " << cbid << endl;
        switch (cbid) {
            case SANITIZER_CBID_RESOURCE_MODULE_LOADED: {
                auto *md = (Sanitizer_ResourceModuleData *) cbdata;
                sanitizer_load_callback(md->context, md->module, md->pCubin, md->cubinSize);
                break;
            }
            case SANITIZER_CBID_RESOURCE_STREAM_CREATED: {
                PRINT("Stream %lld created", cbInfo->context);
                break;
            }
            case SANITIZER_CBID_RESOURCE_DEVICE_MEMORY_ALLOC: {
                Sanitizer_ResourceMemoryData *md = (Sanitizer_ResourceMemoryData *) cbdata;
                sanitizer_thread_context = md->context;
                gpupunk_memory_register_trigger(md);
                break;
            }
            case SANITIZER_CBID_RESOURCE_DEVICE_MEMORY_FREE: {
                Sanitizer_ResourceMemoryData *md = (Sanitizer_ResourceMemoryData *) cbdata;
                sanitizer_thread_context = md->context;
                gpupunk_memory_unregister_trigger(md);
                cout << "device memory free" << endl;
                break;
            }
            default:
                break;
        }
    } else if (domain == SANITIZER_CB_DOMAIN_LAUNCH) {
        Sanitizer_LaunchData *ld = (Sanitizer_LaunchData *) cbdata;
        sanitizer_thread_context = ld->context;
        pid_t x = syscall(__NR_gettid);

        cout << x << endl;
        if (cbid == SANITIZER_CBID_LAUNCH_BEGIN) {
//            cuStreamAddCallback(funA)
            sanitizer_buffer_init(ld->context, ld->hStream);
            gpupunk_kernel_begin(ld, buffer);

        } else if (cbid == SANITIZER_CBID_LAUNCH_END) {
            gpupunk_kernel_end(ld);
        }
    } else if (domain == SANITIZER_CB_DOMAIN_DRIVER_API) {
//        PRINT("driver api call");
//        if (cbid == SANITIZER_CBID_DRIVER_API_cuLaunch) {
//            cout << "SANITIZER_CBID_DRIVER_API_cuLaunch" << endl;
//        }
    } else if (domain == SANITIZER_CB_DOMAIN_MEMCPY) {
        Sanitizer_MemcpyData *md = (Sanitizer_MemcpyData *) cbdata;
        sanitizer_thread_context = md->srcContext;
        // Avoid memcpy to symbol without allocation
        // Let gpupunk update shadow memory
//        gpupunk_memcpy_register(persistent_id, correlation_id, src_host, md->srcAddress, dst_host, md->dstAddress, md->size);
    } else if (domain == SANITIZER_CB_DOMAIN_MEMSET) {
        Sanitizer_MemsetData *md = (Sanitizer_MemsetData *) cbdata;
        sanitizer_thread_context = md->context;
//    gpupunk_memset_register(persistent_id, correlation_id, md->address, md->value, md->width);
    } else if (domain == SANITIZER_CB_DOMAIN_SYNCHRONIZE) {
        switch (cbid) {
            case SANITIZER_CBID_SYNCHRONIZE_STREAM_SYNCHRONIZED:
            case SANITIZER_CBID_SYNCHRONIZE_CONTEXT_SYNCHRONIZED: {
                auto *pSyncData = (Sanitizer_SynchronizeData *) cbdata;
                StreamSynchronized(d_buffer, pSyncData);
                break;
            }
            default:
                break;
        }
    } else if (domain == SANITIZER_CB_DOMAIN_UVM) {
        cout << "uvm=======" << endl;
        cout << cbid << endl;
    }
}

int InitializeInjection() {
    Sanitizer_SubscriberHandle handle;
    CallbackTracker *tracker = new CallbackTracker();

    sanitizerSubscribe(&handle, MemoryTrackerCallback, tracker);
//    sanitizerEnableAllDomains(1, handle);
    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_LAUNCH);
    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_UVM);
    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_RESOURCE);
    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_MEMCPY);
    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_MEMSET);
    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_SYNCHRONIZE);
//    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_DRIVER_API);
//    sanitizerEnableDomain(1, handle, SANITIZER_CB_DOMAIN_RUNTIME_API);

    return 0;
}

int __global_initializer__ = InitializeInjection();
