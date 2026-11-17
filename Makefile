CHECKPOINT := third_party/Checkpoint
PICOC      := $(CHECKPOINT)/3rd-party/picoc
SCRIPT     := scripts/savesync.c
BUILD      := build
OBJ        := $(BUILD)/obj
HOSTRUN    := $(BUILD)/hostrun

.PHONY: lint test hostrun clean

# Parse-check the script with Checkpoint's own picoc parser.
lint:
	$(CHECKPOINT)/tools/scriptlint.sh $(SCRIPT)

test: hostrun lint
	pytest tests -q

# ---- hostrun: Checkpoint's interpreter and bindings on the PC --------------
# Built the way switch/Makefile builds them: picoc's interpreter core without
# its debugger, Checkpoint's platform layer, stdlib and bindings, with the same
# picoc flags. tools/hostrun supplies the fake console and the platform shims.

HOST_C   := $(sort $(filter-out %/debug.c,$(wildcard $(PICOC)/source/interpreter/*.c))) \
            $(sort $(wildcard $(CHECKPOINT)/common/script/cstdlib/*.c)) \
            $(addprefix $(CHECKPOINT)/common/script/,platform_checkpoint.c library_checkpoint.c scriptabort.c)
HOST_CXX := $(addprefix $(CHECKPOINT)/common/script/,checkpoint_api.cpp json_api.cpp zip_api.cpp web_api.cpp \
              httpcall.cpp seal_api.cpp progress_api.cpp scriptargs.cpp scriptheap.cpp scriptconsole.cpp \
              scriptbar.cpp scriptuibridge.cpp scriptengine.cpp scriptrunner.cpp) \
            $(addprefix $(CHECKPOINT)/common/,common.cpp transferprotocol.cpp) \
            $(sort $(wildcard tools/hostrun/*.cpp tools/hostrun/shim/*.cpp))
HOST_OBJS := $(patsubst %,$(OBJ)/%.o,$(HOST_C) $(HOST_CXX))

# The shim folder comes first: it stands in for switch/include's paths.hpp and
# thread.hpp. common/script comes before picoc's include so its platform.h wins.
# mbedtls 4 dropped the gcm/md/pkcs5 API seal_api.cpp uses (the console builds
# against devkitPro's mbedtls 2.28), so link Arch's mbedtls3 from its own folder.
MBEDTLS_INC   := /usr/include/mbedtls3
MBEDTLS_LIB   := /usr/lib/mbedtls3
HOST_INC      := -Itools/hostrun/shim -I$(CHECKPOINT)/common -I$(CHECKPOINT)/common/script \
                 -I$(CHECKPOINT)/3rd-party/json -I$(PICOC)/include -I$(MBEDTLS_INC)
HOST_FLAGS    := -O2 -g -MMD -MP -funwind-tables -DUNIX_HOST -DDEBUGGER -D_GNU_SOURCE=1 $(HOST_INC)
HOST_CXXFLAGS := $(HOST_FLAGS) -std=gnu++23 -fno-rtti

# Taps for the full transcript and the status list (tools/hostrun/capture.cpp),
# and for the faults a test can switch on (tools/hostrun/faults.cpp).
HOST_WRAP := -Wl,--wrap=ckpt_console_write \
             -Wl,--wrap=_ZN13ScriptConsole3logERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE \
             -Wl,--wrap=_ZN14ScriptUiBridge9setStatusENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE \
             -Wl,--wrap=_ZN13ScriptConsole7beginIoENSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEx \
             -Wl,--wrap=_ZN13ScriptConsole5addIoEx \
             -Wl,--wrap=fopen -Wl,--wrap=fclose
HOST_LIBS := -lcurl -L$(MBEDTLS_LIB) -Wl,-rpath,$(MBEDTLS_LIB) -lmbedcrypto -lpthread -lm

hostrun: $(HOSTRUN)

$(HOSTRUN): $(HOST_OBJS)
	$(CXX) -o $@ $^ $(HOST_WRAP) $(HOST_LIBS)

# Checkpoint's sources: its own code, built quietly.
$(OBJ)/third_party/%.c.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(HOST_FLAGS) -w -c $< -o $@

$(OBJ)/third_party/%.cpp.o: third_party/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -w $(EXTRA) -c $< -o $@

# glibc has no funopen; this one file needs it.
$(OBJ)/$(CHECKPOINT)/common/script/scriptconsole.cpp.o: EXTRA := -include tools/hostrun/shim/funopen.h

# hostrun's own sources, with warnings.
$(OBJ)/tools/%.cpp.o: tools/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(HOST_CXXFLAGS) -Wall -Wextra -c $< -o $@

-include $(HOST_OBJS:.o=.d)

clean:
	rm -rf $(BUILD)
