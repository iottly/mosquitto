# 
# Copyright 2015 Stefano Terna
# 
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
# 
#     http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# 

FROM alpine:3.4
MAINTAINER iottly


RUN \
  apk add --update --virtual .auth-plug-build-dependencies \
    autoconf \
    binutils \
    g++ \
    gcc \
    make \
    curl \
    file \
    libtool \
    openssl-dev \
    util-linux-dev \
    git \
    gdb \
    hiredis-dev \
  && apk add --update --virtual .auth-plug-runtime-dependencies \
    c-ares-dev \
    libuuid


# # INSTALL hiredis C client
# RUN \
#   curl -L  -o hiredis-0.13.3.tar.gz https://github.com/redis/hiredis/archive/v0.13.3.tar.gz \
#   && tar xzf hiredis-0.13.3.tar.gz -C /tmp/ \
#   && cd /tmp/hiredis-0.13.3/ \
#   && make \
#   && make install


ADD . /tmp/mosquitto

#&& git clone https://github.com/iottly/mosquitto.git \

RUN  \
  MOSQUITTO_VERSION="1.4.8" \
    && MOSQUITTO_FILENAME="mosquitto" \
    && cd /tmp \
    && cd /tmp/${MOSQUITTO_FILENAME} \
    && make \
    && adduser -s /bin/false -D -H mosquitto
    
#    && find . -type f | grep Makefile | xargs grep -r -- --strip-program  | awk {'print $1'} | cut -d : -f 1 | xargs sed -i 's/--strip-program=\${CROSS_COMPILE}\${STRIP}//g' \
#    && make install \


## Removing build dependencies, clean temporary files
#RUN \ 
#  apk del .auth-plug-build-dependencies \
#    && rm -rf /var/cache/apk/* /tmp/* /var/tmp/*

ADD mosquitto.conf /mosquitto/config/mosquitto.conf

RUN chown -R mosquitto:mosquitto /mosquitto

EXPOSE 1883

CMD ["/usr/local/sbin/mosquitto", "-c", "/mosquitto/config/mosquitto.conf"]
