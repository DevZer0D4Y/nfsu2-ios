cmake_minimum_required(VERSION 3.20)
project(nv2a_metal_msl_test C OBJC)
# The Metal renderer's shaders: generated MSL must compile and link for
# random vertex programs and combiner states, optionally for every program
# a run recorded (progcache.bin, passed on the command line), and the clip
# mapping must keep the NV2A's orientation and winding. macOS only.
if(NOT APPLE)
    message(FATAL_ERROR "This test needs Metal")
endif()
set(CMAKE_C_STANDARD 11)
set(SRC ../../src)
add_executable(nv2a_metal_msl_test
    test_main.m
    ${SRC}/nv2a_metal/nv2a_metal_msl.c
    ${SRC}/nv2a_gl/gl_vsh.c
    ${SRC}/nv2a_gl/gl_psh.c)
target_include_directories(nv2a_metal_msl_test PRIVATE ${SRC}/nv2a_metal)
target_compile_options(nv2a_metal_msl_test PRIVATE -Wall -Wextra -Wno-unused-parameter
                       -Wno-missing-field-initializers)
set_source_files_properties(test_main.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(nv2a_metal_msl_test PRIVATE "-framework Foundation" "-framework Metal")
enable_testing()
add_test(NAME nv2a_metal_msl_test COMMAND nv2a_metal_msl_test)
