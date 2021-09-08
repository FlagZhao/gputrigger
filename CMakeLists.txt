cmake_minimum_required(VERSION 3.16)
project(gputrigger)

include(CheckLanguage)
check_language(CUDA)

enable_language(CUDA)

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

find_package(Threads REQUIRED)


set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

set(SANITIZER_PATH ${CUDAToolkit_TARGET_DIR}/compute-sanitizer)

if(NOT redshow_path)
    message(FATAL_ERROR "You need to specify redshow library path. ")
endif()



include_directories(${SANITIZER_PATH}/include/ ${CUDAToolkit_TARGET_DIR}/include/ ${redshow_path}/include ${redshow_path}/include/common ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/lib/prof-lean)
message(INFO ${SANITIZER_PATH} ${CUDAToolkit_NVCC_EXECUTABLE})

set(CMAKE_CXX_STANDARD 17)
set(SOURCE_FILES ${CMAKE_CURRENT_LIST_DIR}/gpu-patch.cu)
link_libraries(-lsanitizer-public)
link_libraries(-lpthread)
link_directories(${redshow_path}/lib)
aux_source_directory(${CMAKE_CURRENT_LIST_DIR}/lib/prof-lean DIR_PROF_LEAN)
add_link_options(-L${SANITIZER_PATH}  -fPIC -shared)
add_custom_target(gputrigger_patch.fatbin ALL ${CUDAToolkit_NVCC_EXECUTABLE} -o gputrigger_patch.fatbin ${CMAKE_CUDA_FLAGS} --fatbin --keep-device-functions -Xptxas --compile-as-tools-patch -g  -I${SANITIZER_PATH}/include/ -I${CUDAToolkit_TARGET_DIR}/include/ -c ${SOURCE_FILES} )
set_property(TARGET gputrigger_patch.fatbin PROPERTY CUDA_ARCHITECTURES 75)
file(GLOB gputrigger_SRC CONFIGURE_DEPENDS "*.h" "*.cpp" "*.c")

add_library(gputrigger SHARED ${gputrigger_SRC})

set(THREADS_PREFER_PTHREAD_FLAG ON)


target_link_libraries(gputrigger PUBLIC
        Threads::Threads
        ${DIR_PROF_LEAN}
        -lmbedcrypto
        -lredshow
        -lelf
        )

file(GLOB gputrigger_headers CONFIGURE_DEPENDS "*.h" )

install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/gputrigger_patch.fatbin DESTINATION ${CMAKE_INSTALL_PREFIX})
install(TARGETS gputrigger DESTINATION ${CMAKE_INSTALL_PREFIX})
install(FILES  ${gputrigger_headers} DESTINATION ${CMAKE_INSTALL_PREFIX}/include/)
