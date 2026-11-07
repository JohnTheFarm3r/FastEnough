cmake_minimum_required(VERSION 3.22.1)
project(fastenough_companion CXX)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)

add_library(afmcompanion SHARED
    companion_jni.cpp
    ${REPO_ROOT}/server/afm-server.cpp
    ${REPO_ROOT}/thirdparty/motioncam/lib/Decoder.cpp
    ${REPO_ROOT}/thirdparty/motioncam/lib/RawData.cpp
    ${REPO_ROOT}/thirdparty/motioncam/lib/RawData_Legacy.cpp
)

target_include_directories(afmcompanion PRIVATE
    ${REPO_ROOT}/server
    ${REPO_ROOT}/thirdparty
    ${REPO_ROOT}/thirdparty/motioncam/include
)

target_compile_definitions(afmcompanion PRIVATE AFM_HOST_LIBRARY)
target_compile_options(afmcompanion PRIVATE -O2 -march=armv8-a+crc -fexceptions -frtti)
target_link_libraries(afmcompanion PRIVATE log)
