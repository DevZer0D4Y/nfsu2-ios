cmake_minimum_required(VERSION 3.20)
project(nv2a_metal C OBJC)

# Metal renderer for the pushbuffer executor (XBOXRECOMP_METAL=ON, Apple
# only), built beside nv2a_vk and chosen by the host at run time. Shares the
# shader generators with nv2a_gl / nv2a_vk (wrapped in MSL).
add_library(xbox_nv2a_metal STATIC
    nv2a_metal.m
    nv2a_metal_msl.c
)
# The generators are compiled once: by nv2a_vk when it is built too.
if(NOT TARGET xbox_nv2a_vk)
    target_sources(xbox_nv2a_metal PRIVATE ../nv2a_gl/gl_vsh.c ../nv2a_gl/gl_psh.c)
endif()
target_include_directories(xbox_nv2a_metal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_nv2a_metal PUBLIC
    "-framework Metal" "-framework QuartzCore" "-framework Foundation")
# The back end needs macOS 14 / iOS 17 (dynamic vertex strides); the host
# checks that once before installing it, so the per-call warnings are noise.
set_source_files_properties(nv2a_metal.m PROPERTIES COMPILE_OPTIONS
    "-fobjc-arc;-Wno-unguarded-availability-new;-Wno-unguarded-availability")
target_compile_options(xbox_nv2a_metal PRIVATE -Wall -Wextra -Wno-unused-parameter
                       -Wno-missing-field-initializers -O2)
