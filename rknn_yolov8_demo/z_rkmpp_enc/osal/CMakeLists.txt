# vim: syntax=cmake
cmake_minimum_required(VERSION 2.6.3)
PROJECT(osal C CXX)
INCLUDE(GNUInstallDirs)

find_package(Threads)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_REENTRANT -D_GNU_SOURCE")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

if (HAVE_DRM)
    add_definitions(-DHAVE_DRM)
    set(DRM_FILES allocator/allocator_drm.c)
    message(STATUS "compile with drm support")
else()
    message(STATUS "compile without drm support")
endif()

# 创建静态库、动态库或对象库
add_library(osal STATIC
    mpp_mem.cpp
    mpp_log.cpp
    mpp_lock.cpp
    mpp_time.cpp
    mpp_common.cpp
)

target_link_libraries(osal ${CMAKE_THREAD_LIBS_INIT})

target_include_directories(osal PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/inc"
)

set_target_properties(osal PROPERTIES FOLDER "osal")

# leave those special platform here
if(ANDROID)
    add_definitions(-static)
    # in Android pthread is in libc, also need liblog
    # Android 14 requires libc++ not libstdc++
    if("${ANDROID_STL}" STREQUAL "c++_static")
        target_link_libraries(osal log m)
    else()
        target_link_libraries(osal log stdc++ m)
    endif()
endif(ANDROID)

# unit test
# add_subdirectory(test)
