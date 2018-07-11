#FROM ubuntu:16.04
FROM registry.cn-shenzhen.aliyuncs.com/xiaozhibo/janus-gateway:base
MAINTAINER phtanus <sysu511@gmail.com>

# Copy installation scripts in
# COPY *.sh ./
# COPY ../janus-gateway ./janus-gateway

# # Prepare the system
# RUN sh ./setup.sh
# # Install dependencies
# RUN sh ./dependencies.sh
# # Install extras
# RUN sh ./extras.sh
# # Install libsrtp 2.0.0 (To reduce risk of broken interoperability with future WebRTC versions)
# RUN sh ./libsrtp.sh
# # Install usrsctp for data channel support
# RUN sh ./usrsctp.sh
# # Install websocket dependencies
# RUN sh ./websockets.sh
# Install protobuf-c dependencies
# RUN sh ./protobuf-c.sh

# Install oss c sdk
# RUN sh ./oss.sh
# Clone, build and install the gateway
RUN sh ./janus.sh
# Put configs in place
COPY conf/*.cfg /opt/janus/etc/janus/

# Declare the ports we use
# EXPOSE 80 7088 8088 8188

# Define the default start-up command
# CMD ./startup.sh
