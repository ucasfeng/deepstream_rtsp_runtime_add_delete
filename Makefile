CUDA_VER?=11.6
ifeq ($(CUDA_VER),)
  $(error "CUDA_VER is not set")
endif

APP:= deepstream-rtsp-runtime-add-delete

TARGET_DEVICE = $(shell gcc -dumpmachine | cut -f1 -d -)

DS_SDK_ROOT:=/opt/nvidia/deepstream/deepstream

LIB_INSTALL_DIR?=$(DS_SDK_ROOT)/lib/

SRCS:= $(wildcard *.c)

INCS:= $(wildcard *.h)

PKGS:= gstreamer-1.0

OBJS:= $(SRCS:.c=.o)

CFLAGS+= -I$(DS_SDK_ROOT)/sources/includes \
 -I /usr/local/cuda-$(CUDA_VER)/include

CFLAGS+= `pkg-config --cflags $(PKGS)`

LIBS:= `pkg-config --libs $(PKGS)`

LIBS+= -L$(LIB_INSTALL_DIR) -lnvdsgst_helper -lm -lnvdsgst_meta \
 -L/usr/local/cuda-$(CUDA_VER)/lib64/ -lcudart \
 -lcuda -Wl,-rpath,$(LIB_INSTALL_DIR)

all: $(APP)

.o: .c $(INCS) Makefile
	$(CC) -c $@ $(CFLAGS) $<

$(APP): $(OBJS) Makefile
	$(CC) -o $(APP) $(OBJS) $(LIBS)

clean:
	rm -rf $(OBJS) $(APP)
