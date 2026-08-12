cmake_minimum_required(VERSION 3.20)

project(AntiPlayerMosaic LANGUAGES CXX RC)

option(ANTIPLAYER_BUILD_SCAN_TEST "Build the offline signature scanner" ON)

if(ANTIPLAYER_BUILD_SCAN_TEST)
    add_executable(scan_test scan_test.cpp)
    target_compile_features(scan_test PRIVATE cxx_std_20)
endif()

add_library(AntiPlayerMosaic SHARED AntiPlayerMosaic.cpp AntiPlayerMosaic.rc)
target_compile_features(AntiPlayerMosaic PRIVATE cxx_std_20)
target_compile_definitions(AntiPlayerMosaic PRIVATE
    $<$<CONFIG:Release>:ANTIPLAYER_RELEASE_RUNTIME=1>
)
target_link_libraries(AntiPlayerMosaic PRIVATE Psapi)
