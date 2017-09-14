.PHONY: all clean run

CC=gcc
FLAGS=-O3 -Wall -Werror
OUT=socket
IN=socket.c

all: $(OUT)

$(OUT): $(IN)
	$(CC) $(FLAGS) $(IN) -o $(OUT)

run: $(OUT)
	# check difference with -v mode and debug with strace
	./socket 2742200 # Works
	./socket 2742201 # doesn't work, but it'll with -i mode

clean:
	rm -f $(OUT)
