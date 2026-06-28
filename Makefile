all: client server

server: sshd.c protocol.c protocol.h
	@mkdir -p bin
	clang -Wall -Wextra -g -O0 protocol.c sshd.c -o bin/sshd 

client: ssh.c protocol.c protocol.h
	@mkdir -p bin
	@clang -Wall -Wextra -g -O0 ssh.c protocol.c -o bin/ssh

run-server: server
	@./bin/sshd

run-client: client
	@./bin/ssh localhost

clean:
	@rm -rf bin
