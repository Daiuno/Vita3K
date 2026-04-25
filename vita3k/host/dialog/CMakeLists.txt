if(ANDROID)
	add_library(host_dialog STATIC
		src/filesystem_android.cpp
	)
elseif(IOS OR LIBRETRO)
	# The libretro frontend handles file dialogs itself; provide stubs so the
	# rest of the engine still links cleanly without nativefiledialog-extended.
	add_library(host_dialog STATIC
		src/filesystem_stub.cpp
	)
else()
	add_library(host_dialog STATIC
		src/filesystem_nfd.cpp
	)
endif()

# Create alias for more friendly naming and avoid conflicts
# in global CMake scope
add_library(host::dialog ALIAS host_dialog)

target_include_directories(host_dialog PUBLIC include)

target_link_libraries(host_dialog PUBLIC util)
if(ANDROID)
	target_link_libraries(host_dialog PRIVATE SDL3::SDL3 android)
elseif(IOS OR LIBRETRO)
	# stub uses no extra deps
else()
	target_link_libraries(host_dialog PRIVATE NFDe::NFDe)
endif()
