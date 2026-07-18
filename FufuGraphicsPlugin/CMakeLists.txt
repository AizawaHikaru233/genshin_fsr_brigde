cmake_minimum_required(VERSION 3.20)

project(FufuGraphicsPlugin LANGUAGES CXX RC)

add_library(FufuGraphicsPlugin SHARED
    FufuGraphicsPlugin.cpp
    FufuGraphicsPlugin.rc
)

target_compile_features(FufuGraphicsPlugin PRIVATE cxx_std_20)
target_compile_definitions(FufuGraphicsPlugin PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_compile_options(FufuGraphicsPlugin PRIVATE /utf-8)
set_property(TARGET FufuGraphicsPlugin PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(FufuGraphicsPlugin PROPERTIES OUTPUT_NAME "FSR-Bridge-Plugin")
