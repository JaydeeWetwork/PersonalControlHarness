# ui: retained-mode widget toolkit (tree, layout, theme, input routing, controls). Depends on
# core only: drawing (IUiRenderer), clipboard and script calls (IScriptHost) are injected.
add_library(litholattice_ui STATIC widget.cpp painter.cpp theme.cpp ui_root.cpp controls.cpp containers.cpp
            text_buffer.cpp text_edit.cpp lists.cpp menus.cpp dialog.cpp widget_factory.cpp mod_ui.cpp ui_selftest.cpp)
target_include_directories(litholattice_ui PUBLIC include)
target_link_libraries(litholattice_ui PUBLIC litholattice_core)
litholattice_module_options(litholattice_ui)
