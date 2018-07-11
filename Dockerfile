#FROM ubuntu:16.04
FROM registry.cn-shenzhen.aliyuncs.com/xiaozhibo/janus-docker:base
MAINTAINER phtanus <sysu511@gmail.com>


# Copy installation scripts in
COPY docker-script/*.sh ./
COPY . ./janus-gateway

# build and install the gateway
RUN sh ./janus.sh
# Put configs in place
COPY docker-script/conf/*.cfg /opt/janus/etc/janus/

# Declare the ports we use
# EXPOSE 80 7088 8088 8188

# Define the default start-up command
# CMD ./startup.sh
