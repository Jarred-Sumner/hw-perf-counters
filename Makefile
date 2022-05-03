
x64: 
	clang counters.c -shared -O3 -mtune=native -march=native -o counters.x64.dylib
arm64: 
	clang counters.c -shared -O3 -mtune=native -o counters.arm64.dylib
