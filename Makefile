CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread -lssl -lcrypto

# Try MacPorts OpenSSL first, then Homebrew, then system
OPENSSL_MP  = /opt/local
OPENSSL_HB  = /opt/homebrew/opt/openssl
OPENSSL_HB2 = /usr/local/opt/openssl

ifneq ($(wildcard $(OPENSSL_MP)/include/openssl/sha.h),)
  CFLAGS  += -I$(OPENSSL_MP)/include
  LDFLAGS += -L$(OPENSSL_MP)/lib
else ifneq ($(wildcard $(OPENSSL_HB)/include/openssl/sha.h),)
  CFLAGS  += -I$(OPENSSL_HB)/include
  LDFLAGS += -L$(OPENSSL_HB)/lib
else ifneq ($(wildcard $(OPENSSL_HB2)/include/openssl/sha.h),)
  CFLAGS  += -I$(OPENSSL_HB2)/include
  LDFLAGS += -L$(OPENSSL_HB2)/lib
endif

all: server

server: server.c document.c auth.c
	$(CC) $(CFLAGS) -o server server.c document.c auth.c $(LDFLAGS)

clean:
	rm -f server *.rte users.db

.PHONY: all clean
