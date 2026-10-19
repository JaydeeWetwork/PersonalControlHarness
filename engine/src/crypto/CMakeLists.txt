# litholattice_crypto: real cryptography for mod signing (Ed25519 + SHA-256).
# Clean-room Ed25519 (RFC 8032) + SHA-256/SHA-512 (FIPS 180-4).
# The litho_sign CLI (keygen/sign/verify) builds here too.
# Clean-room Ed25519 + SHA-256/SHA-512. No third-party crypto code.
add_library(litholattice_crypto STATIC
  sha256.cpp
  sha512.cpp
  ed25519.cpp
  signing.cpp
  crypto_selftest.cpp)
target_include_directories(litholattice_crypto PUBLIC include)
target_link_libraries(litholattice_crypto PUBLIC litholattice_core)
litholattice_module_options(litholattice_crypto)

# litho_sign: JD's mod-signing tool. keygen stays OFFLINE with JD; only .pub ships.
add_executable(litho_sign litho_sign_main.cpp)
target_link_libraries(litho_sign PRIVATE litholattice_crypto)
litholattice_module_options(litho_sign)
# The tool must run from a plain shell anywhere; keep it in the build root.
set_target_properties(litho_sign PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
