TARGET   = testing.dylib
SRC      = testing.c ../syphon/macho_sea.c
SDK      = $(shell xcrun --sdk macosx --show-sdk-path)
CFLAGS   = -arch arm64 -bundle -fPIC -Wall -Wextra \
           -isysroot $(SDK) \
           -I../syphon \
           -Wl,-undefined,dynamic_lookup
LIBS     = -framework CoreFoundation -framework Foundation

all: $(TARGET)

$(TARGET): $(SRC)
	clang $(CFLAGS) -o $@ $(SRC) $(LIBS)
	codesign -f -s - $@
	@echo "\n=== Built $(TARGET) for arm64 ==="

universal:
	clang -arch arm64e -arch arm64 -bundle -fPIC -Wall -Wextra \
		-isysroot $(SDK) \
		-I../syphon \
		-Wl,-undefined,dynamic_lookup \
		-o $(TARGET) $(SRC) $(LIBS)
	codesign -f -s - $(TARGET)
	@echo "\n=== Built universal $(TARGET) (arm64e + arm64) ==="

clean:
	rm -f $(TARGET)

.PHONY: all universal clean
