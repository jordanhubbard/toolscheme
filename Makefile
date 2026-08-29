CXX ?= c++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra -Wpedantic

.PHONY: all test sanitize clean
all: toolscheme_test
toolscheme_test: toolscheme.cpp toolscheme.hpp test_toolscheme.cpp
	$(CXX) $(CXXFLAGS) toolscheme.cpp test_toolscheme.cpp -o $@
test: toolscheme_test
	./toolscheme_test
sanitize:
	$(CXX) -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined toolscheme.cpp test_toolscheme.cpp -o toolscheme_test_san
	./toolscheme_test_san
clean:
	rm -f toolscheme_test toolscheme_test_san
