all: client server

server: sshd.c protocol.c protocol.h
	@mkdir -p bin
	clang -Wall -Wextra -g -O0 protocol.c sshd.c -o bin/sshd 

client: ssh.c protocol.c protocol.h
	@mkdir -p bin
	@clang -Wall -Wextra -g -O0 ssh.c protocol.c -o bin/ssh

protocol-test: protocol.c protocol.h
	@mkdir -p bin
	@clang -DMAX_PACKET_SIZE=22 -DMAX_PAYLOAD_SIZE=20 -DINBUF_SIZE=50 -DOUTBUF_SIZE=50\
		-Wall -Wextra -g -O0\
		protocol.c test_protocol.c unity.c\
		-o bin/test

run-server: server
	@./bin/sshd

run-client: client
	@./bin/ssh localhost

run-protocol-test: protocol-test
	@./bin/test

clean:
	@rm -rf bin
