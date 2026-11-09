CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -Iinclude -Isrc -DDUMP_PCM_PC -D_POSIX_C_SOURCE=200809L -fprofile-arcs -ftest-coverage
LDFLAGS = -lpthread -fprofile-arcs -ftest-coverage -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=queue_pop -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_condattr_init -Wl,--wrap=pthread_condattr_setclock -Wl,--wrap=pthread_cond_init -Wl,--wrap=pthread_cond_timedwait -Wl,--wrap=opendir

SRC = src/logger.c src/queue.c src/session.c src/file_mgr.c src/dump_control.c src/dump_writer.c src/sidecar.c src/dump_pcm_data.c
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/src
TEST_OBJ_DIR = $(BUILD_DIR)/test
WHITEBOX_OBJ_DIR = $(BUILD_DIR)/whitebox
PROD_COV_DIR = $(BUILD_DIR)/coverage/prod
TEST_COV_DIR = $(BUILD_DIR)/coverage/test
WHITEBOX_COV_DIR = $(BUILD_DIR)/coverage/whitebox
OBJ = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC))
TEST_OBJ = $(TEST_OBJ_DIR)/main.o
WHITEBOX_OBJ = $(WHITEBOX_OBJ_DIR)/whitebox.o

TARGET = dump_pcm_test

.PHONY: all clean run test coverage check-contracts check-verification-flow check-header-contract check-docs-build-contract check-shared-library-exports

all: $(TARGET)

$(TARGET): $(OBJ) $(TEST_OBJ) $(WHITEBOX_OBJ)
	$(CC) -o $@ $(OBJ) $(TEST_OBJ) $(WHITEBOX_OBJ) $(LDFLAGS)


$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/%.o: test/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

$(WHITEBOX_OBJ_DIR)/%.o: test/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD_DIR) $(TARGET) *.gcno *.gcda *.gcov src/*.o src/*.gcno src/*.gcda src/*.gcov src/*.gcov test/*.o test/*.gcno test/*.gcda test/*.gcov

check-verification-flow:
	@sh test/check_verification_flow.sh

check-header-contract:
	@sh test/check_header_contract.sh

check-docs-build-contract:
	@sh test/check_docs_build_contract.sh

check-shared-library-exports:
	@sh test/check_shared_library_exports.sh

check-contracts: check-verification-flow check-header-contract check-docs-build-contract check-shared-library-exports

run: check-contracts $(TARGET)
	@mkdir -p /tmp/audio_dump_test
	@AUDIO_DUMP_ENABLED=1 AUDIO_DUMP_DIR=/tmp/audio_dump_test ./$(TARGET)

test: run
	@echo "Test completed"

coverage: check-contracts all
	@echo "Running tests for coverage..."
	@mkdir -p /tmp/audio_dump_test
	@rm -rf $(PROD_COV_DIR) $(TEST_COV_DIR) $(WHITEBOX_COV_DIR)
	@mkdir -p $(PROD_COV_DIR) $(TEST_COV_DIR) $(WHITEBOX_COV_DIR)
	@AUDIO_DUMP_ENABLED=1 AUDIO_DUMP_DIR=/tmp/audio_dump_test ./$(TARGET)
	@echo "Generating coverage report..."
	@true
	@gcov -f -p -o $(OBJ_DIR) src/logger.c src/queue.c src/session.c src/file_mgr.c src/dump_control.c src/dump_writer.c src/sidecar.c src/dump_pcm_data.c || true
	@mv -f *.gcov $(PROD_COV_DIR)/ 2>/dev/null || true
	@mv -f *#.gcov $(PROD_COV_DIR)/ 2>/dev/null || true
	@gcov -f -p -o $(TEST_OBJ_DIR) test/main.c || true
	@mv -f *.gcov $(TEST_COV_DIR)/ 2>/dev/null || true
	@mv -f *#.gcov $(TEST_COV_DIR)/ 2>/dev/null || true
	@gcov -f -p -o $(WHITEBOX_OBJ_DIR) test/whitebox.c || true
	@mv -f *.gcov $(WHITEBOX_COV_DIR)/ 2>/dev/null || true
	@mv -f *#.gcov $(WHITEBOX_COV_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "=== Production Coverage Summary ==="
	@for f in $(PROD_COV_DIR)/*.gcov; do [ -f "$$f" ] && grep -h "^.*:" "$$f"; done || true
	@echo "=== Test Coverage Summary ==="
	@for f in $(TEST_COV_DIR)/*.gcov; do [ -f "$$f" ] && grep -h "^.*:" "$$f"; done || true
	@echo "=== Whitebox Coverage Summary ==="
	@for f in $(WHITEBOX_COV_DIR)/*.gcov; do [ -f "$$f" ] && grep -h "^.*:" "$$f"; done || true
	@echo "========================="
	@exit 0
