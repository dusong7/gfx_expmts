gcc -g -Wfatal-errors -DGLEW_STATIC ^
main.c voxels.c apg_ply.c apg_pixfont.c gl_utils.c input.c camera.c diamond_square.c ^
-I ..\common\include\ -I ..\common\include\stb\ -L ..\common\win64_gcc\ ^
..\common\src\GL\glew.c ..\common\win64_gcc\libglfw3dll.a ^
-lm -lOpenGL32
copy ..\common\win64_gcc\glfw3.dll .\