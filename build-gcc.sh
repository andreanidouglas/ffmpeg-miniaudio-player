
# Tested with Ubuntu 24.04
gcc -Wall -Wextra -Werror -g -o miniaudio -I/usr/include/x86_64-linux-gnu/ -L/usr/lib/x86_64-linux-gnu/ audio.c -lm -lavcodec -lavutil -lswresample -ldl -lpthread

