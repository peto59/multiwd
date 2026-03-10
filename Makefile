# Makefile

# Choose compiler via environment variable, default to gcc
TYPE ?= thread

# Source directories
SRC_DIR := src
TEST_DIR := tests
BUILD_DIR := build

LIB_NAME := libmultiwd.so

TEST_BIN := $(BUILD_DIR)/tests
TEST_O := $(BUILD_DIR)/tests.o

TTY := $(shell tty)

CFLAGS := -g3 -O0 -fno-inline -fno-omit-frame-pointer -rdynamic -fPIC -DTARGET_TTY=\"$(TTY)\" -fprofile-instr-generate -fcoverage-mapping
LDFLAGS :=  -rdynamic -fprofile-instr-generate -fcoverage-mapping
ifeq ($(TYPE),thread)
LDFLAGS += -fsanitize=thread
endif

ifeq ($(TYPE),address)
LDFLAGS += -fsanitize=address,undefined,leak \
-fsanitize-address-use-after-scope \
-fsanitize=integer \
-fsanitize=bounds \
-fsanitize=alignment \
-fsanitize=null \
-fsanitize=return \
-fsanitize=shift \
-fsanitize=signed-integer-overflow \
-fsanitize=object-size \
-fsanitize=pointer-overflow \
-fsanitize=vptr \
-fstack-protector-strong
endif

# Default target: build library
all: $(BUILD_DIR) $(BUILD_DIR)/$(LIB_NAME)

# Build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile library objects
$(BUILD_DIR)/$(LIB_NAME): $(SRC_DIR)/multiwd.c
	./clang_run_$(TYPE).sh $(CFLAGS) -c $(SRC_DIR)/multiwd.c -o $(BUILD_DIR)/$(LIB_NAME)

# ----------------------------
# Test target
# ----------------------------
# Default: build library first

.PHONY: test FORCE

test: all $(TEST_BIN)
	@echo "Running tests..."
	#@if [[ "$(TYPE)" == "thread" ]]; then \
	#	bash -c 'ulimit -n 65536; \
	#		export LLVM_PROFILE_FILE="build/%p.profraw"; \
	#		while ! ./$(TEST_BIN) -j1 --always-succeed --verbose; do\
	#			printf "\033[H\033[3J"; \
	#		done';\
	#fi
	#@if [[ "$(TYPE)"  == "address" ]]; then \
		ulimit -n 65536; \
		export LLVM_PROFILE_FILE="build/%p.profraw"; \
		./$(TEST_BIN) -j1 --verbose; \
	#fi

# Compile test binary
$(TEST_BIN): FORCE
	@echo "building tests"
	@echo "Creating tty" > $(TTY)
	clang --std=c23 -fPIC -c $(TEST_DIR)/tests.c -o $(TEST_O)
	#clang -g -Iinclude $(BUILD_DIR)/multiwd.so -L. $(TEST_O) $(LDFLAGS) -lcriterion -o $(TEST_BIN)
	clang -Lbuild $(TEST_O) $(LDFLAGS) -lmultiwd -lcriterion -o $(TEST_BIN)

coverage: test
	llvm-profdata merge -sparse build/*.profraw -o build/coverage.profdata
	#llvm-cov show ./build/tests \
    #-instr-profile=build/coverage.profdata \
    #-format=html \
    #-output-dir=build/coverage
	llvm-cov report ./build/tests \
    -instr-profile=build/coverage.profdata

# Clean build
clean:
	rm -rf $(BUILD_DIR)
	#/*.o $(BUILD_DIR)/*.so $(BUILD_DIR)/*.a $(BUILD_DIR)/$(LIB_NAME) $(TEST_BIN)

.PHONY: all test clean coverage
