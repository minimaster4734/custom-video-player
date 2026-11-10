cmake_minimum_required(VERSION 3.22.1)
project(customplayer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prebuilt shared libraries (produced by third_party/build_scripts/build_all.sh).
# They live in src/main/jniLibs/<abi>/ so Gradle packages them into the APK;
# here they are only imported so this library can link against them.
set(PREBUILT_LIBS ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})
set(PREBUILT_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/include)

foreach(lib avformat avcodec avutil swresample swscale dav1d fdk-aac)
    add_library(${lib} SHARED IMPORTED)
    set_target_properties(${lib} PROPERTIES
        IMPORTED_LOCATION ${PREBUILT_LIBS}/lib${lib}.so)
endforeach()

add_library(customplayer SHARED
    native_player.cpp
    decoder.cpp
    packet_queue.cpp
    frame_queue.cpp
    audio_output.cpp
    renderer.cpp)

target_include_directories(customplayer PRIVATE
    ${PREBUILT_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(customplayer PRIVATE -Wall -O2)

target_link_libraries(customplayer
    avformat avcodec avutil swresample swscale dav1d fdk-aac
    log android EGL GLESv2 OpenSLES)
