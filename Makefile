.PHONY: all clean

all:
	@chmod +x ./build.sh
	@./build.sh

clean:
	rm -rf build bin

