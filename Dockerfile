FROM nvcr.io/nvidia/deepstream:5.1-21.02-devel

RUN apt update && apt install -y cmake cmake-curses-gui