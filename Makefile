CXX = i686-w64-mingw32-g++
CXXFLAGS = -O2 -msse2 -mfpmath=sse -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -static -static-libgcc -static-libstdc++
SRCS = $(wildcard src/*.cpp)
HDRS = $(wildcard src/*.h) $(wildcard third_party/*/*.h) $(wildcard third_party/*/*.hlsl)
all: build/dinput8.dll
build/dinput8.dll: $(SRCS) $(HDRS) src/proxy.def
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Ithird_party -shared -o $@ $(SRCS) src/proxy.def -lwinmm -luuid -lws2_32
dist: build/dinput8.dll
	sh tools/dist/make-dist.sh
installtest:
	sh tools/dist/test-install.sh build/installtest/sh
	sh tools/dist/test-install-ps1.sh build/installtest/ps1
.PHONY: all clean dist installtest arkscantest onlinetest
clean:
	rm -rf build
glyphtest: build/glyphtest
build/glyphtest: test/glyphtest.cpp src/glyph_art.cpp src/glyph_canvas.cpp src/glyph_art.h src/glyph_canvas.h
	mkdir -p build
	c++ -std=c++17 -O2 -o $@ test/glyphtest.cpp src/glyph_art.cpp src/glyph_canvas.cpp
streaktest: build/streaktest
build/streaktest: test/streaktest.cpp src/streak_rebuild.cpp src/streak_rebuild.h
	mkdir -p build
	c++ -std=c++17 -O2 -Wall -Wextra -o $@ test/streaktest.cpp src/streak_rebuild.cpp
test: build/padtest.exe
build/padtest.exe: test/padtest.cpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/padtest.cpp -luuid
arkscantest: build/arkscantest
	./build/arkscantest
build/arkscantest: test/arkscantest.cpp src/arkscan.cpp src/arkscan.h
	mkdir -p build
	c++ -std=c++17 -O2 -Wall -Wextra -o $@ test/arkscantest.cpp src/arkscan.cpp
onlinetest: build/onlinetest
	./build/onlinetest
build/onlinetest: test/onlinetest.cpp src/online_conf.h src/online_text.h
	mkdir -p build
	c++ -std=c++17 -O2 -Wall -Wextra -o $@ test/onlinetest.cpp
