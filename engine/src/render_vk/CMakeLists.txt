# render_vk: the Vulkan backend (IRenderer / ITextureArray from render). Vulkan-Headers only; the
# system loader is opened at run time. Shaders: assets/shaders/vk/*.vert|frag (+ the shared
# assets/shaders/chunk_shading.glsl) compiled to SPIR-V at build time by glslang into generated
# headers (spv/<name>_<stage>.h, `const uint32_t kSpv<Name><Stage>[]`).
set(LITHOLATTICE_VK_SHADER_DIR ${LithoLattice_SOURCE_DIR}/assets/shaders/vk)
set(LITHOLATTICE_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/spv)
file(MAKE_DIRECTORY ${LITHOLATTICE_SPV_DIR})
set(LITHOLATTICE_SPV_HEADERS)
foreach(prog chunk hud line sprite)
  foreach(stage vert frag)
    string(SUBSTRING ${prog} 0 1 first)
    string(TOUPPER ${first} first)
    string(SUBSTRING ${prog} 1 -1 rest)
    string(SUBSTRING ${stage} 0 1 sfirst)
    string(TOUPPER ${sfirst} sfirst)
    string(SUBSTRING ${stage} 1 -1 srest)
    set(out ${LITHOLATTICE_SPV_DIR}/${prog}_${stage}.h)
    add_custom_command(OUTPUT ${out}
      COMMAND glslang-standalone -V --target-env vulkan1.0 --quiet -I${LithoLattice_SOURCE_DIR}/assets/shaders
              --vn kSpv${first}${rest}${sfirst}${srest} -o ${out} ${LITHOLATTICE_VK_SHADER_DIR}/${prog}.${stage}
      DEPENDS glslang-standalone ${LITHOLATTICE_VK_SHADER_DIR}/${prog}.${stage} ${LITHOLATTICE_VK_SHADER_DIR}/frame.glsl
              ${LithoLattice_SOURCE_DIR}/assets/shaders/chunk_shading.glsl
      COMMENT "SPIR-V: vk/${prog}.${stage}"
      VERBATIM)
    list(APPEND LITHOLATTICE_SPV_HEADERS ${out})
  endforeach()
endforeach()

add_library(litholattice_render_vk STATIC vk_loader.cpp vk_handles.cpp vk_device.cpp vk_resources.cpp vk_vertex_arena.cpp
            vk_swapchain.cpp vk_texture_array.cpp vk_pipelines.cpp vk_renderer.cpp vk_probe.cpp ${LITHOLATTICE_SPV_HEADERS})
target_include_directories(litholattice_render_vk PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(litholattice_render_vk PUBLIC litholattice_render PRIVATE litholattice_core Vulkan::Headers ${CMAKE_DL_LIBS})
litholattice_module_options(litholattice_render_vk)
