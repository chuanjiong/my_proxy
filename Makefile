


C_SOURCES += *.c *.S
CPP_SOURCES += *.cpp

C_FLAGS += -DLINUX -DMD_HAVE_EPOLL -Wall -O2
CPP_FLAGS += -std=c++11

TARGET += my_proxy


all:
	arm-hisiv400-linux-gnueabi-gcc -c $(C_SOURCES) $(C_FLAGS)
	arm-hisiv400-linux-gnueabi-g++ *.o $(CPP_SOURCES) $(CPP_FLAGS) -o $(TARGET)

clean:
	rm -rf *.o $(TARGET)

