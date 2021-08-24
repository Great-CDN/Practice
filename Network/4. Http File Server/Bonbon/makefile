
##############        App makefile        ###############

TARGET      = http_file_server

CFLAGS      = -pipe -W -Wpointer-arith -g -O0 -fPIC -Wreturn-type -Wno-unused-parameter -Wno-deprecated #-Wall -Wextra #-Werror

LDFLAGS     = -Wl,-rpath,'$$ORIGIN/lib:$$ORIGIN/../lib'# -Wl,-pie

libs       := -ldl -lz -lrt -lpthread

src_inc_paths = ./include
src_inc  = $(patsubst %, -I %, $(src_inc_paths))

src_file1 = $(wildcard src/*.c)
src_obj1 = $(patsubst %.c, %.o, $(src_file1))

src_file2 = $(wildcard src/*.cpp)
src_obj2 = $(patsubst %.cpp, %.o, $(src_file2))


.PHONY: all clean

all: ${TARGET}

${TARGET}: $(src_obj1) $(src_obj2)
	$(CXX) -o $@ ${LDFLAGS} $^ ${libs}
    
clean:
	rm -f ${TARGET} ${src_obj1} ${src_obj2} 

	
${src_obj1}: %.o: %.c
	$(CC) -c ${CFLAGS} ${src_inc}   $< -o $@

${src_obj2}: %.o: %.cpp
	$(CXX) -c ${CFLAGS} ${src_inc}   $< -o $@

