add_library(
	vkutil
	STATIC
	include/vkutil/objects.h
	include/vkutil/vkutil.h
	include/vkutil/vma_libretro.h

	src/objects.cpp
	src/vkutil.cpp
)

if(LIBRETRO)
	target_sources(vkutil PRIVATE src/vma_libretro.cpp)
endif()

target_include_directories(vkutil PUBLIC include)
target_link_libraries(vkutil PUBLIC vulkan vma)
target_link_libraries(vkutil PRIVATE util)
