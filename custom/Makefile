include ../config.mk

.PHONY : really clean install

MOSQ_CFLAGS=-DWITH_BROKER -DWITH_BRIDGE
MOSQ_OBJS=custom.o http.o

MOSQ_LIBS=-lpthread

all : posthttp.so

install : all
	$(INSTALL) -d ${DESTDIR}$(prefix)/lib${LIB_SUFFIX}/
	$(INSTALL) -s --strip-program=${CROSS_COMPILE}${STRIP} posthttp.so ${DESTDIR}${prefix}/lib${LIB_SUFFIX}/posthttp.so
	#ln -sf posthttp.so ${DESTDIR}${prefix}/lib${LIB_SUFFIX}/posthttp.so
	#$(MAKE) -C cpp install

uninstall :
	-rm -f ${DESTDIR}${prefix}/lib${LIB_SUFFIX}/posthttp.so

reallyclean : clean

clean :
	-rm -f *.o posthttp.so

posthttp.so : ${MOSQ_OBJS}
	${CROSS_COMPILE}$(CC) -shared $^ -o $@ ${MOSQ_LIBS}

custom.o : custom.c http.h
	${CROSS_COMPILE}$(CC) $(LIB_CFLAGS) -I../src $(MOSQ_CFLAGS) -c $< -o $@

http.o : http.c http.h
	${CROSS_COMPILE}$(CC) $(LIB_CFLAGS) -I../src $(MOSQ_CFLAGS) -c $< -o $@

