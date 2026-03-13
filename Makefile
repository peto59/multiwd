TYPE ?= thread

SRC_DIR := src
TEST_DIR := tests
BUILD_DIR := build

LIB_NAME := libmultiwd

TEST_BIN := $(BUILD_DIR)/tests
TEST_O := $(BUILD_DIR)/tests.o

TTY := $(shell tty)

CFLAGS := --std=c23 -O2 -fPIC -D_FORTIFY_SOURCE=3  
CFLAGS_TEST := $(CFLAGS) -DTARGET_TTY=\"$(TTY)\" -g3 -fno-inline -fno-omit-frame-pointer -fprofile-instr-generate -fcoverage-mapping

LDFLAGS := -O2 -rdynamic -D_FORTIFY_SOURCE=3 
LDFLAGS_TEST := $(LDFLAGS) -g3 -fno-inline -fno-omit-frame-pointer -fprofile-instr-generate -fcoverage-mapping

ifeq ($(TYPE),thread)
LDFLAGS_TEST += -fsanitize=thread
endif

ifeq ($(TYPE),address)
LDFLAGS_TEST += -fsanitize=address,undefined,leak \
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
all: $(BUILD_DIR) $(BUILD_DIR)/$(LIB_NAME).so

# Build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile library objects
$(BUILD_DIR)/$(LIB_NAME).so: $(BUILD_DIR) $(SRC_DIR)/multiwd.c
	clang $(CFLAGS) -c $(SRC_DIR)/multiwd.c -o $(BUILD_DIR)/$(LIB_NAME).so

$(BUILD_DIR)/$(LIB_NAME)_sanitizers.so: $(BUILD_DIR) $(SRC_DIR)/multiwd.c
	./clang_run_$(TYPE).sh $(CFLAGS_TEST) -c $(SRC_DIR)/multiwd.c -o $(BUILD_DIR)/$(LIB_NAME)_sanitizers.so


.PHONY: test FORCE

test: $(TEST_BIN)_$(TYPE)
	@echo "Running tests..."
	@if [[ "$(TYPE)" == "thread" ]]; then \
		bash -c 'ulimit -n 65536; \
			export LLVM_PROFILE_FILE="build/%p.profraw"; \
			export TSAN_OPTIONS="die_after_fork=0:second_deadlock_stack=1:stack_trace_limit=256"; \
			while ! ./$(TEST_BIN); do\
				sleep 10; \
				printf "\033[H\033[3J"; \
			done';\
	fi
	@if [[ "$(TYPE)"  == "address" ]]; then \
		ulimit -n 65536; \
		export LLVM_PROFILE_FILE="build/%p.profraw"; \
		./$(TEST_BIN) -j1 --verbose; \
	fi

$(TEST_BIN)_address: FORCE $(BUILD_DIR)/$(LIB_NAME)_sanitizers.so
	@echo "building tests"
	@echo "Creating tty" > $(TTY)
	clang --std=c23 -fPIC -c $(TEST_DIR)/tests.c -o $(TEST_O)
	clang -Lbuild $(TEST_O) $(LDFLAGS_TEST) -lmultiwd_sanitizers -lcriterion -o $(TEST_BIN)

$(TEST_BIN)_thread: FORCE $(BUILD_DIR)/$(LIB_NAME)_sanitizers.so
	@echo "building tests"
	@echo "Creating tty" > $(TTY)
	clang --std=c23 -fPIC -c $(TEST_DIR)/tests_thread.c -o $(TEST_O)
	clang -Lbuild $(TEST_O) $(LDFLAGS_TEST) -lmultiwd_sanitizers -o $(TEST_BIN)

coverage: test
	llvm-profdata merge -sparse -failure-mode=all build/*.profraw -o build/coverage.profdata
	llvm-cov show ./build/tests \
    -instr-profile=build/coverage.profdata \
    -format=html \
    -output-dir=build/coverage
	llvm-cov report ./build/tests \
    -instr-profile=build/coverage.profdata

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test clean coverage
