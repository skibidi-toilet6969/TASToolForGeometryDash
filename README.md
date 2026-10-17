# TASToolForGeometryDash
Tool for time manipulation
# Compile
to compile payload.dll
```g++ -O3 -shared payload.cpp ui.cpp imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_win32.cpp backends/imgui_impl_dx9.cpp -o payload.dll -I. -ld3d9 -lgdi32 -ldwmapi -static-libgcc -static-libstdc++ -Wl,--out-implib,libpayload.a```
to compile injector.exe
```g++ -O3 injector.cpp -o injector.exe```
