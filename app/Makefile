PROG1	= voice
OBJS1	= main.c ACAP.c cJSON.c pipewire_audio.c MQTT.c CERTS.c whisper_stt.c wyoming.c
PROGS	= $(PROG1)
USE_LOCAL_WHISPER ?= 1

PKGS = glib-2.0 gio-2.0 axevent fcgi libcurl libpipewire-0.3

CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))

ifeq ($(USE_LOCAL_WHISPER),1)
CFLAGS += -Iwhisper/include
else
CFLAGS += -DEXTERNAL_ONLY_BUILD -DNO_LOCAL_WHISPER
endif

LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

ifeq ($(USE_LOCAL_WHISPER),1)
LDLIBS += -Lwhisper/lib -lwhisper -lggml -lggml-cpu -lggml-base -lstdc++
endif

LDLIBS  += -s -lm -ldl -lpthread

all:	$(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(PROGS) *.o $(LIBDIR) *.eap* *_LICENSE.txt package.conf* param.conf
