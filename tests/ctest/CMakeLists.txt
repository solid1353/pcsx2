enable_testing()
add_custom_target(unittests)
add_custom_command(TARGET unittests POST_BUILD COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure)

macro(add_pcsx2_test target)
	add_executable(${target} EXCLUDE_FROM_ALL ${ARGN})
	target_link_libraries(${target} PRIVATE gtest)
	if(APPLE)
		# For some reason this doesn't get pulled in implicitly...
		target_link_libraries(${target} PRIVATE
			"-framework Foundation"
			"-framework Cocoa"
		)
	endif()

	add_dependencies(unittests ${target})
	add_test(NAME ${target} COMMAND ${target})

	# Windows finds the dependency DLLs through PATH, not next to the test executable.
	if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
		foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
			file(TO_NATIVE_PATH "${prefix}/bin" prefix_bin)
			set_property(TEST ${target} APPEND PROPERTY ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${prefix_bin}")
		endforeach()
	endif()

	if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
		# For some reason, the stack refuses to grow with the latest MSVC updates.
		# Just force the commit size to 1MB for now.
		set_target_properties(${target} PROPERTIES LINK_FLAGS "/STACK:1048576,1048576")
	endif()
endmacro()

add_subdirectory(common)
add_subdirectory(core)
