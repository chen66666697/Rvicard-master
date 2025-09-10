# 交叉编译器
CC = arm-rockchip830-linux-uclibcgnueabihf-gcc

# 打包文件名
TARBALL = InfoNES

# Buildroot sysroot 路径
SYSROOT = /luckfox-pico-main/sysdrv/source/buildroot/buildroot-2023.02.6/output/host/usr/arm-buildroot-linux-uclibcgnueabihf/sysroot

# 头文件和库路径
INCLUDES = -I$(SYSROOT)/usr/include
LDFLAGS = -L$(SYSROOT)/usr/lib

# 源文件
CFILES = ./../K6502.cpp \
         ./../InfoNES.cpp \
         ./../InfoNES_Mapper.cpp \
         ./../InfoNES_pAPU.cpp \
         ./InfoNES_System_Linux.cpp \
         joypad_input.cpp

# 生成的目标文件
OFILES = $(CFILES:.cpp=.o)

# 编译参数
CCFLAGS = -O2 -fsigned-char

# 链接库
LDLIBS = -lstdc++ -lasound -lm -lz -lpthread

# 默认目标
all: InfoNES

# 编译 InfoNES 可执行文件
InfoNES: $(OFILES)
	$(CC) $(INCLUDES) -o $@ $(OFILES) $(LDFLAGS) $(LDLIBS)

# 生成 .o 文件
.cpp.o:
	$(CC) $(INCLUDES) -c $(CCFLAGS) $< -o $@

# 清理生成文件
clean:
	rm -f $(OFILES) ../*~ ../*/*~ core

# 清理所有生成文件，包括可执行文件
cleanall: clean
	rm -f InfoNES

# 打包源码
tar: clean
	( cd ..; \
	  tar cvf $(TARBALL).tar ./*; \
	  gzip -f $(TARBALL).tar )

# 安装可执行文件到系统路径
install: InfoNES
	install ./InfoNES /usr/local/bin

