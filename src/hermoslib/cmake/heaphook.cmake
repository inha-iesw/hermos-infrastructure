# Cargo owns dependency tracking using the checked-in dependency lockfile.
# Run it on each build; an unchanged release build is a cheap no-op.
find_program(CARGO_EXECUTABLE cargo)
if(NOT CARGO_EXECUTABLE)
  message(FATAL_ERROR "cargo is required to build the Hermos heap hook")
endif()
get_filename_component(hermos_heaphook_source
  "${CMAKE_CURRENT_SOURCE_DIR}/../../hermos_heaphook" ABSOLUTE)
set(hermos_heaphook_output "${CMAKE_CURRENT_BINARY_DIR}/libhermos_heaphook.so")
add_custom_target(hermos_heaphook ALL
  COMMAND "${CARGO_EXECUTABLE}" build --locked --release --no-default-features
    --target-dir "${CMAKE_CURRENT_BINARY_DIR}/heaphook"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/heaphook/release/libhermos_heaphook.so"
    "${hermos_heaphook_output}"
  BYPRODUCTS "${hermos_heaphook_output}"
  WORKING_DIRECTORY "${hermos_heaphook_source}"
  COMMENT "Building the Hermos heap hook"
  VERBATIM
)
add_dependencies(hermos_heaphook hermos)
# The exported libhermos target is copied even during ament symlink installs.
# Explicit file permissions also select a real copy for the hook, so resolving
# its path keeps the actual library as its sibling (never Cargo's link stub).
install(FILES "${hermos_heaphook_output}" DESTINATION lib
  PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
