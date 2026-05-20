CLANG ?= clang

BPF_CFLAGS = -O2 -g -Wno-missing-declarations

all: user

xdp_fw.bpf.o: xdp_fw.bpf.c
	$(CLANG) \
		-target bpf \
		-D__TARGET_ARCH_x86 \
		$(BPF_CFLAGS) \
		-c xdp_fw.bpf.c \
		-o xdp_fw.bpf.o

xdp_fw.skel.h: xdp_fw.bpf.o
	bpftool gen skeleton xdp_fw.bpf.o > xdp_fw.skel.h

user: xdp_fw.skel.h user.c
	cc -O2 -g user.c -lbpf -lelf -lz -o user

clean:
	rm -f *.o *.skel.h user