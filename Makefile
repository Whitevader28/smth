# Protocoale de comunicatii
# Laborator 7 - TCP
# Echo Server
# Makefile

# C++ Compiler
CXX = g++

# C++ Flags
CXXFLAGS = -Wall -g -Werror -Wno-error=unused-variable -std=c++17

# Portul pe care asculta serverul
PORT = 12345

# Adresa IP a serverului
IP_SERVER = 192.168.0.2

# Id-ul pe care il va avea clientul
ID_CLIENT = 12

all: server subscriber

common.o: common.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

server_tcp_com.o: server_tcp_com.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Compileaza server.cpp
server: server.cpp common.o server_tcp_com.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm

# Compileaza subscriber.cpp
subscriber: subscriber.cpp common.o server_tcp_com.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lm

.PHONY: clean run_server run_subscriber

# Ruleaza serverul
run_server:
	./server ${PORT}

# Ruleaza subscriberul	
run_subscriber:
	./subscriber ${ID_CLIENT} ${IP_SERVER} ${PORT}

clean:
	rm -rf server subscriber *.o *.dSYM
