CC = gcc

TARGET=raop_play
DESTDIR =

override LDFLAGS := $(LDFLAGS)
override CFLAGS := -Wall $(CFLAGS)

OBJS := raop_play.o raop_client.o rtsp_client.o aexcl_lib.o base64.o aes.o \
audio_stream.o wav_stream.o

all: $(TARGET)

raop_play: $(OBJS)
	$(CC) $(LDFLAGS) -o $@  $^ -lcrypto -lssl -lsamplerate -lid3tag -lpthread

install:
	cp raop_play /usr/local/bin

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(TARGET)

clean:
	rm -f *.o $(TARGET)

distclean:

%.o : %.c
	$(CC) $(CFLAGS) -c $< -o $@

COMMONE_HEADERS := aexcl_lib.h raop_play.h raop_client.h rtsp_client.h

aexcl_lib.o: $(COMMONE_HEADERS)
raop_play.o: $(COMMONE_HEADERS)
raop_client.o: $(COMMONE_HEADERS)
rtsp_client.o: $(COMMONE_HEADERS)
m4a_stream.o: $(COMMONE_HEADERS)
