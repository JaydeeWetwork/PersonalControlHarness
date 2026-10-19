# Audio: synth, SFX and the command queue (depends on core only).
add_library(litholattice_audio STATIC voice.cpp synth.cpp audio_engine.cpp voice_allocator.cpp occlusion_thread.cpp spatial_audio.cpp effects.cpp streaming.cpp codecs.cpp audio_selftest.cpp)
target_include_directories(litholattice_audio PUBLIC include)
target_link_libraries(litholattice_audio PUBLIC litholattice_core)
litholattice_module_options(litholattice_audio)

# Platform audio backend: miniaudio (the only user of miniaudio.h). Implements openAudioOutput().
find_package(Threads REQUIRED)
add_library(litholattice_miniaudio_impl STATIC miniaudio/miniaudio_impl.c)
target_include_directories(litholattice_miniaudio_impl PUBLIC ${LITHOLATTICE_MINIAUDIO_DIR})
target_compile_definitions(litholattice_miniaudio_impl PUBLIC MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION
                           MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
target_link_libraries(litholattice_miniaudio_impl PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(litholattice_miniaudio_impl PUBLIC m)
endif()

add_library(litholattice_audio_device STATIC miniaudio/miniaudio_output.cpp)
target_link_libraries(litholattice_audio_device PUBLIC litholattice_audio PRIVATE litholattice_miniaudio_impl)
litholattice_module_options(litholattice_audio_device)
