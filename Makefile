#
# Shenango Build
#

#
# Common
#

INC     = -I./inc
CFLAGS  = -g -Wall -std=gnu11 -D_GNU_SOURCE $(INC) -mssse3
LDFLAGS = -T base/base.ld -no-pie
LD	= gcc
CC	= gcc
AR	= ar
SPARSE	= sparse
CHECKFLAGS = -D__CHECKER__ -Waddress-space
FLTRACE = fltrace.so

# uncomment to autodetect MLX5
MLX5=$(shell lspci | grep 'ConnectX-5' || echo "")
MLX4=$(shell lspci | grep 'ConnectX-3' || echo "")

# Path and dir of this makefile
MKFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
MKFILE_DIR := $(dir $(MKFILE_PATH))

#
# Make options
#

ifneq ($(DEBUG),)
CFLAGS += -DDEBUG -DCCAN_LIST_DEBUG -rdynamic -O0 -ggdb
LDFLAGS += -rdynamic
else
ifneq ($(GDB),)
CFLAGS += -g -ggdb -O0
else
CFLAGS += -O3
endif
endif

ifneq ($(SAFEMODE),)
CFLAGS += -DSAFEMODE
CFLAGS += -g -O0	# TODO: SAFEMODE fails iokernel with -O3; cause unknown.
endif

ifneq ($(SUPPRESS_LOG),)
CFLAGS += -DSUPPRESS_LOG
endif

ifneq ($(NUMA_NODE),)
CFLAGS += -DNUMA_NODE=$(NUMA_NODE)
endif

ifneq ($(EXCLUDE_CORES),)
CFLAGS += -DEXCLUDE_CORES=$(EXCLUDE_CORES)
endif

ifneq ($(REMOTE_MEMORY),)
CFLAGS += -DREMOTE_MEMORY
endif

ifneq ($(REMOTE_MEMORY_HINTS),)
CFLAGS += -DREMOTE_MEMORY
CFLAGS += -DREMOTE_MEMORY_HINTS
endif

ifneq ($(STATS_CORE),)
CFLAGS += -DSTATS_CORE=$(STATS_CORE)
endif

ifneq ($(PROVIDED_CFLAGS),)
CFLAGS += $(PROVIDED_CFLAGS)
endif

ifneq ($(TCP_RX_STATS),)
CFLAGS += -DTCP_RX_STATS
endif

ifneq ($(MLX5),)
CFLAGS += -DMLX5
else
ifneq ($(MLX4),)
CFLAGS += -DMLX4
endif
endif

#
# Dependencies
#

# rdma
RDMA_LIBS=-lrdmacm -libverbs

# dpdk
dpdk = yes
ifeq ($(MAKECMDGOALS),clean)
	dpdk = no
endif
ifeq ($(MAKECMDGOALS),$(FLTRACE))
	dpdk = no
endif
ifeq ($(dpdk),yes)
DPDK_PATH = ${MKFILE_DIR}/dpdk
DPDK_INCLUDES = $(shell cat $(DPDK_PATH)/dpdk_includes)
DPDK_LIBS = $(shell cat $(DPDK_PATH)/dpdk_libs)
ifeq ($(DPDK_LIBS),)
$(error DPDK libs not found. Please run ./setup.sh -d -f)
endif
endif
CFLAGS += $(DPDK_INCLUDES)

# jemalloc
JEMALLOC_PATH = ${MKFILE_DIR}/jemalloc
JEMALLOC_INC = $(shell cat $(JEMALLOC_PATH)/je_includes)
JEMALLOC_LIBS = $(shell cat $(JEMALLOC_PATH)/je_libs)
JEMALLOC_STATIC_LIBS = $(shell cat $(JEMALLOC_PATH)/je_static_libs)
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(JEMALLOC_STATIC_LIBS),)
$(error JEMALLOC libs not found. Please run ./setup.sh -je -f)
endif
endif
CFLAGS += $(JEMALLOC_INC)

# handy for debugging
print-%  : ; @echo $* = $($*)

#
# Shenango libs
#

# libbase.a - the base library
base_src = $(wildcard base/*.c)
base_obj = $(base_src:.c=.o)

#libnet.a - a packet/networking utility library
net_src = $(wildcard net/*.c) $(wildcard net/ixgbe/*.c)
net_obj = $(net_src:.c=.o)

# iokernel - a soft-NIC service
iokernel_src = $(wildcard iokernel/*.c)
iokernel_obj = $(iokernel_src:.c=.o)
iokernel_noht_obj = $(iokernel_src:.c=-noht.o)

# librmem.a - a remote memory library
rmem_src = $(wildcard rmem/*.c)
rmem_obj = $(rmem_src:.c=.o)

# runtime - a user-level threading and networking library
runtime_src = $(wildcard runtime/*.c) $(wildcard runtime/net/*.c)
runtime_asm = $(wildcard runtime/*.S)
runtime_obj = $(runtime_src:.c=.o) $(runtime_asm:.S=.o)

#
# Shenango tools
#

# controller - remote memory controller
rcntrl_src = tools/rmserver/rcntrl.c tools/rmserver/rdma.c
rcntrl_obj = $(rcntrl_src:.c=.o)

# memserver - remote memory server
memserver_src = tools/rmserver/memserver.c tools/rmserver/rdma.c
memserver_obj = $(memserver_src:.c=.o)

# fltrace - fault tracing library
fltrace_src = $(wildcard tools/fltrace/*.c)
fltrace_obj = $(fltrace_src:.c=.o)
ifeq ($(MAKECMDGOALS),$(FLTRACE))
CFLAGS += -fPIC # (fltrace is a shared library)
CFLAGS += -DREMOTE_MEMORY
CFLAGS += -DRMEM_STANDALONE
CFLAGS += -DKEEP_PERTHREAD_DATA
CFLAGS += -DFAULT_SAMPLER
endif

tools_src = $(wildcard tools/*/*.c)
tools_obj = $(tools_src:.c=.o)

#
# Shenango tests
#

test_src = $(wildcard tests/*.c)
test_obj = $(test_src:.c=.o)
test_targets = $(basename $(test_src))

#
# Makefile targets
#

# (must be first target)
all: runtime iok tools $(test_targets)

## libs
runtime: libs 

libs: libbase.a libnet.a librmem.a libruntime.a 

iok: iokerneld iokerneld-noht

libbase.a: $(base_obj)
	$(AR) rcs $@ $^

libnet.a: $(net_obj)
	$(AR) rcs $@ $^

librmem.a: $(rmem_obj)
	$(AR) rcs $@ $^

libruntime.a: $(runtime_obj)
	$(AR) rcs $@ $^

iokerneld: $(iokernel_obj) libbase.a libnet.a base/base.ld
	$(LD) $(LDFLAGS) -o $@ $(iokernel_obj) libbase.a libnet.a $(DPDK_LIBS)	\
		-lpthread -lm -lnuma -ldl

iokerneld-noht: $(iokernel_noht_obj) libbase.a libnet.a base/base.ld
	$(LD) $(LDFLAGS) -o $@ $(iokernel_noht_obj) libbase.a libnet.a 			\
		$(DPDK_LIBS) -lpthread -lm -lnuma -ldl

## tools
tools: rcntrl memserver

rcntrl: $(rcntrl_obj) libbase.a 
	$(LD) $(LDFLAGS) -o $@ $(rcntrl_obj) libbase.a -lpthread -lm $(RDMA_LIBS)

memserver: $(memserver_obj) libbase.a 
	$(LD) $(LDFLAGS) -o $@ $(memserver_obj) libbase.a -lpthread -lm $(RDMA_LIBS)

# fltrace.so has to be built separately as it uses different flags
# use "make fltrace.so"
$(FLTRACE): $(fltrace_obj) librmem.a libbase.a base/base.ld
	$(LD) $(CFLAGS) $(LDFLAGS) -shared $(fltrace_obj) -o $(FLTRACE)	\
		librmem.a libbase.a $(JEMALLOC_STATIC_LIBS) -lpthread -lm -ldl 
#-Wl,-Map=fltrace.map

## tests
$(test_targets): $(test_obj) libbase.a libruntime.a librmem.a libnet.a base/base.ld
	$(LD) $(LDFLAGS) -o $@ $@.o libruntime.a librmem.a libnet.a libbase.a 	\
		-lpthread -lm $(RDMA_LIBS)

## general build rules for all targets
src = $(base_src) $(net_src) $(rmem_src) $(runtime_src) $(iokernel_src) $(test_src) $(tools_src)
asm = $(runtime_asm)
obj = $(src:.c=.o) $(asm:.S=.o) $(iokernel_src:.c=-noht.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

ifneq ($(MAKECMDGOALS),$(FLTRACE))
# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%-noht.d %.d: %.c
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@

%-noht.o: %.c
	$(CC) $(CFLAGS) -Wno-unused-variable -DCORES_NOHT -c $< -o $@
endif

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
%.d: %.S
	@$(CC) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

# prints sparse checker tool output
sparse: $(src)
	$(foreach f,$^,$(SPARSE) $(filter-out -std=gnu11, $(CFLAGS)) $(CHECKFLAGS) $(f);)

.PHONY: clean
clean:
	rm -f $(obj) $(dep) libbase.a libnet.a librmem.a libruntime.a \
	iokerneld iokerneld-noht rcntrl memserver $(FLTRACE) $(test_targets)
