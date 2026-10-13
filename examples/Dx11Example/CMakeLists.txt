add_executable(Dx11Example WIN32 main.cpp)

target_link_libraries(Dx11Example PRIVATE ReZero::ReZeroGui)

# The backend header pulls in d3dcompiler for runtime shader compilation.
target_link_libraries(Dx11Example PRIVATE d3d11.lib dxgi.lib d3dcompiler.lib)
