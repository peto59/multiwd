TYPE ?= address

SRC_DIR = src
INCLUDE_DIR = include
TEST_DIR = tests
BUILD_DIR = build

LIB_SRC = $(SRC_DIR)/multiwd.c
LIB_O = $(BUILD_DIR)/multiwd.o
LIB_NAME = $(BUILD_DIR)/libmultiwd.so
LIB_NAME_SANITIZERS = $(BUILD_DIR)/libmultiwd_sanitizers.so

TEST_SRC = $(TEST_DIR)/tests_$(TYPE).c
TEST_O = $(BUILD_DIR)/tests.o
TEST_BIN = $(BUILD_DIR)/tests

TTY = $(shell tty)

CC = clang
CFLAGS = --std=c23 -O2 -fPIC -D_FORTIFY_SOURCE=3 -I$(INCLUDE_DIR) 
CFLAGS_TEST = $(CFLAGS) -DMULTIWD_DEBUG=\"1\" -DTARGET_TTY=\"$(TTY)\" -g3 -fno-inline -fno-omit-frame-pointer -fprofile-instr-generate -fcoverage-mapping

LDFLAGS = -O2 -rdynamic -D_FORTIFY_SOURCE=3 
LDFLAGS_TEST = $(LDFLAGS) -g3 -fno-inline -fno-omit-frame-pointer -fprofile-instr-generate -fcoverage-mapping

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


.PHONY: $(BUILD_DIR) test FORCE all clean coverage
all: $(BUILD_DIR) $(LIB_NAME)
FORCE:

$(BUILD_DIR): FORCE
	mkdir -p $(BUILD_DIR)

$(LIB_O): $(LIB_SRC)
	$(CC) $(CFLAGS) -c $^ -o $@

$(LIB_NAME): $(LIB_O)
	clang $(CFLAGS) --shared $^ -o $@

$(LIB_NAME_SANITIZERS): $(LIB_SRC)
	./clang_run_$(TYPE).sh $(CFLAGS_TEST) -c $^ -o $@



test: $(BUILD_DIR) $(TEST_BIN)
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

$(TEST_BIN): FORCE $(LIB_NAME_SANITIZERS)
	@echo "building tests"
	@echo "Creating tty" > $(TTY)
	clang $(CFLAGS) -c $(TEST_SRC) -o $(TEST_O)
	clang -L$(BUILD_DIR) $(TEST_O) $(LDFLAGS_TEST) -lmultiwd_sanitizers -Wl,-rpath,'./build' -lcriterion -o $(TEST_BIN)

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

