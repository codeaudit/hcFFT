# Script to clean up test builds  and executables

# Remove locally generated cmakes and makes
rm -rf CMake* Makefile cmake*

# Remove generated executable
rm fft

# Remove autogenerated kernel and shared objects
rm ../../../kernel*.cpp
rm ../../../libFFTKernel*.so
rm /tmp/libFFTKernel*.so