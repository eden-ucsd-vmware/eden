INC     = -I../../inc -I./
CXXFLAGS  = -g -Wall -std=gnu++11 -D_GNU_SOURCE $(INC) -mssse3
LDFLAGS = -T../../base/base.ld
LD	= g++
CC	= g++
AR	= ar

ifneq ($(DEBUG),)
CXXFLAGS += -DDEBUG -DCCAN_LIST_DEBUG -rdynamic -O0 -ggdb
LDFLAGS += -rdynamic
else
CXXFLAGS += -DNDEBUG -O3
endif

CXXFLAGS += -DEDEN_HINTS

# handy for debugging
print-%  : ; @echo $* = $($*)

# librt++.a - the c++ runtime library
rt_src = thread.cc
rt_obj = $(rt_src:.cc=.o)

test_src = test.cc
test_obj = $(test_src:.cc=.o)

# must be first
all: librt++.a test

librt++.a: $(rt_obj)
	$(AR) rcs $@ $^

test: $(test_obj) librt++.a ../../libruntime.a ../../libnet.a ../../libbase.a
	$(LD) $(LDFLAGS) -o $@ $(test_obj) librt++.a ../../libruntime.a \
	../../librmem.a ../../libnet.a ../../libbase.a -lpthread -lrdmacm -libverbs

# general build rules for all targets
src = $(rt_src) $(test_src)
obj = $(src:.cc=.o)
dep = $(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
%.d: %.cc
	@$(CC) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@
%.o: %.cc
	$(CC) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f $(obj) $(dep) librt++.a test
