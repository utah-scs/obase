CC=clang++-12
OPT=opt-12

# Active data structure for the KV server. Any of:
#   ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy
#   bpt_seq bpt_occ bpt_mass trie_art
# e.g.  make all DS=ht_pugh
# Switching DS automatically rebuilds the objects that embed the choice.
DS ?= bpt_mass

JEMALLOC_DIR ?= $(HOME)/jemalloc
CFLAGS=-Wall -std=c++17 -I. -Idatastructures -Iruntime -pthread -I$(JEMALLOC_DIR)/include -fPIC -march=native -flto -O2 -MMD -MP -DCREST_DS=$(DS) #-g -O0
LDFLAGS=-pthread -L$(JEMALLOC_DIR)/lib -Wl,-rpath,$(JEMALLOC_DIR)/lib -pie -flto
LIBS=-levent -levent_core -levent_extra -levent_openssl -levent_pthreads -ljemalloc

LLVM_FLAGS=$(shell llvm-config-12 --cxxflags --ldflags --libs)

# Set to 0 to skip the OBASE instrumentation pass (baseline builds):
#   make all ENABLE_LLVM_PASS=0
ENABLE_LLVM_PASS ?= 1

# Set to 0 to skip the validation pass (guide misuse then fails at runtime
# instead of at build time; only for debugging the validator itself):
#   make all ENABLE_VALIDATION=0
ENABLE_VALIDATION ?= 1

# ---------------------------------------------------------------------------
# Layout
#   datastructures/  the ten guided data structures (built via the LLVM pass)
#   runtime/         OBASE runtime: guides, TAG, SODA, SAMA, Object Collector
#   compiler/        visibility extractor (pass 1) and IR instrumentation (pass 3)
# ---------------------------------------------------------------------------

BIN_DIR=bin
OBJ_DIR=$(BIN_DIR)/obj
LLVM_DIR=$(BIN_DIR)/llvm
VISIBILITY_DIR=$(BIN_DIR)/visibility_data

$(shell mkdir -p $(BIN_DIR) $(OBJ_DIR)/runtime $(OBJ_DIR)/datastructures $(LLVM_DIR) $(VISIBILITY_DIR))

# The ten data structures (each goes through: validate -> IR -> visibility ->
# pass -> object). EXTRA_DS adds structures on the command line without
# editing this file (used by the Pass 2 end-to-end test):
#   make all DS=ht_conv EXTRA_DS=ht_conv
ALL_DS=ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy bpt_seq bpt_occ bpt_mass trie_art

# A bare `make all` pushes EVERY structure through the pipeline (keeps them
# all compiling); `make all DS=x` builds only the selected one. The server
# binary always links just $(DS).
ifeq ($(origin DS),command line)
PIPELINE_DS = $(sort $(DS) $(EXTRA_DS))
else
PIPELINE_DS = $(sort $(ALL_DS) $(EXTRA_DS))
endif

DS_OBJ=$(addprefix $(OBJ_DIR)/datastructures/,$(addsuffix .o,$(PIPELINE_DS)))
DS_VIS=$(addprefix $(VISIBILITY_DIR)/,$(addsuffix .cc.visibility,$(PIPELINE_DS)))

# Rebuild-on-switch: the stamp's NAME embeds $(DS), so changing DS on the
# command line invalidates the objects that compile in the selection (make
# cannot otherwise see a -D value change).
DS_STAMP=$(BIN_DIR)/.ds-$(DS).stamp
DS_DEPENDENT_OBJ=$(OBJ_DIR)/ProcessCommand.o $(OBJ_DIR)/crest_server.o $(OBJ_DIR)/runtime/ObjectCollector.o

$(DS_STAMP):
	@rm -f $(BIN_DIR)/.ds-*.stamp
	@touch $@

$(DS_DEPENDENT_OBJ): $(DS_STAMP)

# KV-store + runtime sources (compiled directly, no pass)
SERVER_SRC=crest_server.cc ProcessCommand.cc globalConfig.cc \
	runtime/GuideVoid.cc runtime/TagRuntime.cc \
	runtime/soda.cc runtime/ObjectCollector.cc runtime/Sama.cc
SERVER_OBJ=$(addprefix $(OBJ_DIR)/,$(SERVER_SRC:.cc=.o))

SERVER=$(BIN_DIR)/crest-server
CLIENT=$(BIN_DIR)/crest-cli

VISIBILITY_EXTRACTOR=$(BIN_DIR)/visibility-extractor
GUIDE_PASS=$(LLVM_DIR)/libGuideInstrumentation.so
GUIDE_CONVERTER=$(BIN_DIR)/guide-converter
GUIDE_VALIDATOR=$(BIN_DIR)/guide-validator

CLANG_TOOL_LIBS=-lclangTooling -lclangFrontend -lclangSerialization -lclangDriver -lclangParse -lclangSema -lclangAnalysis -lclangEdit -lclangAST -lclangLex -lclangBasic

all: $(SERVER) $(CLIENT)

# Pass 2 is run on demand (source conversion), not as part of the build.
converter: $(GUIDE_CONVERTER)

test-compiler: $(GUIDE_CONVERTER) $(GUIDE_VALIDATOR) $(VISIBILITY_EXTRACTOR) $(GUIDE_PASS)
	JEMALLOC_DIR="$(JEMALLOC_DIR)" compiler/tests/run_tests.sh

# `all` builds the whole pipeline set; the binary links only the selected
# structure (the other objects are compile checks, not payload).
all: $(DS_OBJ)

$(SERVER): $(SERVER_OBJ) $(OBJ_DIR)/datastructures/$(DS).o
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBS)
	@echo "Built $(SERVER) with DS=$(DS)"

$(CLIENT): $(OBJ_DIR)/crest_client.o
	$(CC) -o $@ $^ $(LDFLAGS)

# --- Compiler tooling -------------------------------------------------------

$(VISIBILITY_EXTRACTOR): compiler/VisibilityExtractor.cc
	$(CC) -std=c++17 -o $@ $< $(LLVM_FLAGS) $(CLANG_TOOL_LIBS)

$(GUIDE_CONVERTER): compiler/GuideConverter.cc
	$(CC) -std=c++17 -o $@ $< $(LLVM_FLAGS) -lclangRewrite $(CLANG_TOOL_LIBS)

$(GUIDE_VALIDATOR): compiler/GuideValidator.cc
	$(CC) -std=c++17 -o $@ $< $(LLVM_FLAGS) $(CLANG_TOOL_LIBS)

$(GUIDE_PASS): compiler/GuideInstrumentationPass.cc
	$(CC) -std=c++17 -shared -fPIC -o $@ $< $(LLVM_FLAGS) -lLLVM

# --- Data-structure pipeline: validate -> .ll -> (pass) -> _modified.ll -> .o

# The validation pass runs before IR emission; a violation (pointer
# arithmetic over managed objects, raw aliasing of guide slots) fails the
# build rather than silently miscompiling.
$(LLVM_DIR)/%.ll: datastructures/%.cc $(GUIDE_VALIDATOR)
	@if [ $(ENABLE_VALIDATION) -eq 1 ]; then \
		echo "Validating $*..."; \
		$(GUIDE_VALIDATOR) $< -- $(CFLAGS) || exit 1; \
	fi
	$(CC) $(CFLAGS) -S -emit-llvm -o $@ $<

$(VISIBILITY_DIR)/%.cc.visibility: datastructures/%.cc $(VISIBILITY_EXTRACTOR)
	$(VISIBILITY_EXTRACTOR) -o $@ $< -- $(CFLAGS)

$(LLVM_DIR)/%_modified.ll: $(LLVM_DIR)/%.ll $(GUIDE_PASS) $(VISIBILITY_DIR)/%.cc.visibility
	@if [ $(ENABLE_LLVM_PASS) -eq 1 ]; then \
		echo "Applying LLVM pass to $*..."; \
		VISIBILITY_DATA_DIR=$(VISIBILITY_DIR) $(OPT) -load $(GUIDE_PASS) -guide-instrumentation -S $< -o $@ > $(LLVM_DIR)/$*_report.txt 2>&1; \
	else \
		echo "Skipping LLVM pass for $*..."; \
		cp $< $@; \
	fi

# Static pattern rule (explicit): guarantees DS objects are built from the
# pass-processed IR, never directly from the .cc via the generic rule below.
# IR input takes no include/depfile flags (clang warns they are unused).
LL_CFLAGS=$(filter-out -I% -MMD -MP,$(CFLAGS))
$(DS_OBJ): $(OBJ_DIR)/datastructures/%.o: $(LLVM_DIR)/%_modified.ll
	$(CC) $(LL_CFLAGS) -c -o $@ $<

# --- Generic compilation ----------------------------------------------------

$(OBJ_DIR)/%.o: %.cc
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BIN_DIR)

# Header dependency tracking (-MMD): edits to any included header rebuild
# the objects that use it. The IR-emission step writes its depfiles next to
# the .ll files, so header edits re-validate and re-emit the structures too.
-include $(SERVER_OBJ:.o=.d) $(OBJ_DIR)/crest_client.d
-include $(addprefix $(LLVM_DIR)/,$(addsuffix .d,$(PIPELINE_DS)))

# Keep the chain intermediates (.ll, .visibility) instead of letting make
# delete them after the build; they are the pipeline's debugging surface.
.SECONDARY: $(DS_VIS) $(addprefix $(LLVM_DIR)/,$(addsuffix .ll,$(PIPELINE_DS)))

.PHONY: all clean converter test-compiler
