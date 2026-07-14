cmake_minimum_required(VERSION 3.20)

project(AntiPlayerMosaic LANGUAGES CXX)

add_executable(scan_test scan_test.cpp)
target_compile_features(scan_test PRIVATE cxx_std_20)

add_library(AntiPlayerMosaic SHARED AntiPlayerMosaic.cpp)
target_compile_features(AntiPlayerMosaic PRIVATE cxx_std_20)
target_link_libraries(AntiPlayerMosaic PRIVATE Psapi)
