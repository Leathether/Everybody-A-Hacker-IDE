# Everybody A Hacker IDE

This is an AI-powered IDE that helps make coding accessible to everyone. The IDE uses AI to help write code for you, lowering the barrier to entry for programming.

## Release Information
- Beta Release
- YouTube Tutorial (Coming January 30th, 2025): https://youtu.be/oUMm0oQ_3rs

## Prerequisites

You will need:
1. A GROQ API Key
2. Git installed
3. CMake and a C++ compiler
4. Internet connection

## Installation Steps

### 1. Install System Dependencies

#### Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install build-essential cmake
sudo apt-get install libcurl4-openssl-dev nlohmann-json3-dev
sudo apt-get install libgl1-mesa-dev libglu1-mesa-dev xorg-dev
sudo apt-get install libglfw3-dev
```

#### Fedora:
```bash
sudo dnf install gcc-c++ cmake
sudo dnf install libcurl-devel nlohmann-json-devel
sudo dnf install mesa-libGL-devel mesa-libGLU-devel libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel
sudo dnf install libglfw3-devel
```

#### macOS (using Homebrew):
```bash
brew install cmake
brew install curl nlohmann-json
brew install glfw
```
Note: OpenGL is included with macOS

### 2. Clone the Repository
```bash
git clone https://github.com/Leathether/Everybody-A-Hacker-IDE
cd Everybody-A-Hacker-IDE
```

### 3. Set up ImGui
```bash
# Clone ImGui repository
git clone https://github.com/ocornut/imgui
# Create ImGui directories in your project
mkdir -p imgui/backends
# Copy ImGui files
cp imgui/*.cpp imgui/*.h imgui/
cp imgui/backends/imgui_impl_glfw.* imgui/backends/
cp imgui/backends/imgui_impl_opengl3.* imgui/backends/
cp imgui/backends/imgui_impl_opengl3_loader.h imgui/backends/
```

### 4. Build the Project
```bash
mkdir build
cd build
cmake ..
make
```

### 5. Set up GROQ API Key
```bash
export GROQ_API_KEY=your_api_key_here
```
Replace `your_api_key_here` with your actual GROQ API key.

### 6. Run the IDE
```bash
./cursor_clone
```

## Features in Beta
- Fixed compiler bugs and documentation

## Known Issues
- This is a beta release, expect some bugs
- Please report any issues on GitHub

## Contributing
- Feel free to open issues for bug reports or suggestions
- Feel free to clone and modify the project

## Technical Notes
- The project uses ImGui for the graphical interface
- Requires active internet connection for AI features
- Uses GROQ API for AI capabilities

