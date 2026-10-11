# The movie decoder's FFmpeg on Apple targets: dynamic frameworks from
# tools/build_ffmpeg_apple.sh (LGPL 2.1, only the vp6 decoder), linked and
# shipped beside the app instead of the static libraries other targets use.
#   macOS          found through an rpath to NFSU2_FFMPEG_DIR/Frameworks
#   iOS (Xcode)    embedded and signed by Xcode (XCODE_EMBED_FRAMEWORKS)
#   iOS (Ninja)    copied into the bundle's Frameworks/ (Simulator builds)
# The LGPL text goes along in every case.
set(_nfsu2_ff "${NFSU2_FFMPEG_DIR}/Frameworks")
target_sources(${PROJECT_NAME} PRIVATE src/movie_vp6.c)
target_compile_definitions(${PROJECT_NAME} PRIVATE NFSU2_NATIVE_VP6=1)
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${NFSU2_FFMPEG_DIR}/include")
target_link_libraries(${PROJECT_NAME} PRIVATE
    "${_nfsu2_ff}/avcodec.framework" "${_nfsu2_ff}/avutil.framework")
if(NOT IOS)
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY BUILD_RPATH "${_nfsu2_ff}")
elseif(CMAKE_GENERATOR STREQUAL "Xcode")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        XCODE_EMBED_FRAMEWORKS "${_nfsu2_ff}/avcodec.framework;${_nfsu2_ff}/avutil.framework"
        XCODE_EMBED_FRAMEWORKS_CODE_SIGN_ON_COPY ON
        XCODE_EMBED_FRAMEWORKS_REMOVE_HEADERS_ON_COPY ON
        XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${_nfsu2_ff}/FFmpeg-COPYING.LGPLv2.1"
                "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/FFmpeg-COPYING.LGPLv2.1")
else()
    set_property(TARGET ${PROJECT_NAME} APPEND PROPERTY BUILD_RPATH "@executable_path/Frameworks")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_nfsu2_ff}/avcodec.framework"
                "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Frameworks/avcodec.framework"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${_nfsu2_ff}/avutil.framework"
                "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/Frameworks/avutil.framework"
        COMMAND ${CMAKE_COMMAND} -E copy "${_nfsu2_ff}/FFmpeg-COPYING.LGPLv2.1"
                "$<TARGET_BUNDLE_CONTENT_DIR:${PROJECT_NAME}>/FFmpeg-COPYING.LGPLv2.1")
endif()
