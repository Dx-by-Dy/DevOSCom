CLANG ?= clang

CFLAGS = -O2 -g -Wno-missing-declarations

all: user

vmlinux.h:
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h

write.bpf.o: write.bpf.c write.h vmlinux.h
	$(CLANG) \
		-target bpf \
		-D__TARGET_ARCH_x86 \
		$(CFLAGS) \
		-c write.bpf.c \
		-o write.bpf.o

write.skel.h: write.bpf.o
	bpftool gen skeleton write.bpf.o > write.skel.h

user: write.skel.h user.c
	cc $(CFLAGS) user.c -lbpf -lelf -lz -o user

clean:
	rm -f *.o *.skel.h user vmlinux.h