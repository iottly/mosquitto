include ../config.mk

.PHONY : really clean install

MOSQ_OBJS=custom.o

MOSQ_LIBS=-lpthread

all : libmqtttocore.so

install : all
	$(INSTALL) -d ${DESTDIR}$(prefix)/lib${LIB_SUFFIX}/
	$(INSTALL) -s --strip-program=${CROSS_COMPILE}${STRIP} libmqtttocore.so ${DESTDIR}${prefix}/lib${LIB_SUFFIX}/libmqtttocore.so
	ln -sf libmqtttocore.so ${DESTDIR}${prefix}/lib${LIB_SUFFIX}/libmqtttocore.so
	$(MAKE) -C cpp install

uninstall :
	-rm -f ${DESTDIR}${prefix}/lib${LIB_SUFFIX}/libmqtttocore.so

reallyclean : clean

clean :
	-rm -f *.o libmqtttocore.so

libmqtttocore.so : ${MOSQ_OBJS}
	${CROSS_COMPILE}$(CC) -shared $^ -o $@ ${MOSQ_LIBS}

custom.o : custom.c
	${CROSS_COMPILE}$(CC) $(LIB_CFLAGS) -I../src -DWITH_BROKER -c $< -o $@


