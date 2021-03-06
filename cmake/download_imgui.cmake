# find or download imgui

if (${FG_ENABLE_IMGUI})
	set( FG_EXTERNAL_IMGUI_PATH "" CACHE PATH "path to imgui source" )

	# reset to default
	if (NOT EXISTS "${FG_EXTERNAL_IMGUI_PATH}/imgui.h")
		message( STATUS "imgui is not found in \"${FG_EXTERNAL_IMGUI_PATH}\"" )
		set( FG_EXTERNAL_IMGUI_PATH "${FG_EXTERNALS_PATH}/imgui" CACHE PATH "" FORCE )
	else ()
		message( STATUS "imgui found in \"${FG_EXTERNAL_IMGUI_PATH}\"" )
	endif ()

	if (NOT EXISTS "${FG_EXTERNAL_IMGUI_PATH}/imgui.h")
		set( FG_IMGUI_REPOSITORY "https://github.com/ocornut/imgui.git" )
	else ()
		set( FG_IMGUI_REPOSITORY "" )
	endif ()
	
	set( FG_IMGUI_INSTALL_DIR "${FG_EXTERNALS_INSTALL_PATH}/imgui" CACHE INTERNAL "" FORCE )

	ExternalProject_Add( "External.imgui"
		LIST_SEPARATOR		"${FG_LIST_SEPARATOR}"
		# download
		DOWNLOAD_DIR		"${FG_EXTERNAL_IMGUI_PATH}"
		GIT_REPOSITORY		${FG_IMGUI_REPOSITORY}
		GIT_TAG				master
		GIT_PROGRESS		1
		EXCLUDE_FROM_ALL	1
		LOG_DOWNLOAD		1
		# update
		PATCH_COMMAND		${CMAKE_COMMAND} -E copy
							${CMAKE_FOLDER}/imgui_CMakeLists.txt
							${FG_EXTERNAL_IMGUI_PATH}/CMakeLists.txt
		UPDATE_DISCONNECTED	1
		LOG_UPDATE			1
		# configure
		SOURCE_DIR			"${FG_EXTERNAL_IMGUI_PATH}"
		CMAKE_GENERATOR		"${CMAKE_GENERATOR}"
		CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
		CMAKE_GENERATOR_TOOLSET	"${CMAKE_GENERATOR_TOOLSET}"
		CMAKE_ARGS			"-DCMAKE_CONFIGURATION_TYPES=${FG_EXTERNAL_CONFIGURATION_TYPES}"
							"-DCMAKE_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION}"
							"-DCMAKE_INSTALL_PREFIX=${FG_IMGUI_INSTALL_DIR}"
							"-DCMAKE_DEBUG_POSTFIX="
							"-DCMAKE_RELEASE_POSTFIX="
							${FG_BUILD_TARGET_FLAGS}
		LOG_CONFIGURE 		1
		# build
		BINARY_DIR			"${CMAKE_BINARY_DIR}/build-imgui"
		BUILD_COMMAND		"${CMAKE_COMMAND}"
							--build .
							--target imgui
							--config $<CONFIG>
		# install
		INSTALL_DIR 		"${FG_IMGUI_INSTALL_DIR}"
		LOG_INSTALL 		1
		# test
		TEST_COMMAND		""
	)
	
	set_property( TARGET "External.imgui" PROPERTY FOLDER "External" )
	set( FG_GLOBAL_DEFINITIONS "${FG_GLOBAL_DEFINITIONS}" "FG_ENABLE_IMGUI" )
	set( FG_IMGUI_SOURCE_DIR "${FG_EXTERNAL_IMGUI_PATH}" CACHE INTERNAL "" FORCE)
	set( FG_IMGUI_LIBRARY "${FG_IMGUI_INSTALL_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}imgui${CMAKE_STATIC_LIBRARY_SUFFIX}" CACHE INTERNAL "" FORCE )
endif ()
