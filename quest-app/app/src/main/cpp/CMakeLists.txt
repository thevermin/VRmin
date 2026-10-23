cmake_minimum_required(VERSION 3.22.1)
project(qvbclient CXX C)
set(CMAKE_CXX_STANDARD 17)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

add_library(qvbclient SHARED main.cpp)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")
target_link_libraries(qvbclient native_app_glue OpenXR::openxr_loader android log EGL GLESv3 mediandk camera2ndk aaudio nativewindow z)
