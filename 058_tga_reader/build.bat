gcc -DGLEW_STATIC -D_CRT_SECURE_NO_WARNINGS -Wall -pedantic -g -o demo main_opengl.c ..\common\src\GL\glew.c ..\common\win64_gcc\libglfw3dll.a -I ..\common\include\ -L ..\common\win64_gcc -lglfw3 -lOpenGL32 -lgdi32 -lws2_32 -lpthread 
gcc -std=c89 main.c
