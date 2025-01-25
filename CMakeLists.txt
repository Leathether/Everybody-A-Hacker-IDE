cmake_minimum_required(VERSION 3.10)
project(cursor_clone)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set OpenGL preference to GLVND to fix the warning
set(OpenGL_GL_PREFERENCE GLVND)

# Find required packages
find_package(CURL REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)

# Add ImGui source files
add_library(imgui
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp
)

# Include directories
target_include_directories(imgui PUBLIC
    imgui
    imgui/backends
)

# Add executable
add_executable(cursor_clone 
    main.cpp
    groq_client.cpp
    text_editor.cpp
    cursor_clone.cpp
)

# Add include directories for the executable
target_include_directories(cursor_clone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CURL_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(cursor_clone PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    imgui
    glfw
    OpenGL::GL
) 