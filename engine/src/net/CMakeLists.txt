# Net: snapshot/interpolation model for the 512 TPS deterministic sim.
#
# Transport-agnostic byte-buffer codec: packet framing, delta-vs-acked-baseline
# snapshots, fixed-point quantization, sequence/ack handling, client
# interpolation, and Veloren-style bandwidth budgeting (PrioManager). No
# socket code here; the game drives encode/decode over UDP later.
add_library(litholattice_net STATIC quantize.cpp protocol.cpp snapshot.cpp priority.cpp channels.cpp confinement.cpp confinement_selftest.cpp handshake.cpp encryption.cpp)
target_include_directories(litholattice_net PUBLIC include)
target_link_libraries(litholattice_net PUBLIC litholattice_core)
litholattice_module_options(litholattice_net)
