cmake_minimum_required(VERSION 3.10)
project(gputrigger)

include(CheckLanguage)
check_language(CUDA)

enable_language(CUDA)
#@findhao: for debug
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
            "Default build type: RelWithDebInfo" FORCE)
endif()


if(${CMAKE_VERSION} VERSION_LESS "3.17.0") 
    find_package(CUDA REQUIRED)
    set(CUDAToolkit_NVCC_EXECUTABLE ${CUDA_TOOLKIT_ROOT_DIR}/bin/nvcc)
    set(CUDAToolkit_TARGET_DIR ${CUDA_TOOLKIT_ROOT_DIR})
    if( ${CUDA_VERSION} LESS 11.2 )
        message(FATAL_ERROR "CUDAToolkit mush be newer or equal to 11.2. Your version is " ${CUDA_VERSION} )
    endif()

else()
    find_package(CUDAToolkit REQUIRED)
    if (NOT CMAKE_CUDA_COMPILER)
        if (NOT ${CUDAToolkit_NVCC_EXECUTABLE} AND UNIX AND EXISTS "/usr/local/cuda/bin/nvcc")
            set(CUDAToolkit_NVCC_EXECUTABLE "/usr/local/cuda/bin/nvcc")
            message(WARNING "CMAKE_CUDA_COMPILER guessed: " ${CUDAToolkit_NVCC_EXECUTABLE} "\n"
                    "Please fix your cuda installation: https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html#mandatory-post")
        endif ()
    endif ()
    if( ${CUDAToolkit_VERSION} LESS 11.2 )
        message(FATAL_ERROR "CUDAToolkit mush be newer or equal to 11.2. Your version is " ${CUDAToolkit_VERSION} )
    endif()
endif()

# find_package(MbedTLS)
# find_package(LibElf)

set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(SANITIZER_PATH ${CUDAToolkit_TARGET_DIR}/compute-sanitizer)
# @findhao: drcctprof can not handle pthread
# set(THREADS_PREFER_PTHREAD_FLAG ON)
#@findhao: for debug
# set(redshow_path /home/yhao24/opt/redshow_debug)

if(NOT redshow_path)
    message(FATAL_ERROR "You need to specify redshow library path. ")
endif()

if(NOT gpu_patch_path)
    message(INFO "Use default gpu_patch_path:" ${CMAKE_CURRENT_LIST_DIR}/gpu-patch/ )
    set(gpu_patch_path ${CMAKE_CURRENT_LIST_DIR}/gpu-patch/)
endif()

include_directories(${SANITIZER_PATH}/include/ 
                ${CUDAToolkit_TARGET_DIR}/include/ 
                ${redshow_path}/include/ ${redshow_path}/include/common 
                ${CMAKE_CURRENT_LIST_DIR}/include/
                ${CMAKE_CURRENT_LIST_DIR}/src/
                ${gpu_patch_path}/include/
                )
message(INFO ${SANITIZER_PATH} ${CUDAToolkit_NVCC_EXECUTABLE})

# find_library(redshow redshow HINTS ${redshow_path}/lib)

add_link_options(-L${SANITIZER_PATH}  -fPIC -shared)
# add_custom_target(gputrigger_patch.fatbin ALL ${CUDAToolkit_NVCC_EXECUTABLE} -o gputrigger_patch.fatbin ${CMAKE_CUDA_FLAGS} --fatbin --keep-device-functions -Xptxas --compile-as-tools-patch -g  -I${SANITIZER_PATH}/include/ -I${CUDAToolkit_TARGET_DIR}/include/ -c ${CMAKE_CURRENT_LIST_DIR}/gpu-patch.cu )
# set_property(TARGET gputrigger_patch.fatbin PROPERTY CUDA_ARCHITECTURES "70;75;80;86")

aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/src/ gputrigger_SRC)
# file(GLOB gputrigger_SRC CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/include/*.h" "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp" "${CMAKE_CURRENT_LIST_DIR}/src/*.c" )
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/src/lib/prof-lean DIR_PROF_LEAN)
file(GLOB gputrigger_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/include/*.h" )
file(GLOB PROF_LEAN_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/lib/prof-lean/*.h")
add_library(gputrigger SHARED ${gputrigger_SRC} ${DIR_PROF_LEAN} ${gputrigger_headers} ${PROF_LEAN_headers})

target_link_libraries(gputrigger PUBLIC
        -lmbedcrypto
        # ${redshow}
        -lredshow
        -L${redshow_path}/lib
        -lelf
        -lsanitizer-public
        -lcuda
        -lcudart
        -L${CUDAToolkit_TARGET_DIR}/lib64
        -ldl
        )

# install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/gputrigger_patch.fatbin DESTINATION ${CMAKE_INSTALL_PREFIX})
install(TARGETS gputrigger DESTINATION ${CMAKE_INSTALL_PREFIX})
install(FILES  ${gputrigger_headers} DESTINATION ${CMAKE_INSTALL_PREFIX}/include/)
