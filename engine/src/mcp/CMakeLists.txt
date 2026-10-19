# mcp: Phase 0 clean-room JSON-RPC 2.0 core + Phase 1 server (handshake era,
# stdio transport, agent identity + kPerm* gating, read-only tool/resource
# surface) + Phase 2 Streamable HTTP transport (sockets, strict HTTP/1.1
# parser, SSE framing seam, bearer-token auth, Mcp-Session-Id sessions,
# McpHttpServer owning one McpServer per session).
#
# Link footprint (locked): the server talks to engine subsystems only through
# injectable seam interfaces declared in server.h, so it needs NO other engine
# module at link time. The three cross-module headers it reads (kPerm* bits
# from mods, engine version macros from net, IWorldQuery from console) are
# all header-only; their include dirs come in as PRIVATE relative paths so
# consumers of litholattice_mcp gain no link dependencies and the module
# works regardless of add_subdirectory ordering.
add_library(litholattice_mcp STATIC json.cpp jsonrpc.cpp server.cpp prompts.cpp mcp_selftest.cpp
            sockets.cpp http.cpp http_server.cpp mcp_http_selftest.cpp rate_limit.cpp senses.cpp)
target_include_directories(litholattice_mcp PUBLIC include)
target_include_directories(litholattice_mcp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../mods/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../console/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../net/include)
litholattice_module_options(litholattice_mcp)
