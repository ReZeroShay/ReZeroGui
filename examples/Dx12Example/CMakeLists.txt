add_executable(Dx12Example WIN32 main.cpp)

target_link_libraries(Dx12Example PRIVATE ReZero::ReZeroGui)

# The backend header pulls in d3dcompiler for runtime shader compilation.
target_link_libraries(Dx12Example PRIVATE d3d12.lib dxgi.lib d3dcompiler.lib)
