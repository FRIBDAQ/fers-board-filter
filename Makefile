# fersBoardFilter: split the Janus ring into one ring per FERS board.
# Needs an NSCLDAQ 12 installation: source $DAQROOT/daqsetup.bash first.

ifndef DAQROOT
$(error DAQROOT is not set; source the NSCLDAQ daqsetup.bash first)
endif
DAQINC ?= $(DAQROOT)/include
DAQLIB ?= $(DAQROOT)/lib

CXX      ?= g++
CXXFLAGS += -std=c++17 -O2 -g -Wall -Wextra -I$(DAQINC)
LDLIBS    = -L$(DAQLIB) -Wl,-rpath=$(DAQLIB) \
            -ldaqio -ldataformat -lDataFlow -ldaqshm -lurl -lException

all: fersBoardFilter

fersBoardFilter: fersBoardFilter.o BoardRouter.o
	$(CXX) -o $@ $^ $(LDLIBS)

# The router test needs only DataFormat.h, not the NSCLDAQ libraries
test: test_router
	./test_router

test_router: test_router.o BoardRouter.o
	$(CXX) -o $@ $^

fersBoardFilter.o BoardRouter.o test_router.o: BoardRouter.h

clean:
	rm -f fersBoardFilter test_router *.o

.PHONY: all test clean
