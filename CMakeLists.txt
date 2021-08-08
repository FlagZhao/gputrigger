cmake_minimum_required(VERSION 3.17)
project(gputrigger)


find_package(CUDAToolkit REQUIRED)


include(CheckLanguage)
check_language(CUDA)
if (NOT CMAKE_CUDA_COMPILER OR NOT DEFINED $ENV{CUDACXX})
    if (${CUDAToolkit_NVCC_EXECUTABLE})
        set(ENV{CUDACXX} ${CUDAToolkit_NVCC_EXECUTABLE})
    elseif (UNIX AND EXISTS "/usr/local/cuda/bin/nvcc")
        set(ENV{CUDACXX} "/usr/local/cuda/bin/nvcc")
        message(WARNING "CMAKE_CUDA_COMPILER guessed: " $ENV{CUDACXX} "\n"
                "Please fix your cuda installation: https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html#mandatory-post")
    endif ()
endif ()

message(WARNING " check cuda" $ENV{CUDACXX} )

enable_language(CUDA)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

set(SANITIZER_PATH ${CUDAToolkit_TARGET_DIR}/compute-sanitizer)
set(redshow_path  /home/findhao/d/gvprof_new/redshow)


include_directories(${SANITIZER_PATH}/include/ ${CUDAToolkit_TARGET_DIR}/include/ ${redshow_path}/include ${redshow_path}/include/common ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/lib/prof-lean)
message(INFO ${SANITIZER_PATH} ${CUDAToolkit_NVCC_EXECUTABLE})

set(CMAKE_CXX_STANDARD 17)
set(SOURCE_FILES ${CMAKE_SOURCE_DIR}/gpu-patch.cu)
link_libraries(-lsanitizer-public)
link_libraries(-lpthread)
link_directories(/home/findhao/d/gvprof_new/redshow/lib/)
link_libraries(-lredshow)
aux_source_directory(${CMAKE_SOURCE_DIR}/lib/prof-lean DIR_PROF_LEAN)
add_link_options(-L${SANITIZER_PATH}  -fPIC -shared)
add_custom_target(gputrigger_patch.fatbin ALL $ENV{CUDACXX} ${CMAKE_CUDA_FLAGS} --fatbin --keep-device-functions -Xptxas --compile-as-tools-patch -g  -I${SANITIZER_PATH}/include/ -I${CUDAToolkit_TARGET_DIR}/include/ -c ${SOURCE_FILES} )
set_property(TARGET gputrigger_patch.fatbin PROPERTY CUDA_ARCHITECTURES 75)
file(GLOB gputrigger_SRC CONFIGURE_DEPENDS "*.h" "*.cpp" "*.c")

add_library(gputrigger SHARED ${gputrigger_SRC})
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(gputrigger PUBLIC Threads::Threads ${DIR_PROF_LEAN})