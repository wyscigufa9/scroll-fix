cmake_minimum_required(VERSION 3.20)
project(ScrollFix VERSION 1.0.0 LANGUAGES CXX RC)

add_executable(ScrollFix WIN32
    src/main.cpp
    src/ScrollFix.rc
)

target_compile_features(ScrollFix PRIVATE cxx_std_20)
target_compile_definitions(ScrollFix PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(ScrollFix PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(ScrollFix PRIVATE /OPT:REF /OPT:ICF)
endif()

target_link_libraries(ScrollFix PRIVATE
    user32
    shell32
    advapi32
)

install(TARGETS ScrollFix RUNTIME DESTINATION .)
