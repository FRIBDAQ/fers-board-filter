# fersBoardFilter: build the FERS boards' Janus ring items into one event per
# trigger.  Needs an NSCLDAQ 12 installation: source $DAQROOT/daqsetup.bash
# first.

ifneq ($(MAKECMDGOALS),clean)
ifndef DAQROOT
$(error DAQROOT is not set; source the NSCLDAQ daqsetup.bash first)
endif
endif
DAQINC ?= $(DAQROOT)/include
DAQLIB ?= $(DAQROOT)/lib

CXX      ?= g++
CXXFLAGS += -std=c++17 -O2 -g -Wall -Wextra -I$(DAQINC)
LDLIBS    = -L$(DAQLIB) -Wl,-rpath=$(DAQLIB) \
            -ldaqio -ldataformat -lDataFlow -ldaqshm -lurl -lException

all: fersBoardFilter

fersBoardFilter: fersBoardFilter.o EventBuilder.o
	$(CXX) -o $@ $^ $(LDLIBS)

# The builder test needs only DataFormat.h, not the NSCLDAQ libraries
test: test_builder
	./test_builder

test_builder: test_builder.o EventBuilder.o
	$(CXX) -o $@ $^

fersBoardFilter.o EventBuilder.o test_builder.o: EventBuilder.h

clean:
	rm -f fersBoardFilter test_builder *.o

.PHONY: all test clean
